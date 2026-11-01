/*
 * 零拷贝发布（loan/borrow）。
 *
 * 一次 loan 在解析目标端点时就固定了传输，publish 时只做一次 envelope 前缀
 * 编码＋扇出，跳过普通 send 路径里把 payload 拷进 chunk 的那一次 memcpy。
 * 句柄由 core 分配、publish/release 归还，额度来自 provider 的段内 chunk。
 */
#include <nb/publish.h>

#include <stdlib.h>
#include <string.h>

#include "node_internal.h"
#include "proxy_internal.h"
#include "util.h"

struct nb_loan {
    nb_node_t *node;
    nb_transport_t *transport;
    void *handle;
    uint8_t *data;
    uint32_t capacity;
    uint8_t encoding;
    char route[NB_GLOBAL_MAX_ROUTE_BYTES];
};

/* 事件 id 只用于关联，分配方式与 stub 的 nb_stub_next_id 保持一致。 */
static nb_request_id_t nb_loan_next_id(nb_node_t *node)
{
    nb_request_id_t id;

    ntk_mutex_lock(node->id_lock);
    id = (nb_request_id_t)(node->next_request_id++);
    ntk_mutex_unlock(node->id_lock);
    return id ? id : 1;
}

int nb_publisher_loan(nb_publisher_t *pub,
                      const char *service,
                      const char *method,
                      uint32_t capacity,
                      nb_loan_t **out_loan,
                      uint8_t **out_data,
                      uint32_t *out_capacity)
{
    nb_endpoint_t target;
    nb_transport_t *transport;
    nb_loan_t *loan;
    void *handle = NULL;
    void *data = NULL;
    uint32_t cap = 0;
    char route[NB_GLOBAL_MAX_ROUTE_BYTES];
    int rc;

    if (!pub || !pub->node || !service || !method || !out_loan || !out_data ||
        !out_capacity || capacity == 0) {
        return NB_ERR_INVALID;
    }
    *out_loan = NULL;
    if (nb_route_format(route, sizeof(route), service, method) != NB_OK) {
        return NB_ERR_INVALID;
    }
    if (nb_proxy_resolve(pub, service, 0, &target) != NB_OK) {
        return NB_ERR_NOTFOUND;
    }
    transport = nb_node_find_transport(pub->node, &target);
    if (!transport) {
        return NB_ERR_NOTFOUND;
    }
    rc = nb_transport_loan(transport, route, capacity, &data, &cap, &handle);
    if (rc != NB_OK) {
        return rc;
    }
    loan = (nb_loan_t *)malloc(sizeof(*loan));
    if (!loan) {
        nb_transport_release_loan(transport, handle);
        return NB_ERR_NOMEM;
    }
    memset(loan, 0, sizeof(*loan));
    loan->node = pub->node;
    loan->transport = transport;
    loan->handle = handle;
    loan->data = (uint8_t *)data;
    loan->capacity = cap;
    loan->encoding = pub->payload_encoding;
    memcpy(loan->route, route, sizeof(route));
    *out_data = loan->data;
    *out_capacity = cap;
    *out_loan = loan;
    return NB_OK;
}

int nb_publisher_publish_loan(nb_loan_t *loan,
                              uint32_t length,
                              uint8_t encoding,
                              nb_request_id_t *out_request_id)
{
    nb_message_t msg;
    nb_request_id_t id;
    int rc;

    if (!loan) {
        return NB_ERR_INVALID;
    }
    if (length > loan->capacity) {
        nb_transport_release_loan(loan->transport, loan->handle);
        free(loan);
        return NB_ERR_TOOBIG;
    }
    id = nb_loan_next_id(loan->node);
    memset(&msg, 0, sizeof(msg));
    msg.kind = NB_MSG_KIND_EVENT;
    msg.request_id = id;
    msg.route = loan->route;
    msg.payload.data = loan->data;
    msg.payload.len = length;
    msg.payload.encoding = encoding ? encoding : loan->encoding;
    /* send_loan 无论成败都接管 chunk 所有权，句柄不再可用。 */
    rc = nb_transport_send_loan(loan->transport, loan->route, loan->handle, &msg);
    free(loan);
    if (out_request_id) {
        *out_request_id = id;
    }
    return rc;
}

void nb_publisher_loan_release(nb_loan_t *loan)
{
    if (!loan) {
        return;
    }
    nb_transport_release_loan(loan->transport, loan->handle);
    free(loan);
}
