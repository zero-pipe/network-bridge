/*
 * 服务端请求执行：handler 任务、worker/executor 调度与入站请求分发。
 *
 * 方法注册与生命周期见 service.c；服务端流见 service_stream.c。
 */
#include "node_internal.h"
#include "service_internal.h"

#include "atomic.h"
#include "envelope.h"
#include "metric_macros.h"
#include <nb/internal/poller_alloc.h>
#include "util.h"

#include <nb/base/log.h>

#include <stdlib.h>
#include <string.h>

/*
 * 阻塞 handler 的线程池任务：请求字段全部深拷贝。worker 里构造 call 并执行
 * 用户 handler；nb_reply() 会自动把响应回投到 origin poller 发送。
 */
#define HANDLER_INLINE_PAYLOAD 128

/*
 * 小 payload 在 dispatch 边界拷贝更便宜：避免把 frame_owner move 到 worker
 * 后在另一线程归还池，导致 pool lock 的 cacheline 在 IO/worker 之间来回弹跳。
 * 大于该阈值仍 move，避免大包拷贝。
 */
#define NB_DISPATCH_COPY_MAX_BYTES 4096u

typedef struct nb_handler_task {
    nb_node_t *node;
    nb_transport_t *transport;
    nb_reply_token_t token;
    ntk_poller *poller;
    char peer_host[NB_HOST_MAX];
    uint16_t peer_port;
    uint32_t msg_id;
    int one_way;
    nb_handler_fn fn;
    void *user;
    nb_service_interceptor_fn interceptor;
    void *interceptor_user;
    char service[NB_SERVICE_NAME_MAX];
    char method[NB_METHOD_NAME_MAX];
    uint8_t *payload;
    uint8_t inline_payload[HANDLER_INLINE_PAYLOAD]; /* 小请求免一次 malloc */
    size_t payload_len;
    uint8_t encoding;
    uint64_t deadline_ms;                           /* 请求 deadline（绝对单调 ms，0 = 无） */
    nb_metadata_t *metadata;                        /* 请求 metadata 解码副本，按需分配；无则为 NULL */
    nb_buf_t *frame_owner;                          /* 入站整帧所有权；非空时 payload 借用其中 */
    int external;                                   /* 1 = 自定义 executor 任务 */
} nb_handler_task_t;

static void nb_handler_inflight_release(nb_node_t *node);
static void nb_handler_external_release(nb_node_t *node);

static void nb_handler_task_release(nb_handler_task_t *t)
{
    if (t->external) {
        nb_handler_inflight_release(t->node);
        nb_handler_external_release(t->node);
    } else {
        nb_handler_inflight_release(t->node);
    }
}

static void nb_handler_task_destroy(nb_handler_task_t *t)
{
    if (!t) {
        return;
    }
    if (t->frame_owner) {
        nb_buf_release(t->frame_owner);
    } else if (t->payload && t->payload != t->inline_payload) {
        free(t->payload);
    }
    free(t->metadata);
    if (t->node && t->node->handler_task_pool.lock) {
        nb_obj_pool_free(&t->node->handler_task_pool, t);
    } else {
        free(t);
    }
}

/* worker 路径的 call：优先取节点复用池，抢不到锁/池空则回退 malloc。 */
nb_call_t *nb_worker_call_alloc(nb_node_t *node)
{
    nb_call_t *call;
    if (!node) {
        return NULL;
    }
    call = (nb_call_t *)nb_obj_pool_alloc(&node->worker_call_pool, sizeof(*call));
    if (!call) {
        return NULL;
    }
    memset(call, 0, sizeof(*call));
    call->from_worker_pool = 1;
    return call;
}

void nb_worker_call_free(nb_node_t *node, nb_call_t *call)
{
    if (!call) {
        return;
    }
    if (call->from_worker_pool) {
        if (node && node->worker_call_pool.lock) {
            nb_obj_pool_free(&node->worker_call_pool, call);
        } else {
            free(call);
        }
        return;
    }
    nb_poller_free(call);
}

