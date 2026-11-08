/*
 * POSIX shared-memory transport provider — iceoryx-style zero-copy.
 *
 * 一个 channel 对应一个命名共享内存段（see shm_segment.c）：段内按 size class
 * 分档管理 chunk，请求/响应用无锁 MPMC 队列传递“段内偏移”。消息不再拷进固定
 * slot：
 *   - 发送方从段里借一块 chunk，把编码后的 v2 envelope 直接写进去，入队偏移；
 *   - 接收方线程弹出偏移，把 chunk 包成 external nb_buf 作为 frame_owner 交给
 *     core，payload 直接指向段内内存——大包跨线程零拷贝，小包由 core 决定拷贝；
 *   - 服务端回复时按请求带来的 client id 借新 chunk 写响应，投递到该 client 的
 *     响应队列。
 *
 * 多节点：endpoint 端口是段内唯一节点 id（1..9）。每个节点都有独立入站队列，
 * 任意节点都可以 send/reply；mesh 发现广播完整 shm URI，因此不再依赖固定
 * server/client 角色。
 *
 * 发布/订阅：KIND_EVENT 走段内订阅表扇出——service 注册方法时把 route
 * （"service.method"）登记到本节点槽位，事件按 route 零拷贝投递给所有订阅者
 * （发布者自身除外）。RPC、响应与 event 统一进入目标节点的入站队列，由 core
 * 按 kind 分发。
 */
#include "transport_internal.h"

#include <nb/shm.h>

#include "atomic.h"
#include <nb/endpoint.h>
#include "envelope.h"
#include "shm_segment.h"
#include "util.h"

#define NB_LOG_TAG "shm"
#include <nb/log.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__linux__)

#include <pthread.h>

#define NB_SHM_WAIT_MS 50

typedef struct nb_shm_instance {
    nb_transport_provider_options_t options;
    char channel[NB_HOST_MAX];
    uint16_t self_id;
    nb_shmseg_t *seg;
    pthread_t thread;
    uint32_t running;
    int thread_started;
    /* 0 = 阻塞等待（sem_timedwait）；N = 空闲前先自旋 N 微秒再阻塞。 */
    uint32_t poll_us;
} nb_shm_instance_t;

/* 用指针宽度承载回程 client id：入站时放 reply_token，回复时取回。 */
static void *nb_shm_token(uint16_t client_id)
{
    return (void *)(uintptr_t)client_id;
}

static uint16_t nb_shm_token_id(void *token)
{
    return (uint16_t)(uintptr_t)token;
}

static void nb_shm_fill_envelope(const nb_transport_message_t *message, nb_envelope_t *env)
{
    memset(env, 0, sizeof(*env));
    env->kind = message->kind;
    env->encoding = message->payload.encoding;
    env->flags = message->flags;
    env->status = message->status;
    env->request_id = message->request_id;
    env->timeout_ms = message->timeout_ms;
    env->route = message->route;
    env->metadata = message->metadata;
    env->metadata_len = message->metadata_len;
    env->status_msg = message->status_msg;
    env->status_msg_len = message->status_msg_len;
    env->payload = message->payload.data;
    env->payload_len = message->payload.len;
    if (message->has_stream) {
        env->stream_flags = (uint8_t)message->stream.flags;
        env->stream_id = message->stream.stream_id;
        env->stream_offset = message->stream.offset;
        env->stream_total_size = message->stream.total_size;
    }
}

/* 借 chunk → 编码 envelope，返回 chunk 偏移。失败时已回滚，无所有权泄漏。 */
static int nb_shm_encode(nb_shm_instance_t *inst, const nb_transport_message_t *message,
                         uint64_t *off_out)
{
    nb_envelope_t env;
    uint64_t off;
    uint8_t *data;
    uint32_t cap;
    size_t need;
    size_t written;

    if (!inst || !inst->seg || !message || !off_out) {
        return NB_ERR_INVALID;
    }
    nb_shm_fill_envelope(message, &env);
    need = nb_envelope_size(&env);
    if (need == 0) {
        return NB_ERR_INVALID;
    }
    /* 最大档都放不下才是 TOOBIG；否则失败只是暂时无空闲 chunk（AGAIN）。 */
    if (need > nb_shm_class_bytes[NB_SHM_CLASS_COUNT - 1u]) {
        return NB_ERR_TOOBIG;
    }
    off = nb_shmseg_alloc(inst->seg, need);
    if (off == 0) {
        return NB_ERR_AGAIN;
    }
    data = nb_shmseg_data(inst->seg, off);
    cap = nb_shmseg_capacity(inst->seg, off);
    if (!data || need > cap) {
        nb_shmseg_chunk_free(inst->seg, off);
        return NB_ERR_TOOBIG;
    }
    written = nb_envelope_encode(data, cap, &env);
    if (written != need) {
        nb_shmseg_chunk_free(inst->seg, off);
        return NB_ERR_IO;
    }
    nb_shmseg_set_length(inst->seg, off, (uint32_t)written);
    *off_out = off;
    return NB_OK;
}

