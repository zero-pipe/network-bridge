/* Stub：node 之上、传输无关的调用原语（请求/事件 + 关联 + 重传）。 */
#include "node_internal.h"
#include "proxy_internal.h"

#include "envelope.h"
#include "metric_macros.h"
#include "poller_alloc.h"
#include "util.h"

#include <nb/log.h>

#include <stdlib.h>
#include <string.h>

typedef struct nb_stub_task {
    nb_stub_t *stub;
    nb_request_id_t id;
    uint64_t timeout_ms;
    response_fn cb;
    response_chunk_fn chunk_cb;
    stream_done_fn done_cb;
    int streaming;
    int event;
    void *user;
    char service[NB_SERVICE_NAME_MAX];
    char method[NB_METHOD_NAME_MAX];
    uint8_t *data;
    nb_buf_t *data_owner;
    uint8_t *metadata;
    uint16_t metadata_len;
    size_t len;
    uint8_t encoding;
} nb_stub_task_t;

typedef struct nb_stub_resend_task {
    nb_node_t *node;
    nb_request_id_t id;
} nb_stub_resend_task_t;

static void nb_stub_task_free(nb_stub_task_t *task);

/* Default resolver: static route table + service discovery. */
static int nb_stub_default_send(nb_stub_t *stub,
                                const nb_stub_request_t *request,
                                void *user)
{
    nb_node_t *node = stub->node;
    (void)user;
    if (nb_node_resolve(node, request->service, 0, request->target) != NB_OK) {
        return NB_ERR_NOTFOUND;
    }
    return nb_node_send_call(node, request);
}

static uint64_t nb_stub_resend_task_fn(void *user)
{
    nb_stub_resend_task_t *task = (nb_stub_resend_task_t *)user;
    nb_pending_t *pending = nb_pending_find(task->node, task->id);
    if (pending && pending->has_req) {
        nb_pending_retx(task->node, pending);
    }
    free(task);
    return 0;
}

/*
 * 只在需要重发时保留请求副本：UDP 的 RTO 和所有传输的 backpressure 重试。
 * 异步调用的 owner 任务已经持有一份拷贝，直接转移所有权；否则此时再复制。
 */
static int nb_stub_save_replay(nb_pending_t *p,
                               nb_stub_task_t *owner,
                               nb_buf_t *buf_owner,
                               const uint8_t *data,
                               size_t len,
                               const uint8_t *metadata,
                               uint16_t metadata_len)
{
    if (!p) {
        return NB_ERR_INVALID;
    }
    if (p->replay_ready) {
        return NB_OK;
    }
    if (len > 0) {
        if (buf_owner) {
            p->data_owner = buf_owner;
            p->data = (uint8_t *)data;
        } else if (owner && owner->data) {
            p->data = owner->data;
            owner->data = NULL;
        } else {
            p->data = (uint8_t *)nb_poller_alloc(len);
            if (!p->data) {
                return NB_ERR_NOMEM;
            }
            memcpy(p->data, data, len);
        }
        p->len = len;
    } else if (buf_owner) {
        p->data_owner = buf_owner;
        p->data = (uint8_t *)data;
    }
    if (metadata_len > 0 && metadata) {
        if (owner && owner->metadata) {
            p->metadata = owner->metadata;
            owner->metadata = NULL;
        } else {
            p->metadata = (uint8_t *)malloc(metadata_len);
            if (!p->metadata) {
                if (p->data_owner) {
                    nb_buf_release(p->data_owner);
                    p->data_owner = NULL;
                } else if (p->data) {
                    nb_poller_free(p->data);
                }
                p->data = NULL;
                p->len = 0;
                return NB_ERR_NOMEM;
            }
            memcpy(p->metadata, metadata, metadata_len);
        }
        p->metadata_len = metadata_len;
    }
    p->replay_ready = 1;
    return NB_OK;
}

