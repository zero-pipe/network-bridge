/*
 * 消息分发与出站传输选择。
 *
 * 从 node.c 拆出：入站把传输上抛的完整消息分派到 service（请求/事件）或
 * pending（响应）；出站按 scheme/transport 选中一条传输发送。
 */
#include "node_internal.h"

#include "envelope.h"
#include "metric_macros.h"
#include "pending.h"
#include "service_internal.h"
#include "util.h"

#include <nb/log.h>

#include <stdlib.h>
#include <string.h>

/* 延迟采样序号；nb_metrics_sample_every > 1 时用于按比例抽样。 */
static uint32_t nb_g_latency_sample_seq;

/* 记录一次已完成 RPC 的端到端延迟（从发起 pending 到收到响应）。 */
static void nb_pending_record_latency(nb_node_t *node, const nb_pending_t *p)
{
    uint64_t now;
    uint64_t ms;
    int bucket;
    if (!node || !p || p->started_ms == 0) {
        return;
    }
    if (!nb_latency_enabled) {
        return;
    }
    if (nb_metrics_sample_every > 1u) {
        uint32_t seq = nb_atomic_fetch_add_u32(&nb_g_latency_sample_seq, 1u);
        if (seq % nb_metrics_sample_every != 0u) {
            return;
        }
    }
    now = nb_now_ms();
    ms = now > p->started_ms ? now - p->started_ms : 0;
    for (bucket = 0; bucket < NB_LATENCY_HIST_BUCKETS - 1; bucket++) {
        if (ms <= nb_latency_bounds[bucket]) {
            break;
        }
    }
    NB_METRIC_ADD(node->metrics.latency_hist[bucket], 1u);
    NB_METRIC_INC(node->metrics.latency_count);
    NB_METRIC_ADD(node->metrics.latency_sum_ms, ms);
}

/* 把响应帧上的 metadata / status message 解析到 pending（按需分配）。 */
static void nb_pending_attach_response(nb_pending_t *p, const nb_message_t *msg)
{
    if (!p) {
        return;
    }
    if (p->response_metadata) {
        free(p->response_metadata);
        p->response_metadata = NULL;
    }
    if (p->response_message) {
        free(p->response_message);
        p->response_message = NULL;
    }
    if (msg->metadata_len > 0 && msg->metadata) {
        nb_metadata_t *md = (nb_metadata_t *)calloc(1, sizeof(*md));
        if (md) {
            if (nb_metadata_decode(md, msg->metadata, msg->metadata_len) == NB_OK && md->count > 0) {
                p->response_metadata = md;
            } else {
                free(md);
            }
        }
    }
    if (msg->status_msg_len > 0 && msg->status_msg) {
        p->response_message = (char *)malloc((size_t)msg->status_msg_len + 1);
        if (p->response_message) {
            memcpy(p->response_message, msg->status_msg, msg->status_msg_len);
            p->response_message[msg->status_msg_len] = '\0';
        }
    }
}

int nb_node_resolve(nb_node_t *node, const char *service, int prefer_stream, nb_endpoint_t *out)
{
    if (!node || !service || !out) {
        return NB_ERR_INVALID;
    }
    if (nb_route_select(&node->routes, service, prefer_stream, out) == NB_OK) {
        return NB_OK;
    }
    if (nb_registry_select_prefer(&node->registry, service, prefer_stream, out) != NB_OK) {
        return NB_ERR_NOTFOUND;
    }
    return NB_OK;
}

/*
 * per-peer 入站限速：按 ip:port 做固定 1 秒窗口计数。未配置时零开销返回允许。
 * 命中上限返回 0，调用方对 REQUEST 回 NB_ERR_AGAIN、对 EVENT 直接丢弃。
 */