static void nb_handler_task_run(void *user)
{
    nb_handler_task_t *t = (nb_handler_task_t *)user;
    nb_call_t *call;
    nb_request_t req;
    int one_way = t->one_way;

    /* worker 线程没有 current poller，nb_poller_alloc 会回退到 malloc。 */
    call = nb_worker_call_alloc(t->node);
    if (!call) {
        nb_handler_task_release(t);
        nb_handler_task_destroy(t);
        return;
    }
    /* nb_worker_call_alloc 已清零并设置 from_worker_pool，这里不能再 memset。 */
    call->node = t->node;
    call->transport = t->transport;
    call->poller = t->poller;
    call->token = t->token;
    nb_copy_str(call->peer_host, sizeof(call->peer_host), t->peer_host, "0.0.0.0");
    call->peer_port = t->peer_port;
    call->msg_id = t->msg_id;
    call->one_way = one_way;
    call->deadline_ms = t->deadline_ms;

    req.service = t->service;
    req.method = t->method;
    req.payload.data = t->payload;
    req.payload.len = t->payload_len;
    req.payload.encoding = t->encoding;
    req.peer_host = call->peer_host;
    req.peer_port = call->peer_port;
    req.deadline_ms = t->deadline_ms;
    req.metadata = t->metadata;

    /*
     * 排队期间可能已过期或被客户端取消：启动 handler 前再检查一次，避免做
     * 注定被丢弃的重活。取消/超时的响应客户端通常已不再等待，发送失败也无妨。
     */
    {
        int canceled = nb_node_is_canceled(t->node, t->msg_id, t->peer_host, t->peer_port);
        int expired = t->deadline_ms != 0 && nb_now_ms() >= t->deadline_ms;
        if (canceled || expired) {
            if (!one_way) {
                nb_reply(call, canceled ? NB_ERR_NOTFOUND : NB_ERR_TIMEOUT, NULL);
            } else {
                nb_worker_call_free(t->node, call);
            }
            nb_handler_task_release(t);
            nb_handler_task_destroy(t);
            return;
        }
    }
    if (t->interceptor) {
        int st = t->interceptor(call, &req, t->interceptor_user);
        if (st != NB_OK) {
            if (!one_way) {
                nb_reply(call, st, NULL);
            } else {
                nb_worker_call_free(t->node, call);
            }
            nb_handler_task_release(t);
            nb_handler_task_destroy(t);
            return;
        }
    }
    nb_log_debug("worker request %s.%s id=%llu len=%zu",
                  t->service,
                  t->method,
                  (unsigned long long)t->msg_id,
                  t->payload_len);

    t->fn(call, &req, t->user);
    nb_handler_task_release(t);
    /* one_way 没有 nb_reply，call 在 handler 返回后释放；请求载荷也可释放。 */
    if (one_way) {
        nb_worker_call_free(t->node, call);
    }
    nb_handler_task_destroy(t);
}

/*
 * worker 未完成任务（排队 + 执行中）限流。0 表示不限制；超限返回 NB_ERR_AGAIN，
 * 由调用方在 poller 上直接回错误状态，避免无界排队。
 */
static int nb_handler_inflight_acquire(nb_node_t *node)
{
    int ok;
    if (node->handler_max_concurrency == 0 || !node->handler_limit_mutex) {
        return NB_OK;
    }
    ntk_mutex_lock(node->handler_limit_mutex);
    ok = node->handler_inflight < node->handler_max_concurrency;
    if (ok) {
        node->handler_inflight++;
    }
    ntk_mutex_unlock(node->handler_limit_mutex);
    return ok ? NB_OK : NB_ERR_AGAIN;
}

static void nb_handler_inflight_release(nb_node_t *node)
{
    if (node->handler_max_concurrency == 0 || !node->handler_limit_mutex) {
        return;
    }
    ntk_mutex_lock(node->handler_limit_mutex);
    if (node->handler_inflight > 0) {
        node->handler_inflight--;
    }
    ntk_mutex_unlock(node->handler_limit_mutex);
}

static void nb_handler_external_acquire(nb_node_t *node)
{
    if (!node->handler_limit_mutex) {
        return;
    }
    ntk_mutex_lock(node->handler_limit_mutex);
    node->external_inflight++;
    ntk_mutex_unlock(node->handler_limit_mutex);
}