/* 在 poller 线程内完成一次请求的发送。 */
static void nb_stub_call_now(nb_stub_t *stub,
                          nb_stub_task_t *owner,
                          nb_buf_t *buf_owner,
                          nb_request_id_t id,
                          const char *service,
                          const char *method,
                          const uint8_t *data,
                          size_t len,
                          uint8_t encoding,
                          response_fn cb,
                          response_chunk_fn chunk_cb,
                          stream_done_fn done_cb,
                          int streaming,
                          void *user,
                          uint64_t timeout_ms,
                          const uint8_t *metadata,
                          uint16_t metadata_len)
{
    nb_node_t *node = stub->node;
    nb_pending_t *p;
    nb_payload_t pl;
    nb_endpoint_t target;
    nb_stub_request_t request;
    int rc;
    int prc = NB_OK;

    p = nb_pending_add(
        node, id, streaming ? NULL : cb, user, timeout_ms, ntk_poller_current(), &prc);
    if (!p) {
        int err = prc == NB_OK ? NB_ERR_NOMEM : prc;
        if (buf_owner) {
            nb_buf_release(buf_owner);
        }
        if (streaming) {
            if (done_cb) {
                done_cb(err, user);
            }
        } else if (cb) {
            cb(err, NULL, user);
        }
        return;
    }
    p->owner = stub;
    p->streaming = streaming;
    p->chunk_cb = chunk_cb;
    p->done_cb = done_cb;
    p->stream_user = user;
    p->has_req = 1;
    p->replay_ready = 0;
    nb_copy_str(p->service, sizeof(p->service), service, "");
    nb_copy_str(p->method, sizeof(p->method), method, "");
    p->encoding = encoding;
    p->len = 0;
    p->data = NULL;
    p->data_owner = NULL;
    p->metadata = NULL;
    p->metadata_len = 0;
    p->retries = 0;
    p->bp_retries = 0;
    p->next_retx_ms = nb_now_ms() + NB_RPC_RTO_INIT_MS;
    nb_log_debug("call %s.%s id=%llu len=%zu timeout=%llu",
                  service,
                  method,
                  (unsigned long long)id,
                  len,
                  (unsigned long long)timeout_ms);

    pl.data = data;
    pl.len = len;
    pl.encoding = encoding;
    memset(&target, 0, sizeof(target));
    memset(&request, 0, sizeof(request));
    request.service = service;
    request.method = method;
    request.payload = &pl;
    request.kind = NB_STUB_KIND_REQUEST;
    request.request_id = id;
    request.timeout_ms = timeout_ms;
    request.metadata = metadata;
    request.metadata_len = metadata_len;
    request.target = &target;
    rc = stub->send(stub, &request, stub->user);
    /* 目标在解析成功时即已回填，先落进 pending 供重传使用。 */
    nb_copy_str(p->ip, sizeof(p->ip), target.host, "0.0.0.0");
    p->port = target.port;
    p->transport = target.transport;
    nb_copy_str(p->scheme, sizeof(p->scheme), target.scheme, "udp");
    if (rc == NB_OK) {
        NB_METRIC_INC(node->metrics.requests_sent);
        NB_METRIC_ADD(node->metrics.bytes_sent, len);
        if (target.transport == NB_TRANSPORT_KIND_UDP) {
            if (nb_stub_save_replay(
                    p, owner, buf_owner, data, len, metadata, metadata_len) != NB_OK) {
                /* 请求已经发出，但 RTO 副本分配失败：降级为不重传，不改变调用结果。 */
                nb_log_warn("request replay copy failed (RTO disabled): id=%llu len=%zu",
                            (unsigned long long)id,
                            len);
            }
            buf_owner = NULL;
        } else if (buf_owner) {
            nb_buf_release(buf_owner);
            buf_owner = NULL;
        }
        return;
    }
    if (rc == NB_ERR_AGAIN && p->bp_retries++ < 1000) {
        if (nb_stub_save_replay(
                p, owner, buf_owner, data, len, metadata, metadata_len) != NB_OK) {
            buf_owner = NULL;
            nb_pending_invoke(p, NB_ERR_NOMEM, NULL);
            return;
        }
        buf_owner = NULL;
        nb_stub_resend_task_t *retry = (nb_stub_resend_task_t *)calloc(1, sizeof(*retry));
        if (retry) {
            ntk_poller *poller = ntk_poller_current();
            retry->node = node;
            retry->id = id;
            if (ntk_poller_do_delay(poller ? poller : node->poller, 1, nb_stub_resend_task_fn, retry) !=
                NULL) {
                return;
            }
            free(retry);
        }
        rc = NB_ERR_NOMEM;
    }
    if (buf_owner) {
        nb_buf_release(buf_owner);
    }
    nb_pending_invoke(p, rc, NULL);
}

