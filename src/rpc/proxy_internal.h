#ifndef NB_PROXY_INTERNAL_H
#define NB_PROXY_INTERNAL_H

/* 客户端内部结构：proxy、stub、流式会话与调用上下文。 */

#include <nb/proxy.h>

#include <stddef.h>
#include <stdint.h>

#include "caps.h"
#include "pending.h"
#include "atomic.h"
#include "stub_internal.h"

struct nb_proxy {
    nb_node_t *node;
    uint8_t payload_encoding;
    uint32_t prefer_stream_threshold;
    nb_stub_t *stub;
    nb_spinlock_t endpoint_lock; /* 保护 endpoint / has_endpoint（setter vs poller 读） */
    int has_endpoint;
    nb_endpoint_t endpoint;
    int has_interceptor;
    nb_proxy_interceptor_t interceptor;
};

struct nb_stream {
    nb_proxy_t *proxy;
    nb_stub_t *stub; /* open 后只依赖 stub 指针做 owner 标记，不再解引用 proxy */
    nb_node_t *node;
    nb_request_id_t request_id;
    uint32_t stream_id;
    char service[NB_SERVICE_NAME_MAX];
    char method[NB_METHOD_NAME_MAX];
    nb_endpoint_t endpoint;
    nb_response_fn cb;
    nb_response_chunk_fn chunk_cb;
    nb_stream_done_fn done_cb;
    void *user;
    int bidi;
    uint64_t timeout_ms;
    uint64_t next_offset;
    int opened;
    int sent_any;
    int send_closed;
    int closed;
};

int nb_proxy_resolve(nb_proxy_t *proxy,
                      const char *service,
                      int prefer_stream,
                      nb_endpoint_t *out);

#endif /* NB_PROXY_INTERNAL_H */