/* 编码后投递到目标节点的统一入站队列。 */
static int nb_shm_publish_queue(nb_shm_instance_t *inst,
                                uint16_t target_id,
                                const nb_transport_message_t *message)
{
    uint64_t off = 0;
    int rc = nb_shm_encode(inst, message, &off);

    if (rc != NB_OK) {
        return rc;
    }
    if (nb_shmseg_push(inst->seg, target_id, off) != 0) {
        nb_shmseg_chunk_free(inst->seg, off);
        return NB_ERR_AGAIN;
    }
    return NB_OK;
}

/* 事件：编码后按 route（service.method）零拷贝扇出给所有订阅者。 */
static int nb_shm_publish_event(nb_shm_instance_t *inst, const nb_transport_message_t *message)
{
    uint64_t off = 0;
    int rc;

    if (!message->route || !message->route[0]) {
        return NB_ERR_INVALID;
    }
    rc = nb_shm_encode(inst, message, &off);
    if (rc != NB_OK) {
        return rc;
    }
    /* publish 接管 chunk 所有权（无人订阅或部分队列满都会内部回收）。 */
    if (nb_shmseg_publish(inst->seg, message->route, off) != 0) {
        return NB_ERR_IO;
    }
    return NB_OK;
}

/* 弹出 chunk → 解码 → 以 frame_owner 形式交给 core（零拷贝）。 */
static void nb_shm_deliver(nb_shm_instance_t *inst, uint64_t off, uint16_t source_id,
                           void *reply_token)
{
    nb_envelope_t env;
    nb_transport_message_t msg;
    nb_endpoint_t peer;
    nb_buf_t *frame;
    char route_buf[NB_GLOBAL_MAX_ROUTE_BYTES];
    uint8_t *data;
    uint32_t len;

    data = nb_shmseg_data(inst->seg, off);
    len = nb_shmseg_length(inst->seg, off);
    if (!data || nb_envelope_decode(data, len, &env) != NB_OK) {
        nb_shmseg_chunk_free(inst->seg, off);
        return;
    }
    frame = nb_shmseg_wrap_rx(inst->seg, off);
    if (!frame) {
        nb_shmseg_chunk_free(inst->seg, off);
        return;
    }
    if (!inst->options.on_receive) {
        nb_buf_release(frame); /* 释放即归还 chunk */
        return;
    }

    memset(&msg, 0, sizeof(msg));
    msg.kind = env.kind;
    msg.flags = env.flags;
    msg.status = env.status;
    msg.request_id = env.request_id;
    msg.timeout_ms = env.timeout_ms;
    msg.metadata = env.metadata;
    msg.metadata_len = env.metadata_len;
    msg.status_msg = env.status_msg;
    msg.status_msg_len = env.status_msg_len;
    if (env.route && env.route_len < sizeof(route_buf)) {
        memcpy(route_buf, env.route, env.route_len);
        route_buf[env.route_len] = '\0';
        msg.route = route_buf;
    }
    msg.payload.data = env.payload;
    msg.payload.len = env.payload_len;
    msg.payload.encoding = env.encoding;
    if (env.stream_flags != 0 || env.stream_id != 0) {
        msg.has_stream = 1;
        msg.stream.flags = env.stream_flags;
        msg.stream.stream_id = env.stream_id;
        msg.stream.offset = env.stream_offset;
        msg.stream.total_size = env.stream_total_size;
    }
    msg.frame_owner = frame;
    if (nb_endpoint_from_parts(&peer,
                               "shm",
                               inst->channel,
                               source_id,
                               nb_transport_kind_from_scheme("shm")) == NB_OK) {
        msg.peer = &peer;
        msg.peer_host = peer.host;
        msg.peer_port = peer.port;
    }
    inst->options.on_receive(inst->options.user, reply_token, &msg);
}

static uint64_t nb_shm_now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void nb_shm_drain(nb_shm_instance_t *inst)
{
    for (;;) {
        uint64_t off;
        uint16_t source = 0;

        if (nb_shmseg_pop(inst->seg, &off, &source) != 0) {
            break;
        }
        nb_shm_deliver(inst, off, source, nb_shm_token(source));
    }
}

