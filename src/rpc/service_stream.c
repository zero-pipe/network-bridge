/*
 * 服务端流：接收流状态、按流串行分发、过期回收。
 *
 * INLINE 直接在 poller 上跑；WORKER / EXECUTOR 下每条流的 chunk 串行提交，
 * 保证同一流的处理顺序。
 */
#include "node_internal.h"
#include "service_internal.h"

#include "atomic.h"
#include "envelope.h"
#include "metric_macros.h"
#include <nb/internal/poller_alloc.h>
#include "util.h"

#include <nb/log.h>

#include <stdlib.h>
#include <string.h>

static nb_recv_stream_t *nb_find_recv_stream(nb_node_t *node, const nb_message_t *msg)
{
    int i;
    for (i = 0; i < NB_STREAM_MAX_RECV_STREAMS; i++) {
        nb_recv_stream_t *stream = &node->recv_streams[i];
        if (stream->used && stream->request_id == msg->request_id &&
            stream->peer_port == msg->peer_port &&
            strcmp(stream->peer_host, msg->peer_host ? msg->peer_host : "") == 0) {
            return stream;
        }
    }
    return NULL;
}

static nb_recv_stream_t *nb_alloc_recv_stream(nb_node_t *node,
                                            nb_transport_t *transport,
                                            const nb_message_t *msg)
{
    int i;
    int used = 0;
    for (i = 0; i < NB_STREAM_MAX_RECV_STREAMS; i++) {
        if (node->recv_streams[i].used) {
            used++;
        }
    }
    if (node->max_recv_streams != 0 && (uint32_t)used >= node->max_recv_streams) {
        NB_METRIC_INC(node->metrics.streams_rejected);
        return NULL;
    }
    for (i = 0; i < NB_STREAM_MAX_RECV_STREAMS; i++) {
        nb_recv_stream_t *stream = &node->recv_streams[i];
        if (!stream->used) {
            nb_call_t *call = (nb_call_t *)nb_poller_alloc(sizeof(*call));
            if (!call) {
                return NULL;
            }
            memset(call, 0, sizeof(*call));
            stream->used = 1;
            stream->request_id = msg->request_id;
            nb_copy_str(stream->peer_host, sizeof(stream->peer_host), msg->peer_host, "0.0.0.0");
            stream->peer_port = msg->peer_port;
            stream->call = call;
            stream->last_ms = nb_now_ms();
            stream->bytes = 0;
            call->node = node;
            call->transport = transport;
            call->poller = ntk_poller_current();
            call->token = msg->reply_token;
            nb_copy_str(call->peer_host, sizeof(call->peer_host), msg->peer_host, "0.0.0.0");
            call->peer_port = msg->peer_port;
            call->msg_id = msg->request_id;
            call->stream_owned = 1;
            call->deadline_ms = nb_service_deadline_from_timeout(msg->timeout_ms);
            return stream;
        }
    }
    return NULL;
}

/* ---- 流式 chunk 的 worker/executor 派发（每个 stream 串行，保证顺序） ---- */

typedef struct nb_stream_task {
    nb_recv_stream_t *state;
    nb_call_t *call;
    uint8_t *data;
    size_t len;
    uint8_t encoding;
    unsigned flags;
    uint64_t offset;
    const nb_metadata_t *metadata;   /* 借用 stream 状态里的副本；仅本次任务内有效 */
    nb_metadata_t chunk_metadata;    /* 本 chunk 自带 metadata 的解码副本 */
    int has_chunk_metadata;
    char message[NB_STATUS_MSG_MAX]; /* 本 chunk 的状态消息 */
    uint16_t message_len;
    uint64_t deadline_ms;
    nb_service_interceptor_fn interceptor;
    void *interceptor_user;
    nb_chunk_handler_fn fn;
    void *user;
    int first;
    int rejected;
    struct nb_stream_task *next;
} nb_stream_task_t;

static void nb_stream_task_run(void *user);
static void nb_stream_task_complete(void *user);

static void nb_stream_task_free(nb_stream_task_t *t)
{
    if (!t) {
        return;
    }
    free(t->data);
    free(t);
}