static void nb_handler_external_release(nb_node_t *node)
{
    if (!node->handler_limit_mutex) {
        return;
    }
    ntk_mutex_lock(node->handler_limit_mutex);
    if (node->external_inflight > 0) {
        node->external_inflight--;
    }
    ntk_mutex_unlock(node->handler_limit_mutex);
}

/*
 * 决定是否把入站整帧所有权交给 worker/executor：
 * 小 payload 返回 NULL，让任务在 IO 线程上拷贝并在原 poller 释放 frame_owner；
 * 大 payload 返回 owner 并从 msg 摘除，避免一次大拷贝。
 */
static nb_buf_t *nb_dispatch_take_frame(nb_message_t *msg)
{
    nb_buf_t *owner;

    if (msg->frame_owner && msg->payload.len <= NB_DISPATCH_COPY_MAX_BYTES) {
        return NULL;
    }
    owner = msg->frame_owner;
    msg->frame_owner = NULL;
    return owner;
}

/* 把已解析好的请求深拷贝成可跨线程执行的任务。 */
static int nb_handler_task_create(nb_node_t *node,
                               nb_transport_t *transport,
                               const nb_message_t *msg,
                               const char *service,
                               const char *method,
                               nb_handler_fn fn,
                               void *user,
                               nb_service_interceptor_fn interceptor,
                               void *interceptor_user,
                               int external,
                               nb_buf_t *frame_owner,
                               nb_handler_task_t **out)
{
    nb_handler_task_t *t;
    size_t payload_len = msg->payload.len;

    *out = NULL;
    if (payload_len > 0 && !msg->payload.data) {
        if (frame_owner) {
            nb_buf_release(frame_owner);
        }
        return NB_ERR_INVALID;
    }
    t = (nb_handler_task_t *)nb_obj_pool_alloc(&node->handler_task_pool, sizeof(*t));
    if (!t) {
        if (frame_owner) {
            nb_buf_release(frame_owner);
        }
        return NB_ERR_NOMEM;
    }
    memset(t, 0, sizeof(*t));
    t->node = node;
    t->transport = transport;
    t->token = msg->reply_token;
    t->poller = ntk_poller_current();
    nb_copy_str(t->peer_host, sizeof(t->peer_host), msg->peer_host, "0.0.0.0");
    t->peer_port = msg->peer_port;
    t->msg_id = msg->request_id;
    t->one_way = msg->kind == KIND_EVENT;
    t->fn = fn;
    t->user = user;
    t->interceptor = interceptor;
    t->interceptor_user = interceptor_user;
    t->frame_owner = frame_owner;
    t->external = external;
    nb_copy_str(t->service, sizeof(t->service), service, "");
    nb_copy_str(t->method, sizeof(t->method), method, "");
    t->encoding = msg->payload.encoding;
    t->deadline_ms = nb_service_deadline_from_timeout(msg->timeout_ms);
    if (msg->metadata_len > 0 && msg->metadata) {
        nb_metadata_t *md = (nb_metadata_t *)malloc(sizeof(*md));
        if (md) {
            if (nb_metadata_decode(md, msg->metadata, msg->metadata_len) == NB_OK && md->count > 0) {
                t->metadata = md;
            } else {
                free(md);
            }
        }
    }
    t->payload_len = payload_len;
    if (frame_owner) {
        t->payload = (uint8_t *)msg->payload.data;
    } else if (payload_len > 0) {
        t->payload = payload_len <= sizeof(t->inline_payload) ? t->inline_payload :
                                                                (uint8_t *)malloc(payload_len);
        if (!t->payload) {
            nb_handler_task_destroy(t);
            return NB_ERR_NOMEM;
        }
        memcpy(t->payload, msg->payload.data, payload_len);
    }
    *out = t;
    return NB_OK;
}

