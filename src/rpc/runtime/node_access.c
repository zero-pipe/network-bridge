/* node 访问门面的实现（供发现等 node 作用域后端使用）。 */
#include "node_access.h"

#include "node_internal.h"
#include "util.h"

uint32_t nb_node_id(const nb_node_t *node)
{
    return node ? node->node_id : 0;
}

const char *nb_node_discovery_host(const nb_node_t *node)
{
    return node ? node->discovery_host : NULL;
}

uint32_t nb_node_ttl_ms(const nb_node_t *node)
{
    return node ? node->node_ttl_ms : 0;
}

uint32_t nb_node_discovery_formation(const nb_node_t *node)
{
    return node ? node->discovery_formation : 0;
}

uint32_t nb_node_hello_interval_ms(const nb_node_t *node)
{
    return node ? node->hello_interval_ms : 0;
}

uint32_t nb_node_discovery_service_mask(const nb_node_t *node)
{
    return node ? node->discovery_service_mask : 0;
}

zn_poller *nb_node_poller(const nb_node_t *node)
{
    return node ? node->poller : NULL;
}

zn_poller_pool *nb_node_pool(const nb_node_t *node)
{
    return node ? node->pool : NULL;
}

/* 池里 owner_thread == 当前线程的 poller；没有则 NULL。 */
static zn_poller *nb_pool_current(const zn_poller_pool *pool)
{
    unsigned n;
    unsigned i;

    if (!pool) {
        return NULL;
    }
    n = zn_poller_pool_size(pool);
    for (i = 0; i < n; i++) {
        zn_poller *p = zn_poller_pool_at(pool, i);

        if (p && zn_poller_is_current_thread(p)) {
            return p;
        }
    }
    return NULL;
}

zn_poller *nb_node_current_poller(const nb_node_t *node)
{
    zn_poller *p;

    if (!node) {
        return NULL;
    }
    p = nb_pool_current(node->pool);
    if (p) {
        return p;
    }
    return nb_pool_current(node->send_pool);
}

nb_registry_t *nb_node_registry(nb_node_t *node)
{
    return node ? &node->registry : NULL;
}

nb_discovery_state_t *nb_node_discovery(nb_node_t *node)
{
    return node ? &node->discovery : NULL;
}

void nb_node_set_discovery_port(nb_node_t *node, uint16_t port)
{
    if (node) {
        node->discovery_port = port;
    }
}

size_t nb_node_service_count(const nb_node_t *node)
{
    size_t n = 0;
    const nb_service_t *svc;
    if (!node) {
        return 0;
    }
    if (node->services_lock) {
        zb_mutex_lock((zb_mutex *)node->services_lock);
    }
    for (svc = node->services; svc; svc = svc->next) {
        if (!svc->removed) {
            n++;
        }
    }
    if (node->services_lock) {
        zb_mutex_unlock((zb_mutex *)node->services_lock);
    }
    return n;
}

const char *nb_node_service_name(const nb_node_t *node, size_t index)
{
    const nb_service_t *svc;
    if (!node) {
        return NULL;
    }
    if (node->services_lock) {
        zb_mutex_lock((zb_mutex *)node->services_lock);
    }
    for (svc = node->services; svc; svc = svc->next) {
        if (svc->removed) {
            continue;
        }
        if (index == 0) {
            if (node->services_lock) {
                zb_mutex_unlock((zb_mutex *)node->services_lock);
            }
            return svc->name;
        }
        index--;
    }
    if (node->services_lock) {
        zb_mutex_unlock((zb_mutex *)node->services_lock);
    }
    return NULL;
}

const char *nb_node_binding_scheme(const nb_node_t *node, size_t index)
{
    if (!node || index >= node->transport_count || !node->transports[index]) {
        return NULL;
    }
    return node->transports[index]->scheme;
}

uint16_t nb_node_binding_port(const nb_node_t *node, size_t index)
{
    if (!node || index >= node->transport_count || !node->transports[index]) {
        return 0;
    }
    return node->transports[index]->port;
}

int nb_node_binding_uri(const nb_node_t *node, size_t index, char *out, size_t cap)
{
    if (!node || index >= node->transport_count || !node->transports[index] || !out || cap == 0) {
        return NB_ERR_INVALID;
    }
    if (!node->transports[index]->endpoint_uri[0]) {
        return NB_ERR_NOTFOUND;
    }
    nb_copy_str(out, cap, node->transports[index]->endpoint_uri, "");
    return out[0] ? NB_OK : NB_ERR_TOOBIG;
}