static void nb_stream_clear_pending(nb_recv_stream_t *state)
{
    nb_stream_task_t *t = state->pending_head;
    while (t) {
        nb_stream_task_t *next = t->next;
        nb_stream_task_free(t);
        t = next;
    }
    state->pending_head = NULL;
    state->pending_tail = NULL;
}

static void nb_stream_close(nb_recv_stream_t *state)
{
    nb_call_t *call = state->call;
    nb_stream_clear_pending(state);
    free(state->metadata);
    memset(state, 0, sizeof(*state));
    if (call) {
        nb_poller_free(call);
    }
}

static int nb_stream_submit(nb_recv_stream_t *state, nb_stream_task_t *task)
{
    nb_method_t *m = state->method;
    nb_node_t *node = state->call ? state->call->node : NULL;
    if (!m) {
        nb_stream_task_free(task);
        return NB_ERR_STATE;
    }
    if (m->executor) {
        if (m->executor->submit(m->executor->user, nb_stream_task_run, task) != NB_OK) {
            nb_stream_task_free(task);
            return NB_ERR_IO;
        }
        return NB_OK;
    }
    if (node && node->handler_pool) {
        if (ntk_thread_pool_async(node->handler_pool, nb_stream_task_run, task, 1) != NTK_OK) {
            nb_stream_task_free(task);
            return NB_ERR_IO;
        }
        return NB_OK;
    }
    nb_stream_task_free(task);
    return NB_ERR_STATE;
}

static void nb_stream_dispatch_next(nb_recv_stream_t *state)
{
    nb_stream_task_t *task;
    if (!state || state->in_flight) {
        return;
    }
    task = state->pending_head;
    if (!task) {
        return;
    }
    state->pending_head = task->next;
    if (!state->pending_head) {
        state->pending_tail = NULL;
    }
    task->next = NULL;
    state->in_flight = 1;
    if (nb_stream_submit(state, task) != NB_OK) {
        state->in_flight = 0;
        state->closing = 1;
    }
}

static void nb_stream_task_run(void *user)
{
    nb_stream_task_t *task = (nb_stream_task_t *)user;
    nb_recv_stream_t *state = task->state;
    nb_call_t *call = task->call;
    nb_request_t req;
    nb_chunk_t chunk;
    int rejected = 0;

    if (!state || !call) {
        nb_stream_task_free(task);
        return;
    }
    req.service = state->service;
    req.method = state->method_name;
    req.payload.data = task->data;
    req.payload.len = task->len;
    req.payload.encoding = task->encoding;
    req.peer_host = call->peer_host;
    req.peer_port = call->peer_port;
    req.deadline_ms = task->deadline_ms;
    req.metadata = task->metadata;
    chunk.data = task->data;
    chunk.len = task->len;
    chunk.encoding = task->encoding;
    chunk.offset = task->offset;
    chunk.flags = task->flags;
    chunk.metadata = task->has_chunk_metadata ? &task->chunk_metadata : NULL;
    chunk.status_msg = task->message_len ? task->message : NULL;
    if (task->first && task->interceptor) {
        int st = task->interceptor(call, &req, task->interceptor_user);
        if (st != NB_OK) {
            nb_reply_stream(call, st, NULL);
            rejected = 1;
        }
    }
    if (!rejected) {
        task->fn(call, &req, &chunk, task->user);
    }
    task->rejected = rejected;
    if (ntk_poller_async(call->poller, nb_stream_task_complete, task, 1) != NTK_OK) {
        nb_stream_task_free(task);
    }
}

static void nb_stream_task_complete(void *user)
{
    nb_stream_task_t *task = (nb_stream_task_t *)user;
    nb_recv_stream_t *state = task->state;
    int rejected = task->rejected;
    nb_stream_task_free(task);
    if (!state || !state->used) {
        return;
    }
    state->in_flight = 0;
    state->last_ms = nb_now_ms();
    if (rejected) {
        state->closing = 1;
        nb_stream_clear_pending(state);
    }
    if (state->closing && !state->pending_head) {
        nb_stream_close(state);
        return;
    }
    nb_stream_dispatch_next(state);
}