/* 投递到节点内置 handler 线程池。 */
static int nb_service_dispatch_worker(nb_node_t *node,
                                   nb_transport_t *transport,
                                   const nb_message_t *msg,
                                   const char *service,
                                   const char *method,
                                   nb_handler_fn fn,
                                   void *user,
                                   nb_service_interceptor_fn interceptor,
                                   void *interceptor_user,
                                   nb_buf_t *frame_owner)
{
    nb_handler_task_t *t = NULL;
    int rc;

    rc = nb_handler_inflight_acquire(node);
    if (rc != NB_OK) {
        if (frame_owner) {
            nb_buf_release(frame_owner);
        }
        return rc;
    }
    rc = nb_handler_task_create(
        node, transport, msg, service, method, fn, user, interceptor, interceptor_user, 0,
        frame_owner, &t);
    if (rc != NB_OK) {
        nb_handler_inflight_release(node);
        return rc;
    }
    if (ntk_thread_pool_async(node->handler_pool, nb_handler_task_run, t, 1) != NTK_OK) {
        nb_handler_inflight_release(node);
        nb_handler_task_destroy(t);
        return NB_ERR_IO;
    }
    return NB_OK;
}

/* 投递到调用者提供的 executor（借用语义）。 */
static int nb_service_dispatch_external(nb_node_t *node,
                                     nb_transport_t *transport,
                                     const nb_message_t *msg,
                                     const char *service,
                                     const char *method,
                                     nb_handler_fn fn,
                                     void *user,
                                     const nb_service_executor_t *executor,
                                     nb_service_interceptor_fn interceptor,
                                     void *interceptor_user,
                                     nb_buf_t *frame_owner)
{
    nb_handler_task_t *t = NULL;
    int rc;

    /* 自定义 executor 与内置 worker 共用同一未完成上限，避免执行器侧无界排队。 */
    rc = nb_handler_inflight_acquire(node);
    if (rc != NB_OK) {
        if (frame_owner) {
            nb_buf_release(frame_owner);
        }
        return rc;
    }
    rc = nb_handler_task_create(
        node, transport, msg, service, method, fn, user, interceptor, interceptor_user, 1,
        frame_owner, &t);
    if (rc != NB_OK) {
        nb_handler_inflight_release(node);
        return rc;
    }
    nb_handler_external_acquire(node);
    rc = executor->submit(executor->user, nb_handler_task_run, t);
    if (rc != NB_OK) {
        nb_handler_external_release(node);
        nb_handler_inflight_release(node);
        nb_handler_task_destroy(t);
        return rc;
    }
    return NB_OK;
}