static void nb_stub_event_now(nb_stub_t *stub,
                           nb_request_id_t id,
                           const char *service,
                           const char *method,
                           const uint8_t *data,
                           size_t len,
                           uint8_t encoding,
                           const uint8_t *metadata,
                           uint16_t metadata_len,
                           nb_buf_t *buf_owner)
{
    nb_payload_t pl;
    nb_endpoint_t target;
    nb_stub_request_t request;
    pl.data = data;
    pl.len = len;
    pl.encoding = encoding;
    memset(&target, 0, sizeof(target));
    memset(&request, 0, sizeof(request));
    request.service = service;
    request.method = method;
    request.payload = &pl;
    request.kind = NB_STUB_KIND_EVENT;
    request.request_id = id;
    request.metadata = metadata;
    request.metadata_len = metadata_len;
    request.target = &target;
    (void)stub->send(stub, &request, stub->user);
    if (buf_owner) {
        nb_buf_release(buf_owner);
    }
}

static void nb_stub_call_task_fn(void *user)
{
    nb_stub_task_t *t = (nb_stub_task_t *)user;
    const uint8_t *data = t->data;
    size_t len = t->len;
    nb_buf_t *buf_owner = t->data_owner;
    if (buf_owner) {
        /* buf_owner 已接管 data 指向的内部内存，task 不能再按独立分配释放。 */
        t->data_owner = NULL;
        t->data = NULL;
    }
    if (t->event) {
        nb_stub_event_now(t->stub,
                       t->id,
                       t->service,
                       t->method,
                       data,
                       len,
                       t->encoding,
                       t->metadata,
                       t->metadata_len,
                       buf_owner);
    } else {
        nb_stub_call_now(t->stub,
                      t,
                      buf_owner,
                      t->id,
                      t->service,
                      t->method,
                      data,
                      len,
                      t->encoding,
                      t->cb,
                      t->chunk_cb,
                      t->done_cb,
                      t->streaming,
                      t->user,
                      t->timeout_ms,
                      t->metadata,
                      t->metadata_len);
    }
    nb_stub_task_free(t);
}

static nb_request_id_t nb_stub_next_id(nb_node_t *node)
{
    nb_request_id_t id;
    ntk_mutex_lock(node->id_lock);
    id = (nb_request_id_t)(node->next_request_id++);
    ntk_mutex_unlock(node->id_lock);
    return id ? id : 1;
}

static void nb_stub_task_free(nb_stub_task_t *task)
{
    if (!task) {
        return;
    }
    if (task->data_owner) {
        nb_buf_release(task->data_owner);
    } else {
        nb_poller_free(task->data);
    }
    free(task->metadata);
    free(task);
}

/* 创建一次调用的任务快照（service/method/payload 全部深拷贝）。 */
static nb_stub_task_t *nb_stub_task_create(nb_stub_t *stub,
                                         nb_request_id_t id,
                                         const char *service,
                                         const char *method,
                                         const uint8_t *data,
                                         size_t len,
                                         uint8_t encoding,
                                         const uint8_t *metadata,
                                         uint16_t metadata_len,
                                         nb_buf_t *buf_owner)
{
    nb_stub_task_t *task;

    task = (nb_stub_task_t *)calloc(1, sizeof(*task));
    if (!task) {
        if (buf_owner) {
            nb_buf_release(buf_owner);
        }
        return NULL;
    }
    task->data_owner = buf_owner;
    task->stub = stub;
    task->id = id;
    nb_copy_str(task->service, sizeof(task->service), service, "");
    nb_copy_str(task->method, sizeof(task->method), method, "");
    task->encoding = encoding;
    task->len = len;
    if (len > 0) {
        if (buf_owner) {
            task->data = (uint8_t *)data;
        } else {
            /* 与 pending 的释放方式保持一致：可能被 pending 直接接管。 */
            task->data = (uint8_t *)nb_poller_alloc(len);
            if (!task->data) {
                nb_stub_task_free(task);
                return NULL;
            }
            memcpy(task->data, data, len);
        }
    } else if (buf_owner) {
        task->data = (uint8_t *)data;
    }
    if (metadata_len > 0 && metadata) {
        task->metadata = (uint8_t *)malloc(metadata_len);
        if (!task->metadata) {
            nb_stub_task_free(task);
            return NULL;
        }
        memcpy(task->metadata, metadata, metadata_len);
        task->metadata_len = metadata_len;
    }
    return task;
}