static void *nb_shm_thread_main(void *arg)
{
    nb_shm_instance_t *inst = (nb_shm_instance_t *)arg;

    while (nb_atomic_load_acquire_u32(&inst->running) != 0u) {
        if (inst->poll_us != 0u) {
            /* 混合模式：有数据继续排空；空闲超过 poll_us 后退回阻塞等待。 */
            uint64_t idle_until = nb_shm_now_us() + inst->poll_us;

            for (;;) {
                uint64_t off;
                uint16_t source = 0;

                if (nb_shmseg_pop(inst->seg, &off, &source) == 0) {
                    nb_shm_deliver(inst, off, source, nb_shm_token(source));
                    idle_until = nb_shm_now_us() + inst->poll_us;
                    continue;
                }
                if (nb_shm_now_us() >= idle_until) {
                    break;
                }
#if defined(__x86_64__) || defined(__i386__)
                __builtin_ia32_pause();
#endif
            }
        }
        (void)nb_shmseg_wait(inst->seg, NB_SHM_WAIT_MS);
        nb_shm_drain(inst);
    }
    return NULL;
}

static int nb_shm_op_create(const nb_transport_provider_options_t *options,
                            nb_transport_instance_t **out)
{
    nb_shm_instance_t *inst;
    nb_endpoint_t endpoint;

    if (!options || !out || !options->endpoint) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    if (nb_endpoint_parse(options->endpoint, &endpoint) != NB_OK ||
        strcmp(endpoint.scheme, "shm") != 0 || endpoint.port == 0) {
        return NB_ERR_INVALID;
    }
    if (endpoint.host[0] == '\0') {
        return NB_ERR_INVALID;
    }
    inst = (nb_shm_instance_t *)calloc(1, sizeof(*inst));
    if (!inst) {
        return NB_ERR_NOMEM;
    }
    inst->options = *options;
    inst->options.ps = NULL;
    inst->self_id = endpoint.port;
    nb_copy_str(inst->channel, sizeof(inst->channel), endpoint.host, "");
    inst->poll_us = options->poll_us;
    {
        /* Env var keeps overriding the binding config for ops/benchmark tuning. */
        const char *poll_env = getenv("NB_SHM_POLL_US");
        if (poll_env && poll_env[0]) {
            inst->poll_us = (uint32_t)strtoul(poll_env, NULL, 10);
        }
    }
    inst->seg = nb_shmseg_open(inst->channel, inst->self_id);
    if (!inst->seg) {
        free(inst);
        return NB_ERR_IO;
    }
    *out = (nb_transport_instance_t *)inst;
    return NB_OK;
}

static int nb_shm_op_start(nb_transport_instance_t *instance)
{
    nb_shm_instance_t *inst = (nb_shm_instance_t *)instance;
    int rc;

    if (!inst || !inst->seg) {
        return NB_ERR_INVALID;
    }
    if (inst->thread_started) {
        return NB_OK;
    }
    nb_atomic_store_release_u32(&inst->running, 1u);
    rc = pthread_create(&inst->thread, NULL, nb_shm_thread_main, inst);
    if (rc != 0) {
        nb_atomic_store_release_u32(&inst->running, 0u);
        return NB_ERR_NOMEM;
    }
    inst->thread_started = 1;
    return NB_OK;
}

static void nb_shm_op_stop(nb_transport_instance_t *instance)
{
    nb_shm_instance_t *inst = (nb_shm_instance_t *)instance;

    if (!inst || !inst->thread_started) {
        return;
    }
    nb_atomic_store_release_u32(&inst->running, 0u);
    nb_shmseg_wake(inst->seg);
    pthread_join(inst->thread, NULL);
    inst->thread_started = 0;
}

static void nb_shm_op_destroy(nb_transport_instance_t *instance)
{
    nb_shm_instance_t *inst = (nb_shm_instance_t *)instance;

    if (!inst) {
        return;
    }
    nb_shm_op_stop(instance);
    nb_shmseg_close(inst->seg);
    free(inst);
}

static int nb_shm_op_send(nb_transport_instance_t *instance,
                          const nb_endpoint_t *target,
                          const nb_transport_message_t *message)
{
    nb_shm_instance_t *inst = (nb_shm_instance_t *)instance;

    if (!inst || !message || !target) {
        return NB_ERR_INVALID;
    }
    if (strcmp(target->host, inst->channel) != 0) {
        return NB_ERR_NOTFOUND;
    }
    /* 事件走发布/订阅扇出：不区分 server/client 角色，也不看目标端口。 */
    if (message->kind == KIND_EVENT) {
        return nb_shm_publish_event(inst, message);
    }
    if (target->port == 0 || target->port > NB_SHM_MAX_NODES) {
        return NB_ERR_INVALID;
    }
    return nb_shm_publish_queue(inst, target->port, message);
}