void nb_service_handle_request(nb_node_t *node, nb_transport_t *transport, nb_message_t *msg)
{
    char tmp[NB_GLOBAL_MAX_ROUTE_BYTES];
    char *dot;
    nb_service_t *svc;
    nb_method_t *m = NULL;
    nb_handler_fn fn = NULL;
    void *user = NULL;
    nb_service_interceptor_fn interceptor = NULL;
    void *interceptor_user = NULL;
    int dedup_active = 0;

    if (!node || !msg) {
        return;
    }
    nb_copy_str(tmp, sizeof(tmp), msg->route, "");
    dot = strchr(tmp, '.');
    if (!dot) {
        nb_service_send_status(transport, msg, NB_ERR_NOTFOUND);
        return;
    }
    *dot = '\0';
    ntk_mutex_lock(node->services_lock);
    for (svc = node->services; svc; svc = svc->next) {
        if (svc->removed || strcmp(svc->name, tmp) != 0) {
            continue;
        }
        for (m = svc->methods; m; m = m->next) {
            if (strcmp(m->name, dot + 1) == 0 && m->fn) {
                fn = m->fn;
                user = m->user;
                break;
            }
        }
        if (fn) {
            interceptor = svc->interceptor;
            interceptor_user = svc->interceptor_user;
        }
        break;
    }
    ntk_mutex_unlock(node->services_lock);
    if (!fn) {
        nb_service_send_status(transport, msg, NB_ERR_NOTFOUND);
        return;
    }
    /*
     * UDP unary 去重：命中已完成请求直接重放缓存响应；命中在途请求丢弃重复，
     * 避免非幂等 handler 被重传执行多次。TCP 可靠，不参与。
     */
    if (transport && transport->kind == NB_TRANSPORT_KIND_UDP && msg->kind == KIND_REQUEST &&
        msg->request_id != 0) {
        nb_dedup_action_t action = nb_dedup_begin(&node->dedup, transport, msg);
        if (action != NB_DEDUP_PROCESS) {
            return; /* 已重放缓存响应，或在途重复 */
        }
        dedup_active = 1;
    }
    /*
     * WORKER 方法走节点线程池。池未配置时退化为内联并告警；入池失败时直接
     * 回错误状态，避免慢 handler 在池异常时阻塞 poller。
     */
    if (m->executor) {
        nb_buf_t *frame_owner = nb_dispatch_take_frame(msg);
        int rc = nb_service_dispatch_external(node,
                                           transport,
                                           msg,
                                           tmp,
                                           dot + 1,
                                           fn,
                                           user,
                                           m->executor,
                                           interceptor,
                                           interceptor_user,
                                           frame_owner);
        if (rc == NB_OK) {
            return;
        }
        nb_log_error("executor dispatch failed: service=%s method=%s rc=%d", tmp, dot + 1, rc);
        if (dedup_active) {
            nb_dedup_abort(&node->dedup, msg);
        }
        nb_service_send_status(transport, msg, rc);
        return;
    }
    if (m->run_in_worker) {
        if (node->handler_pool) {
            nb_buf_t *frame_owner = nb_dispatch_take_frame(msg);
            int rc = nb_service_dispatch_worker(node,
                                             transport,
                                             msg,
                                             tmp,
                                             dot + 1,
                                             fn,
                                             user,
                                             interceptor,
                                             interceptor_user,
                                             frame_owner);
            if (rc == NB_OK) {
                return;
            }
            if (rc == NB_ERR_AGAIN) {
                nb_log_warn("worker overloaded: service=%s method=%s", tmp, dot + 1);
            } else {
                nb_log_error(
                    "worker dispatch failed: service=%s method=%s rc=%d", tmp, dot + 1, rc);
            }
            if (dedup_active) {
                nb_dedup_abort(&node->dedup, msg);
            }
            nb_service_send_status(transport, msg, rc);
            return;
        }
        if (nb_atomic_exchange_u32((uint32_t *)&node->worker_fallback_warned, 1u) == 0) {
            nb_log_warn(
                "handler_threads=0: worker method %s.%s runs inline on poller", tmp, dot + 1);
        }
    }
    nb_log_debug("request %s.%s id=%llu len=%zu peer=%s:%u",
                  tmp,
                  dot + 1,
                  (unsigned long long)msg->request_id,
                  msg->payload.len,
                  msg->peer_host ? msg->peer_host : "?",
                  (unsigned)msg->peer_port);
    {
        nb_call_t *call = (nb_call_t *)nb_poller_alloc(sizeof(*call));
        nb_request_t req;
        nb_metadata_t request_metadata;
        int has_metadata = 0;
        if (!call) {
            if (dedup_active) {
                nb_dedup_abort(&node->dedup, msg);
            }
            nb_service_send_status(transport, msg, NB_ERR_NOMEM);
            return;
        }
        memset(call, 0, sizeof(*call));
        call->node = node;
        call->transport = transport;
        call->poller = ntk_poller_current();
        call->token = msg->reply_token;
        nb_copy_str(call->peer_host, sizeof(call->peer_host), msg->peer_host, "0.0.0.0");
        call->peer_port = msg->peer_port;
        call->msg_id = msg->request_id;
        call->one_way = msg->kind == KIND_EVENT;
        call->deadline_ms = nb_service_deadline_from_timeout(msg->timeout_ms);
        req.service = tmp;
        req.method = dot + 1;
        req.payload = msg->payload;
        req.peer_host = call->peer_host;
        req.peer_port = msg->peer_port;
        req.deadline_ms = call->deadline_ms;
        if (msg->metadata_len > 0 && msg->metadata &&
            nb_metadata_decode(&request_metadata, msg->metadata, msg->metadata_len) == NB_OK &&
            request_metadata.count > 0) {
            has_metadata = 1;
        }
        req.metadata = has_metadata ? &request_metadata : NULL;
        {
            int one_way = call->one_way;
            int st = interceptor ? interceptor(call, &req, interceptor_user) : NB_OK;
            if (st == NB_OK) {
                fn(call, &req, user);
            } else {
                nb_reply(call, st, NULL);
            }
            if (one_way) {
                nb_poller_free(call);
            }
        }
    }
}

uint64_t nb_call_deadline_ms(const nb_call_t *call)
{
    return call ? call->deadline_ms : 0;
}

uint32_t nb_call_request_id(const nb_call_t *call)
{
    return call ? call->msg_id : 0;
}