static int nb_node_rate_allow(nb_node_t *node, const nb_message_t *msg)
{
    uint64_t now;
    int i;
    int free_slot = -1;
    if (!node->rate_limit_pps || !msg->peer_host) {
        return 1;
    }
    now = nb_now_ms();
    ntk_mutex_lock(node->rate_lock);
    for (i = 0; i < NB_NODE_RATE_LIMIT_SLOTS; i++) {
        nb_rate_slot_t *s = &node->rates[i];
        if (s->used && s->port == msg->peer_port && strcmp(s->ip, msg->peer_host) == 0) {
            if (now - s->window_start_ms >= NB_NODE_RATE_WINDOW_MS) {
                s->window_start_ms = now;
                s->count = 0;
            }
            if (s->count >= node->rate_limit_pps) {
                ntk_mutex_unlock(node->rate_lock);
                return 0;
            }
            s->count++;
            ntk_mutex_unlock(node->rate_lock);
            return 1;
        }
        if (!s->used || now - s->window_start_ms >= NB_NODE_RATE_WINDOW_MS) {
            if (free_slot < 0) {
                free_slot = i;
            }
        }
    }
    if (free_slot < 0) {
        ntk_mutex_unlock(node->rate_lock);
        return 1; /* 槽位耗尽：退化为不限速，避免误伤新对端 */
    }
    {
        nb_rate_slot_t *s = &node->rates[free_slot];
        s->used = 1;
        nb_copy_str(s->ip, sizeof(s->ip), msg->peer_host, "0.0.0.0");
        s->port = msg->peer_port;
        s->window_start_ms = now;
        s->count = 1;
    }
    ntk_mutex_unlock(node->rate_lock);
    return 1;
}

static void nb_node_dispatch(nb_node_t *node, nb_transport_t *transport, nb_message_t *msg);

/*
 * 跨 poller 响应回投：pending 归属 poller 才处理本请求的超时/回调，因此响应
 * 若落在其他 poller，需要把消息字段深拷贝到任务，再 async 回归属 poller。
 */
typedef struct nb_response_task {
    nb_node_t *node;
    nb_transport_t *transport;
    nb_message_t msg;
    char *route;
    char *peer_host;
    uint8_t *metadata;
    uint8_t *status_msg;
    uint8_t *payload;
} nb_response_task_t;

static void nb_response_task_destroy(nb_response_task_t *t)
{
    if (!t) {
        return;
    }
    if (t->msg.frame_owner) {
        nb_buf_release(t->msg.frame_owner);
        t->msg.frame_owner = NULL;
    }
    free(t->route);
    free(t->peer_host);
    free(t->metadata);
    free(t->status_msg);
    free(t->payload);
    free(t);
}

static int nb_response_task_init(nb_response_task_t *t,
                              nb_node_t *node,
                              nb_transport_t *transport,
                              const nb_message_t *msg,
                              nb_buf_t *frame_owner)
{
    size_t route_len = msg->route ? strlen(msg->route) + 1 : 0;
    size_t ip_len = msg->peer_host ? strlen(msg->peer_host) + 1 : 0;

    memset(t, 0, sizeof(*t));
    t->node = node;
    t->transport = transport;
    t->msg = *msg;
    t->msg.frame_owner = frame_owner;
    t->msg.reply_token = NULL;
    if (route_len) {
        t->route = (char *)malloc(route_len);
        if (!t->route) {
            goto fail;
        }
        memcpy(t->route, msg->route, route_len);
        t->msg.route = t->route;
    } else {
        t->msg.route = NULL;
    }
    if (ip_len) {
        t->peer_host = (char *)malloc(ip_len);
        if (!t->peer_host) {
            goto fail;
        }
        memcpy(t->peer_host, msg->peer_host, ip_len);
        t->msg.peer_host = t->peer_host;
    } else {
        t->msg.peer_host = NULL;
    }
    if (frame_owner) {
        t->msg.metadata = msg->metadata;
        t->msg.metadata_len = msg->metadata_len;
        t->msg.status_msg = msg->status_msg;
        t->msg.status_msg_len = msg->status_msg_len;
        t->msg.payload = msg->payload;
    } else if (msg->metadata_len > 0 && msg->metadata) {
        t->metadata = (uint8_t *)malloc(msg->metadata_len);
        if (!t->metadata) {
            goto fail;
        }
        memcpy(t->metadata, msg->metadata, msg->metadata_len);
        t->msg.metadata = t->metadata;
    } else {
        t->msg.metadata = NULL;
        t->msg.metadata_len = 0;
    }
    if (!frame_owner && msg->status_msg_len > 0 && msg->status_msg) {
        t->status_msg = (uint8_t *)malloc(msg->status_msg_len);
        if (!t->status_msg) {
            goto fail;
        }
        memcpy(t->status_msg, msg->status_msg, msg->status_msg_len);
        t->msg.status_msg = (const char *)t->status_msg;
    } else if (!frame_owner) {
        t->msg.status_msg = NULL;
        t->msg.status_msg_len = 0;
    }
    if (!frame_owner && msg->payload.len > 0 && msg->payload.data) {
        t->payload = (uint8_t *)malloc(msg->payload.len);
        if (!t->payload) {
            goto fail;
        }
        memcpy(t->payload, msg->payload.data, msg->payload.len);
        t->msg.payload.data = t->payload;
    } else if (!frame_owner) {
        t->msg.payload.data = NULL;
        t->msg.payload.len = 0;
    }
    return NB_OK;
fail:
    nb_response_task_destroy(t);
    return NB_ERR_NOMEM;
}