static int nb_shm_op_reply(nb_transport_instance_t *instance,
                           void *reply_token,
                           const nb_endpoint_t *peer,
                           const nb_transport_message_t *message)
{
    nb_shm_instance_t *inst = (nb_shm_instance_t *)instance;
    uint16_t target_id;
    (void)peer;

    if (!inst || !message) {
        return NB_ERR_INVALID;
    }
    target_id = nb_shm_token_id(reply_token);
    if (target_id == 0 || target_id > NB_SHM_MAX_NODES) {
        return NB_ERR_INVALID;
    }
    return nb_shm_publish_queue(inst, target_id, message);
}

/*
 * Registering a service method means "this node wants inbound messages for that
 * route". RPC requests are queued based on server/client role and need no table;
 * event publish relies on this subscription table for fan-out.
 */
static int nb_shm_op_register_method(nb_transport_instance_t *instance,
                                     const char *service,
                                     const char *method)
{
    nb_shm_instance_t *inst = (nb_shm_instance_t *)instance;
    char topic[NB_GLOBAL_MAX_ROUTE_BYTES];

    if (!inst || !inst->seg || !service || !method) {
        return NB_ERR_INVALID;
    }
    if (nb_route_format(topic, sizeof(topic), service, method) != NB_OK) {
        return NB_ERR_INVALID;
    }
    return nb_shmseg_subscribe(inst->seg, topic) == 0 ? NB_OK : NB_ERR_NOMEM;
}

static void nb_shm_op_unregister_method(nb_transport_instance_t *instance,
                                        const char *service,
                                        const char *method)
{
    nb_shm_instance_t *inst = (nb_shm_instance_t *)instance;
    char topic[NB_GLOBAL_MAX_ROUTE_BYTES];

    if (!inst || !inst->seg || !service || !method) {
        return;
    }
    if (nb_route_format(topic, sizeof(topic), service, method) != NB_OK) {
        return;
    }
    (void)nb_shmseg_unsubscribe(inst->seg, topic);
}

/* loan 路径只承载 EVENT 发布：chunk 前缀固定为固定头 + route TLV。 */
static size_t nb_shm_loan_prefix_bytes(const char *route)
{
    size_t route_len;

    if (!route || !route[0]) {
        return 0;
    }
    route_len = strlen(route);
    if (route_len > 0xffffu) {
        return 0;
    }
    return (size_t)ENVELOPE_HDR_LEN + NB_EXT_TLV_HDR_LEN + route_len;
}

/*
 * 借出段内一块 chunk，把数据区按 envelope 前缀让出一段，剩余容量交给应用直写。
 * loan 句柄就是 chunk 的段内偏移：无需额外分配，且与 chunk 生命周期一致。
 */
static int nb_shm_op_loan(nb_transport_instance_t *instance,
                          const char *route,
                          uint32_t payload_capacity,
                          void **payload_data,
                          uint32_t *payload_capacity_out,
                          void **loan)
{
    nb_shm_instance_t *inst = (nb_shm_instance_t *)instance;
    uint64_t off;
    uint8_t *data;
    uint32_t cap;
    size_t hdr;

    if (!inst || !inst->seg || !payload_data || !payload_capacity_out || !loan) {
        return NB_ERR_INVALID;
    }
    hdr = nb_shm_loan_prefix_bytes(route);
    if (hdr == 0 || payload_capacity > UINT32_MAX - hdr) {
        return NB_ERR_INVALID;
    }
    off = nb_shmseg_alloc(inst->seg, hdr + payload_capacity);
    if (off == 0) {
        return NB_ERR_AGAIN;
    }
    data = nb_shmseg_data(inst->seg, off);
    cap = nb_shmseg_capacity(inst->seg, off);
    if (!data || (size_t)cap < hdr + payload_capacity) {
        nb_shmseg_chunk_free(inst->seg, off);
        return NB_ERR_TOOBIG;
    }
    *payload_data = data + hdr;
    *payload_capacity_out = cap - (uint32_t)hdr;
    *loan = (void *)(uintptr_t)off;
    return NB_OK;
}

/*
 * 补齐 envelope 前缀并以 EVENT 扇出。无论成败都接管 loan 所有权；只接受与
 * 借出时同一 route、无 metadata/status/stream 的消息（否则前缀长度会漂移，
 * 把 payload 覆盖掉）。
 */