/* 跨线程投递；失败时释放任务并返回错误码。 */
static int nb_stub_task_submit(nb_stub_t *stub, nb_stub_task_t *task)
{
    ntk_poller *poller = nb_node_pick_poller(stub->node, task->id);
    if (ntk_poller_async(poller, nb_stub_call_task_fn, task, 1) != NTK_OK) {
        nb_stub_task_free(task);
        return NB_ERR_IO;
    }
    return NB_OK;
}

/* 校验并归一化 payload（长度上限、空指针、默认 encoding）。 */
static int nb_stub_payload_prepare(const nb_stub_t *stub,
                                const nb_payload_t *payload,
                                const uint8_t **data,
                                size_t *len,
                                uint8_t *encoding)
{
    if (!stub || !stub->node || !data || !len || !encoding) {
        return NB_ERR_INVALID;
    }
    *data = payload ? (const uint8_t *)payload->data : NULL;
    *len = payload ? payload->len : 0;
    *encoding = payload ? payload->encoding : 0;
    if (*len > stub->node->max_msg_bytes || (*len > 0 && !*data)) {
        return NB_ERR_INVALID;
    }
    if (*encoding == 0) {
        *encoding = stub->payload_encoding;
    }
    return NB_OK;
}

/* 把 metadata 编码进调用方的 inline 线格式缓冲；无 metadata 时 out_len = 0。 */
static int nb_stub_metadata_encode(const nb_metadata_t *metadata,
                                uint8_t *buf,
                                size_t cap,
                                uint16_t *out_len)
{
    size_t len = 0;
    *out_len = 0;
    if (!metadata) {
        return NB_OK;
    }
    if (nb_metadata_encode(metadata, buf, cap, &len) != NB_OK) {
        return NB_ERR_TOOBIG;
    }
    *out_len = (uint16_t)len;
    return NB_OK;
}

/* ---- 生命周期 ---- */

int nb_stub_create(nb_node_t *node, const nb_stub_options_t *options, nb_stub_t **out)
{
    nb_stub_t *stub;
    if (!node || !out) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    stub = (nb_stub_t *)calloc(1, sizeof(*stub));
    if (!stub) {
        return NB_ERR_NOMEM;
    }
    stub->node = node;
    stub->payload_encoding = options ? options->payload_encoding : 0;
    stub->send = (options && options->send) ? options->send : nb_stub_default_send;
    stub->user = options ? options->user : NULL;
    *out = stub;
    return NB_OK;
}

void nb_stub_destroy(nb_stub_t *stub)
{
    free(stub);
}

nb_node_t *nb_stub_node(const nb_stub_t *stub)
{
    return stub ? stub->node : NULL;
}

/* ---- 调用 ---- */

static int nb_stub_payload_prepare_move(const nb_stub_t *stub,
                                        nb_owned_payload_t *payload,
                                        const uint8_t **data,
                                        size_t *len,
                                        uint8_t *encoding,
                                        nb_buf_t **owner)
{
    nb_payload_t view;

    if (!stub || !stub->node || !payload || !payload->buf || !data || !len || !encoding ||
        !owner) {
        if (payload) {
            nb_owned_payload_release(payload);
        }
        return NB_ERR_INVALID;
    }
    view = nb_owned_payload_view(payload);
    if (view.len > stub->node->max_msg_bytes || (view.len > 0 && !view.data)) {
        nb_owned_payload_release(payload);
        return NB_ERR_INVALID;
    }
    *data = (const uint8_t *)view.data;
    *len = view.len;
    *encoding = view.encoding ? view.encoding : stub->payload_encoding;
    *owner = nb_owned_payload_take(payload);
    return NB_OK;
}