void nb_service_handle_stream_chunk(nb_node_t *node,
                                     nb_transport_t *transport,
                                     const nb_message_t *msg)
{
    char tmp[NB_GLOBAL_MAX_ROUTE_BYTES];
    char *dot;
    nb_service_t *svc = NULL;
    nb_method_t *method = NULL;
    nb_recv_stream_t *state;
    nb_request_t request;
    nb_chunk_t chunk;
    nb_metadata_t chunk_metadata;
    int has_chunk_metadata = 0;
    char chunk_message[NB_STATUS_MSG_MAX];
    uint16_t chunk_message_len = 0;
    const nb_stream_meta_t *stream_meta;
    int use_worker;
    nb_service_interceptor_fn interceptor = NULL;
    void *interceptor_user = NULL;

    if (!node || !msg || !msg->has_stream) {
        return;
    }
    stream_meta = &msg->stream;
    state = nb_find_recv_stream(node, msg);

    if (stream_meta->flags & NB_STREAM_FLAG_CANCEL) {
        if (state) {
            if ((state->run_in_worker || state->executor) &&
                (state->in_flight || state->pending_head)) {
                state->closing = 1;
                state->canceled = 1;
                nb_stream_clear_pending(state);
                if (!state->in_flight) {
                    nb_stream_close(state);
                }
            } else {
                nb_stream_close(state);
            }
        }
        return;
    }

    if (state) {
        method = state->method;
    } else {
        if (!(stream_meta->flags & NB_STREAM_FLAG_FIRST)) {
            return; /* 没有 FIRST 也没有已有状态：协议错误 */
        }
        nb_copy_str(tmp, sizeof(tmp), msg->route, "");
        dot = strchr(tmp, '.');
        if (!dot) {
            return;
        }
        *dot = '\0';
        ntk_mutex_lock(node->services_lock);
        for (svc = node->services; svc; svc = svc->next) {
            if (svc->removed || strcmp(svc->name, tmp) != 0) {
                continue;
            }
            for (method = svc->methods; method; method = method->next) {
                if (strcmp(method->name, dot + 1) == 0 && method->stream_fn) {
                    break;
                }
            }
            if (method) {
                interceptor = svc->interceptor;
                interceptor_user = svc->interceptor_user;
            }
            break;
        }
        ntk_mutex_unlock(node->services_lock);
        if (!method) {
            return;
        }
        state = nb_alloc_recv_stream(node, transport, msg);
        if (state) {
            state->method = method;
            state->run_in_worker = method->run_in_worker;
            state->executor = method->executor;
            state->interceptor = interceptor;
            state->interceptor_user = interceptor_user;
            state->deadline_ms = state->call ? state->call->deadline_ms : 0;
            if (msg->metadata_len > 0 && msg->metadata) {
                nb_metadata_t *md = (nb_metadata_t *)calloc(1, sizeof(*md));
                if (md) {
                    if (nb_metadata_decode(md, msg->metadata, msg->metadata_len) == NB_OK &&
                        md->count > 0) {
                        state->metadata = md;
                    } else {
                        free(md);
                    }
                }
            }
            nb_copy_str(state->service, sizeof(state->service), tmp, "");
            nb_copy_str(state->method_name, sizeof(state->method_name), dot + 1, "");
        }
    }
    if (!state || !state->call || !method) {
        return;
    }
    use_worker = (state->run_in_worker && node->handler_pool) || state->executor;
    if (use_worker) {
        int first = !state->intercepted;
        nb_stream_task_t *task = (nb_stream_task_t *)calloc(1, sizeof(*task));
        if (!task) {
            state->closing = 1;
            nb_stream_close(state);
            return;
        }
        task->state = state;
        task->call = state->call;
        task->fn = method->stream_fn;
        task->user = method->user;
        task->first = first;
        task->interceptor = state->interceptor;
        task->interceptor_user = state->interceptor_user;
        task->deadline_ms = state->deadline_ms;
        task->flags = stream_meta->flags;
        task->offset = stream_meta->offset;
        task->encoding = msg->payload.encoding;
        task->len = msg->payload.len;
        if (task->len > 0) {
            task->data = (uint8_t *)malloc(task->len);
            if (!task->data) {
                free(task);
                state->closing = 1;
                nb_stream_close(state);
                return;
            }
            memcpy(task->data, msg->payload.data, task->len);
        }
        if (state->metadata) {
            task->metadata = state->metadata;
        }
        if (msg->metadata_len > 0 && msg->metadata &&
            nb_metadata_decode(&task->chunk_metadata, msg->metadata, msg->metadata_len) == NB_OK &&
            task->chunk_metadata.count > 0) {
            task->has_chunk_metadata = 1;
        }
        if (msg->status_msg_len > 0 && msg->status_msg) {
            size_t message_len = msg->status_msg_len;
            if (message_len >= sizeof(task->message)) {
                message_len = sizeof(task->message) - 1;
            }
            memcpy(task->message, msg->status_msg, message_len);
            task->message[message_len] = '\0';
            task->message_len = (uint16_t)message_len;
        }
        state->intercepted = 1;
        state->last_ms = nb_now_ms();
        state->bytes += msg->payload.len;
        if (state->pending_tail) {
            state->pending_tail->next = task;
        } else {
            state->pending_head = task;
        }
        state->pending_tail = task;
        if (stream_meta->flags & NB_STREAM_FLAG_LAST) {
            state->closing = 1;
        }
        nb_stream_dispatch_next(state);
        if (state->closing && !state->in_flight && !state->pending_head) {
            nb_stream_close(state);
        }
        return;
    }
    if (state->run_in_worker && !node->handler_pool &&
        nb_atomic_exchange_u32((uint32_t *)&node->worker_fallback_warned, 1u) == 0) {
        nb_log_warn("handler_threads=0: worker stream %s.%s runs inline on poller",
                     state->service,
                     state->method_name);
    }
    state->last_ms = nb_now_ms();
    state->bytes += msg->payload.len;
    memset(&request, 0, sizeof(request));
    request.service = state->service;
    request.method = state->method_name;
    request.payload = msg->payload;
    request.peer_host = state->call->peer_host;
    request.peer_port = msg->peer_port;
    request.deadline_ms = state->call->deadline_ms;
    request.metadata = state->metadata;
    memset(&chunk, 0, sizeof(chunk));
    chunk.data = msg->payload.data;
    chunk.len = msg->payload.len;
    chunk.encoding = msg->payload.encoding;
    chunk.offset = stream_meta->offset;
    chunk.flags = stream_meta->flags;
    if (msg->metadata_len > 0 && msg->metadata &&
        nb_metadata_decode(&chunk_metadata, msg->metadata, msg->metadata_len) == NB_OK &&
        chunk_metadata.count > 0) {
        has_chunk_metadata = 1;
    }
    chunk.metadata = has_chunk_metadata ? &chunk_metadata : NULL;
    if (msg->status_msg_len > 0 && msg->status_msg) {
        size_t message_len = msg->status_msg_len;
        if (message_len >= sizeof(chunk_message)) {
            message_len = sizeof(chunk_message) - 1;
        }
        memcpy(chunk_message, msg->status_msg, message_len);
        chunk_message[message_len] = '\0';
        chunk_message_len = (uint16_t)message_len;
    }
    chunk.status_msg = chunk_message_len ? chunk_message : NULL;

    if (!state->intercepted && state->interceptor) {
        int st = state->interceptor(state->call, &request, state->interceptor_user);
        if (st != NB_OK) {
            state->intercepted = 1;
            nb_reply_stream(state->call, st, NULL);
            nb_stream_close(state);
            return;
        }
    }
    state->intercepted = 1;
    method->stream_fn(state->call, &request, &chunk, method->user);

    if (stream_meta->flags & NB_STREAM_FLAG_LAST) {
        nb_stream_close(state);
    }
}

void nb_service_expire_streams(nb_node_t *node, uint64_t now_ms)
{
    int i;
    if (!node) {
        return;
    }
    for (i = 0; i < NB_STREAM_MAX_RECV_STREAMS; i++) {
        nb_recv_stream_t *stream = &node->recv_streams[i];
        if (stream->used && now_ms - stream->last_ms > NB_STREAM_IDLE_TIMEOUT_MS) {
            if ((stream->run_in_worker || stream->executor) &&
                (stream->in_flight || stream->pending_head)) {
                stream->closing = 1;
                nb_stream_clear_pending(stream);
                if (!stream->in_flight) {
                    nb_stream_close(stream);
                }
            } else {
                nb_stream_close(stream);
            }
        }
    }
}