static void nb_response_dispatch_task(void *user)
{
    nb_response_task_t *t = (nb_response_task_t *)user;
    nb_node_dispatch(t->node, t->transport, &t->msg);
    nb_response_task_destroy(t);
}

static void nb_node_dispatch(nb_node_t *node, nb_transport_t *transport, nb_message_t *msg)
{
    if (msg->kind == KIND_CANCEL) {
        nb_service_handle_cancel(node, transport, msg);
        return;
    }
    if (msg->kind == KIND_PING) {
        nb_message_t pong;

        /* PING 是控制面心跳：原样回显 request_id，不进入业务分发。 */
        memset(&pong, 0, sizeof(pong));
        pong.kind = KIND_PING;
        pong.request_id = msg->request_id;
        pong.peer_host = msg->peer_host;
        pong.peer_port = msg->peer_port;
        pong.reply_token = msg->reply_token;
        if (transport && transport->ops && transport->ops->reply) {
            (void)transport->ops->reply(transport, &pong);
        }
        return;
    }
    if (msg->kind == KIND_GOAWAY) {
        /* 对端声明不再接收新请求；已发起的调用仍按各自超时处理。 */
        nb_log_warn("GOAWAY from %s:%u (request_id=%u)",
                     msg->peer_host ? msg->peer_host : "?",
                     (unsigned)msg->peer_port,
                     (unsigned)msg->request_id);
        return;
    }
    if (msg->kind == KIND_REQUEST || msg->kind == KIND_EVENT) {
        if (!nb_node_rate_allow(node, msg)) {
            NB_METRIC_INC(node->metrics.rate_limited);
            if (msg->kind == KIND_REQUEST) {
                nb_service_send_status(transport, msg, NB_ERR_AGAIN);
            }
            return;
        }
        if (msg->has_stream) {
            nb_service_handle_stream_chunk(node, transport, msg);
        } else {
            nb_service_handle_request(node, transport, msg);
        }
        return;
    }
    if (msg->kind == KIND_RESPONSE) {
        ntk_poller *owner = nb_pending_owner(node, msg->request_id);
        if (owner && !ntk_poller_is_current_thread(owner)) {
            nb_buf_t *frame_owner = msg->frame_owner;
            nb_response_task_t *task =
                (nb_response_task_t *)calloc(1, sizeof(*task));
            if (frame_owner) {
                msg->frame_owner = NULL;
            }
            if (!task) {
                if (frame_owner) {
                    nb_buf_release(frame_owner);
                }
                return;
            }
            if (nb_response_task_init(task, node, transport, msg, frame_owner) != NB_OK) {
                return; /* nb_response_task_init 已释放 task */
            }
            if (ntk_poller_async(owner, nb_response_dispatch_task, task, 1) != NTK_OK) {
                nb_response_task_destroy(task);
            }
            return;
        }
        nb_pending_t *p = nb_pending_find(node, msg->request_id);
        nb_response_t resp;
        if (!p) {
            return;
        }
        NB_METRIC_INC(node->metrics.responses_received);
        NB_METRIC_ADD(node->metrics.bytes_received, msg->payload.len);
        if (msg->flags & NB_MSG_FLAG_FRAMEWORK_STATUS) {
            /* 框架错误走回调 status 通道，nb_response_t.status 只放业务码。 */
            nb_pending_record_latency(node, p);
            nb_pending_invoke(p, msg->status, NULL);
            return;
        }
        nb_pending_attach_response(p, msg);
        if (p->streaming) {
            int app_status = msg->status;
            nb_chunk_t chunk;
            chunk.data = msg->payload.data;
            chunk.len = msg->payload.len;
            chunk.encoding = msg->payload.encoding;
            chunk.offset = msg->has_stream ? msg->stream.offset : p->expected_offset;
            chunk.flags = msg->has_stream ? msg->stream.flags : 0;
            chunk.metadata = p->response_metadata;
            chunk.status_msg = p->response_message;
            if (p->chunk_cb) {
                p->chunk_cb(app_status, &chunk, p->stream_user);
            }
            p->expected_offset = chunk.offset + chunk.len;
            if (chunk.flags & NB_STREAM_FLAG_LAST) {
                stream_done_fn done = p->done_cb;
                void *stream_user = p->stream_user;
                nb_pending_record_latency(node, p);
                p->chunk_cb = NULL;
                p->done_cb = NULL;
                if (done) {
                    done(app_status, stream_user);
                }
                nb_pending_release(p);
            }
            return;
        }
        resp.status = msg->status;
        resp.status_msg = p->response_message;
        resp.payload = msg->payload;
        resp.metadata = p->response_metadata;
        resp.owns_payload = false;
        nb_pending_record_latency(node, p);
        nb_pending_invoke(p, NB_OK, &resp);
        return;
    }
    /* EVENT: 预留，忽略 */
}