static int nb_stub_call_event_impl(nb_stub_t *stub,
                               const char *service,
                               const char *method,
                               const nb_payload_t *payload,
                               nb_owned_payload_t *move,
                               const nb_metadata_t *metadata,
                               nb_request_id_t *out_request_id)
{
    nb_stub_task_t *t;
    nb_request_id_t id;
    const uint8_t *data = NULL;
    size_t len = 0;
    uint8_t encoding = 0;
    uint8_t metadata_buf[NB_METADATA_WIRE_MAX];
    uint16_t metadata_len = 0;
    nb_buf_t *owner = NULL;
    int rc;

    if (!stub || !stub->node || !service || !method) {
        if (move) {
            nb_owned_payload_release(move);
        }
        return NB_ERR_INVALID;
    }
    rc = move ? nb_stub_payload_prepare_move(stub, move, &data, &len, &encoding, &owner)
              : nb_stub_payload_prepare(stub, payload, &data, &len, &encoding);
    if (rc != NB_OK) {
        return rc;
    }
    if (nb_stub_metadata_encode(metadata, metadata_buf, sizeof(metadata_buf), &metadata_len) != NB_OK) {
        if (owner) {
            nb_buf_release(owner);
        }
        nb_log_error("metadata encode failed (too large?): service=%s method=%s", service, method);
        return NB_ERR_TOOBIG;
    }
    id = nb_stub_next_id(stub->node);
    if (nb_node_is_send_thread(stub->node) ||
        ntk_poller_is_current_thread(nb_node_pick_poller(stub->node, id))) {
        nb_stub_event_now(
            stub, id, service, method, data, len, encoding, metadata_buf, metadata_len, owner);
        if (out_request_id) {
            *out_request_id = id;
        }
        return NB_OK;
    }
    t = nb_stub_task_create(
        stub, id, service, method, data, len, encoding, metadata_buf, metadata_len, owner);
    if (!t) {
        return NB_ERR_NOMEM;
    }
    t->event = 1;
    rc = nb_stub_task_submit(stub, t);
    if (rc != NB_OK) {
        return rc;
    }
    if (out_request_id) {
        *out_request_id = id;
    }
    return NB_OK;
}

int nb_stub_call_event_with_metadata(nb_stub_t *stub,
                       const char *service,
                       const char *method,
                       const nb_payload_t *payload,
                       const nb_metadata_t *metadata,
                       nb_request_id_t *out_request_id)
{
    return nb_stub_call_event_impl(
        stub, service, method, payload, NULL, metadata, out_request_id);
}

int nb_stub_call_event_owned_with_metadata(nb_stub_t *stub,
                            const char *service,
                            const char *method,
                            nb_owned_payload_t *payload,
                            const nb_metadata_t *metadata,
                            nb_request_id_t *out_request_id)
{
    return nb_stub_call_event_impl(
        stub, service, method, NULL, payload, metadata, out_request_id);
}

int nb_stub_call_event(nb_stub_t *stub,
                    const char *service,
                    const char *method,
                    const nb_payload_t *payload,
                    nb_request_id_t *out_request_id)
{
    return nb_stub_call_event_with_metadata(stub, service, method, payload, NULL, out_request_id);
}

static int nb_stub_call_impl(nb_stub_t *stub,
                             const char *service,
                             const char *method,
                             const nb_payload_t *payload,
                             nb_owned_payload_t *move,
                             response_fn cb,
                             response_chunk_fn chunk_cb,
                             stream_done_fn done_cb,
                             int streaming,
                             void *user,
                             uint64_t timeout_ms,
                             nb_request_id_t *out_request_id,
                             const nb_metadata_t *metadata)
{
    nb_stub_task_t *t;
    nb_request_id_t id;
    const uint8_t *data = NULL;
    size_t len = 0;
    uint8_t encoding = 0;
    uint8_t metadata_buf[NB_METADATA_WIRE_MAX];
    uint16_t metadata_len = 0;
    nb_buf_t *owner = NULL;
    int rc;

    if (!stub || !stub->node || !service || !method) {
        if (move) {
            nb_owned_payload_release(move);
        }
        return NB_ERR_INVALID;
    }
    if (!streaming && !cb) {
        return NB_ERR_INVALID;
    }
    if (streaming && (!chunk_cb || !done_cb)) {
        return NB_ERR_INVALID;
    }
    rc = move ? nb_stub_payload_prepare_move(stub, move, &data, &len, &encoding, &owner)
              : nb_stub_payload_prepare(stub, payload, &data, &len, &encoding);
    if (rc != NB_OK) {
        return rc;
    }
    if (nb_stub_metadata_encode(metadata, metadata_buf, sizeof(metadata_buf), &metadata_len) != NB_OK) {
        if (owner) {
            nb_buf_release(owner);
        }
        nb_log_error("metadata encode failed (too large?): service=%s method=%s", service, method);
        return NB_ERR_TOOBIG;
    }
    id = nb_stub_next_id(stub->node);

    if (ntk_poller_is_current_thread(nb_node_pick_poller(stub->node, id))) {
        nb_stub_call_now(stub,
                      NULL,
                      owner,
                      id,
                      service,
                      method,
                      data,
                      len,
                      encoding,
                      cb,
                      chunk_cb,
                      done_cb,
                      streaming,
                      user,
                      timeout_ms,
                      metadata_buf,
                      metadata_len);
        owner = NULL;
        if (out_request_id) {
            *out_request_id = id;
        }
        return NB_OK;
    }

    t = nb_stub_task_create(
        stub, id, service, method, data, len, encoding, metadata_buf, metadata_len, owner);
    if (!t) {
        return NB_ERR_NOMEM;
    }
    t->timeout_ms = timeout_ms;
    t->cb = cb;
    t->chunk_cb = chunk_cb;
    t->done_cb = done_cb;
    t->streaming = streaming;
    t->user = user;
    rc = nb_stub_task_submit(stub, t);
    if (rc != NB_OK) {
        return rc;
    }
    if (out_request_id) {
        *out_request_id = id;
    }
    return NB_OK;
}