static int nb_shm_op_send_loan(nb_transport_instance_t *instance,
                               const char *route,
                               void *loan,
                               const nb_transport_message_t *message)
{
    nb_shm_instance_t *inst = (nb_shm_instance_t *)instance;
    uint64_t off = (uint64_t)(uintptr_t)loan;
    nb_envelope_t env;
    uint8_t *data;
    uint32_t cap;
    size_t hdr;
    size_t written;

    if (!inst || !inst->seg || !route || !message || off == 0) {
        return NB_ERR_INVALID;
    }
    hdr = nb_shm_loan_prefix_bytes(route);
    data = nb_shmseg_data(inst->seg, off);
    cap = nb_shmseg_capacity(inst->seg, off);
    if (hdr == 0 || !data || (size_t)cap < hdr || !message->route ||
        strcmp(message->route, route) != 0 || message->kind != KIND_EVENT ||
        message->metadata_len != 0 || message->status_msg_len != 0 ||
        message->status != 0 || message->has_stream ||
        message->payload.data != data + hdr ||
        message->payload.len > (size_t)cap - hdr) {
        nb_shmseg_chunk_free(inst->seg, off);
        return NB_ERR_INVALID;
    }
    nb_shm_fill_envelope(message, &env);
    written = nb_envelope_encode_prefix(data, cap, &env);
    if (written != hdr) {
        nb_shmseg_chunk_free(inst->seg, off);
        return NB_ERR_IO;
    }
    nb_shmseg_set_length(inst->seg, off, (uint32_t)written + message->payload.len);
    /* publish 接管 chunk 所有权（无人订阅或部分队列满都会内部回收）。 */
    if (nb_shmseg_publish(inst->seg, route, off) != 0) {
        return NB_ERR_IO;
    }
    return NB_OK;
}

static void nb_shm_op_release_loan(nb_transport_instance_t *instance, void *loan)
{
    nb_shm_instance_t *inst = (nb_shm_instance_t *)instance;
    uint64_t off = (uint64_t)(uintptr_t)loan;

    if (!inst || !inst->seg || off == 0) {
        return;
    }
    nb_shmseg_chunk_free(inst->seg, off);
}

/* 原生主题：读者把主题名原样登记为订阅键；写者无需登记，事件发布按 route
 * （主题名）在段内扇出。主题名不含 '.'，因此不会与 RPC 的 "service.method"
 * 路由混淆。 */
static int nb_shm_op_register_topic(nb_transport_instance_t *instance,
                                    const char *topic,
                                    uint32_t roles)
{
    nb_shm_instance_t *inst = (nb_shm_instance_t *)instance;

    if (!inst || !inst->seg || !topic || !topic[0]) {
        return NB_ERR_INVALID;
    }
    if (roles & NB_PS_TOPIC_READER) {
        return nb_shmseg_subscribe(inst->seg, topic) == 0 ? NB_OK : NB_ERR_NOMEM;
    }
    return (roles & NB_PS_TOPIC_WRITER) ? NB_OK : NB_ERR_INVALID;
}

static const nb_rpc_transport_ops_t nb_g_shm_rpc = {
    .send = nb_shm_op_send,
    .reply = nb_shm_op_reply,
    .loan = nb_shm_op_loan,
    .send_loan = nb_shm_op_send_loan,
    .release_loan = nb_shm_op_release_loan,
};

static const nb_ps_backend_ops_t nb_g_shm_ps = {
    .register_topic = nb_shm_op_register_topic,
    .register_method = nb_shm_op_register_method,
    .unregister_method = nb_shm_op_unregister_method,
};

static const nb_transport_provider_ops_t nb_g_shm_ops = {
    .abi_version = NB_TRANSPORT_PROVIDER_ABI_VERSION,
    .struct_size = sizeof(nb_transport_provider_ops_t),
    .create = nb_shm_op_create,
    .start = nb_shm_op_start,
    .stop = nb_shm_op_stop,
    .destroy = nb_shm_op_destroy,
    .rpc = &nb_g_shm_rpc,
    .ps = &nb_g_shm_ps,
};

static const nb_transport_provider_t nb_g_shm_provider = {
    .scheme = "shm",
    .ops = &nb_g_shm_ops,
};

const nb_transport_provider_t *nb_shm_provider(void)
{
    return &nb_g_shm_provider;
}

int nb_shm_register(void)
{
    if (nb_transport_provider_find("shm")) {
        return NB_OK;
    }
    return nb_transport_provider_register(&nb_g_shm_provider);
}

#else /* !__linux__ */

int nb_shm_register(void)
{
    return NB_ERR_NOT_SUPPORTED;
}

const nb_transport_provider_t *nb_shm_provider(void)
{
    return NULL;
}

#endif