void nb_node_on_message(nb_node_t *node, nb_transport_t *transport, nb_message_t *msg)
{
    if (!node || !transport || !msg) {
        return;
    }
    nb_node_dispatch(node, transport, msg);
    if (msg->frame_owner) {
        nb_buf_release(msg->frame_owner);
        msg->frame_owner = NULL;
    }
}

nb_transport_t *nb_node_find_transport(nb_node_t *node, const nb_endpoint_t *target)
{
    size_t i;

    if (!node || !target) {
        return NULL;
    }
    for (i = 0; i < node->transport_count; i++) {
        nb_transport_t *transport = node->transports[i];
        if (!transport) {
            continue;
        }
        if (target->scheme[0] && strcmp(transport->scheme, target->scheme) != 0) {
            continue;
        }
        if (!target->scheme[0] && transport->kind != target->transport) {
            continue;
        }
        if (transport->kind == NB_TRANSPORT_KIND_CUSTOM && target->host[0] &&
            transport->bind_host[0] && strcmp(transport->bind_host, target->host) != 0) {
            continue;
        }
        return transport;
    }
    return NULL;
}

int nb_node_send(nb_node_t *node, const nb_endpoint_t *target, const nb_message_t *msg)
{
    nb_message_t outbound;
    nb_transport_t *transport;

    if (!node || !msg || !target || (nb_endpoint_is_network(target) && !target->port)) {
        return NB_ERR_INVALID;
    }
    outbound = *msg;
    outbound.target = target;
    outbound.peer_host = target->host;
    outbound.peer_port = target->port;
    transport = nb_node_find_transport(node, target);
    if (!transport) {
        return NB_ERR_NOTFOUND;
    }
    return nb_transport_send(transport, &outbound);
}

int nb_node_send_call(nb_node_t *node, const nb_stub_request_t *request)
{
    nb_message_t message;
    char route[NB_GLOBAL_MAX_ROUTE_BYTES];

    if (!node || !request || !request->service || !request->method || !request->target ||
        !request->target->host[0] ||
        (nb_endpoint_is_network(request->target) && !request->target->port)) {
        return NB_ERR_INVALID;
    }
    if (nb_route_format(route, sizeof(route), request->service, request->method) != NB_OK) {
        return NB_ERR_INVALID;
    }
    memset(&message, 0, sizeof(message));
    message.kind = request->kind == NB_STUB_KIND_EVENT ? KIND_EVENT :
                   (request->kind == NB_STUB_KIND_CANCEL ? KIND_CANCEL : KIND_REQUEST);
    message.request_id = (uint32_t)request->request_id;
    /* 请求剩余超时透传给服务端（0 = 无 deadline）；CANCEL 不使用该字段。 */
    message.timeout_ms =
        request->timeout_ms > 0xffffffffull ? 0xffffffffu : (uint32_t)request->timeout_ms;
    message.metadata = request->metadata;
    message.metadata_len = request->metadata_len;
    message.route = route;
    message.payload = request->payload ? *request->payload : (nb_payload_t){ 0 };
    return nb_node_send(node, request->target, &message);
}