int nb_stub_call_ex(nb_stub_t *stub,
                     const char *service,
                     const char *method,
                     const nb_payload_t *payload,
                     response_fn cb,
                     response_chunk_fn chunk_cb,
                     stream_done_fn done_cb,
                     int streaming,
                     void *user,
                     uint64_t timeout_ms,
                     nb_request_id_t *out_request_id,
                     const nb_metadata_t *metadata)
{
    return nb_stub_call_impl(stub,
                             service,
                             method,
                             payload,
                             NULL,
                             cb,
                             chunk_cb,
                             done_cb,
                             streaming,
                             user,
                             timeout_ms,
                             out_request_id,
                             metadata);
}

int nb_stub_call_owned(nb_stub_t *stub,
                       const char *service,
                       const char *method,
                       nb_owned_payload_t *payload,
                       const nb_metadata_t *metadata,
                       response_fn cb,
                       response_chunk_fn chunk_cb,
                       stream_done_fn done_cb,
                       bool streaming,
                       void *user,
                       uint64_t timeout_ms,
                       nb_request_id_t *out_request_id)
{
    return nb_stub_call_impl(stub,
                             service,
                             method,
                             NULL,
                             payload,
                             cb,
                             chunk_cb,
                             done_cb,
                             streaming,
                             user,
                             timeout_ms,
                             out_request_id,
                             metadata);
}

int nb_stub_call_owned_ex(nb_stub_t *stub,
                          const char *service,
                          const char *method,
                          nb_owned_payload_t *payload,
                          const nb_stub_call_options_t *options)
{
    static const nb_stub_call_options_t defaults = NB_STUB_CALL_OPTIONS_INIT;
    const nb_stub_call_options_t *o = options ? options : &defaults;

    if (!o->cb && !o->chunk_cb) {
        return NB_ERR_INVALID;
    }
    return nb_stub_call_owned(stub,
                              service,
                              method,
                              payload,
                              o->metadata,
                              o->cb,
                              o->chunk_cb,
                              o->done_cb,
                              o->streaming,
                              o->user,
                              o->timeout_ms,
                              o->out_request_id);
}

int nb_stub_call_async(nb_stub_t *stub,
                    const char *service,
                    const char *method,
                    const nb_payload_t *payload,
                    response_fn cb,
                    void *user,
                    uint64_t timeout_ms,
                    nb_request_id_t *out_request_id)
{
    return nb_stub_call_ex(
        stub, service, method, payload, cb, NULL, NULL, 0, user, timeout_ms, out_request_id, NULL);
}

int nb_stub_call_async_with_metadata(nb_stub_t *stub,
                       const char *service,
                       const char *method,
                       const nb_payload_t *payload,
                       const nb_metadata_t *metadata,
                       response_fn cb,
                       void *user,
                       uint64_t timeout_ms,
                       nb_request_id_t *out_request_id)
{
    return nb_stub_call_ex(stub,
                            service,
                            method,
                            payload,
                            cb,
                            NULL,
                            NULL,
                            0,
                            user,
                            timeout_ms,
                            out_request_id,
                            metadata);
}

/* ---- 批量取消（销毁前调用，避免回调访问已释放对象） ---- */

/*
 * 批量取消：pending 分散在各 poller 的 lane 上，只允许归属 poller 取消自己的
 * lane。ctx 由各 poller 的取消任务共享（原子 remaining + sem），全部完成后由
 * 调用方回收；超时不回收，避免任务仍在引用 ctx 时 UAF。
 */
typedef struct nb_stub_cancel_ctx {
    nb_stub_t *stub;
    nb_node_t *node;
    int status;
    ntk_sem *sem;
    uint32_t remaining;
} nb_stub_cancel_ctx_t;

typedef struct nb_stub_cancel_task {
    nb_stub_cancel_ctx_t *ctx;
} nb_stub_cancel_task_t;

/* remaining 归零时唤醒等待方；所有递减路径都必须经过这里，保证
 * remaining==0 与 sem_post 之间存在 happen-before，调用方才能安全回收 ctx。 */
static void nb_stub_cancel_ctx_done(nb_stub_cancel_ctx_t *ctx)
{
    if (nb_atomic_sub_u32(&ctx->remaining, 1u) == 1u) {
        if (ctx->sem) {
            ntk_sem_post(ctx->sem, 1);
        }
    }
}

/* 在 poller 线程内取消本 lane 上属于该 stub 的 pending。 */
static void nb_stub_cancel_lane_now(nb_stub_cancel_ctx_t *ctx, int idx)
{
    nb_pending_t *p;
    if (idx >= 0) {
        p = ctx->node->pending_lanes[idx];
        while (p) {
            nb_pending_t *next = p->lane_next;
            if (p->used && p->owner == ctx->stub) {
                nb_pending_invoke(p, ctx->status, NULL);
            }
            p = next;
        }
    }
    nb_stub_cancel_ctx_done(ctx);
}

static void nb_stub_cancel_lane_fn(void *user)
{
    nb_stub_cancel_task_t *t = (nb_stub_cancel_task_t *)user;
    nb_stub_cancel_ctx_t *ctx = t->ctx;
    int idx = nb_node_poller_index(ctx->node, ntk_poller_current());
    nb_stub_cancel_lane_now(ctx, idx);
    free(t);
}

int nb_stub_cancel_all(nb_stub_t *stub, int status)
{
    nb_node_t *node;
    nb_stub_cancel_ctx_t *ctx;
    ntk_sem *sem;
    unsigned i;
    int cur_idx;
    if (!stub || !(node = stub->node)) {
        return NB_ERR_INVALID;
    }
    if (node->shutting_down) {
        return NB_OK; /* poller 已停，回调不会再运行，无需取消 */
    }
    if (node->io_count == 0) {
        return NB_OK;
    }
    sem = ntk_sem_create(0);
    if (!sem) {
        return NB_ERR_NOMEM;
    }
    ctx = (nb_stub_cancel_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        ntk_sem_destroy(sem);
        return NB_ERR_NOMEM;
    }
    ctx->stub = stub;
    ctx->node = node;
    ctx->status = status;
    ctx->sem = sem;
    ctx->remaining = node->io_count;

    /* 当前线程若本身是某个 poller，则本 lane 内联处理，避免自等待死锁。 */
    cur_idx = nb_node_poller_index(node, ntk_poller_current());
    if (cur_idx >= 0) {
        nb_stub_cancel_lane_now(ctx, cur_idx);
    }
    for (i = 0; i < node->io_count; i++) {
        nb_stub_cancel_task_t *t;
        if ((int)i == cur_idx) {
            continue;
        }
        t = (nb_stub_cancel_task_t *)calloc(1, sizeof(*t));
        if (!t) {
            nb_stub_cancel_ctx_done(ctx);
            continue;
        }
        t->ctx = ctx;
        if (ntk_poller_async(node->io_pollers[i], nb_stub_cancel_lane_fn, t, 1) != NTK_OK) {
            free(t);
            nb_stub_cancel_ctx_done(ctx);
        }
    }
    /* 必须经过 sem：即便 remaining 已经归零，也由最后完成的递减路径 post，
     * 直接回收 ctx/sem 会与 poller 任务形成 data race（UAF）。 */
    if (ntk_sem_timedwait(sem, 5000) != NTK_OK) {
        /* 超时：任务可能仍在执行，sem/ctx 交给最后完成的任务回收，避免 UAF。 */
        return NB_ERR_TIMEOUT;
    }
    ntk_sem_destroy(sem);
    free(ctx);
    return NB_OK;
}
