/* mesh 服务发现：nb 适配层。
 *
 * 协议、分帧、CRC、序号/epoch、哈希表都在 ntk_discovery 库里；这里只做三件事：
 *   1. 把本地服务映射成 key（服务名），把 binding 映射成 peer 端点；
 *   2. 把库的对端/键回调写进 node registry；
 *   3. 实现 registry backend 的 start/stop/tick/announce。
 */
#include "discovery.h"

#include <nb/rpc/node.h>

#include "node_access.h"
#include "registry_backend.h"
#include "util.h"

#define NB_LOG_TAG "discovery"
#include <nb/base/log.h>

#include <stdio.h>
#include <string.h>

/* 实例的前 4 字节承载 nb 的 32 位 node id；其余字节保持 0。同一 node id 的
 * 进程重启会被库用 epoch 识别为新一次启动，不会产生重复对端。 */
static uint32_t disc_instance_node_id(const uint8_t instance[NTK_DISC_INSTANCE_SIZE])
{
    return (uint32_t)instance[0] | ((uint32_t)instance[1] << 8) |
           ((uint32_t)instance[2] << 16) | ((uint32_t)instance[3] << 24);
}

static void disc_instance_from_node(uint32_t node_id,
                                    uint8_t instance[NTK_DISC_INSTANCE_SIZE])
{
    memset(instance, 0, NTK_DISC_INSTANCE_SIZE);
    instance[0] = (uint8_t)(node_id & 0xffu);
    instance[1] = (uint8_t)((node_id >> 8) & 0xffu);
    instance[2] = (uint8_t)((node_id >> 16) & 0xffu);
    instance[3] = (uint8_t)((node_id >> 24) & 0xffu);
}

/* Local bindings -> endpoints carried in every HELLO. */
static size_t disc_local_endpoints(nb_node_t *node,
                                   ntk_disc_endpoint_t *out,
                                   size_t max)
{
    size_t count = 0;
    size_t bindings = nb_node_binding_count(node);
    size_t i;

    for (i = 0; i < bindings && count < max; i++) {
        char uri[NB_ENDPOINT_URI_MAX];

        if (nb_node_binding_uri(node, i, uri, sizeof(uri)) == NB_OK && uri[0]) {
            memset(&out[count], 0, sizeof(out[count]));
            out[count].kind = 0;
            nb_copy_str(out[count].uri, sizeof(out[count].uri), uri, "");
            count++;
        }
    }
    return count;
}

/* Publish the current local state (services as keys, bindings as endpoints). */
static void disc_sync_local(nb_node_t *node)
{
    nb_discovery_state_t *st = nb_node_discovery(node);
    ntk_disc_endpoint_t eps[NTK_DISC_MAX_ENDPOINTS];
    size_t ep_count;
    size_t services;
    size_t i;

    if (!st || !st->disc) {
        return;
    }
    (void)ntk_discovery_clear_keys(st->disc);
    ep_count = disc_local_endpoints(node, eps, NTK_DISC_MAX_ENDPOINTS);
    (void)ntk_discovery_set_endpoints(st->disc, eps, ep_count);
    services = nb_node_service_count(node);
    for (i = 0; i < services; i++) {
        const char *name = nb_node_service_name(node, i);

        if (name && name[0]) {
            /* The RPC adapter uses the service name as the key and keeps the
             * endpoints on the peer; the value stays empty. */
            (void)ntk_discovery_advertise(st->disc, name, NULL, 0, 0);
        }
    }
}

static void disc_registry_replace(nb_node_t *node,
                                  uint32_t node_id,
                                  const char *instance_name,
                                  const ntk_disc_peer_info_t *peer,
                                  const ntk_disc_key_info_t *keys,
                                  size_t key_count)
{
    const char *services[NB_DISCOVERY_MAX_SERVICES_PER_PEER];
    nb_endpoint_t endpoints[NB_MAX_BINDINGS];
    size_t endpoint_count = 0;
    size_t i;

    for (i = 0; i < key_count; i++) {
        services[i] = keys[i].key;
    }
    for (i = 0; i < peer->endpoint_count && endpoint_count < NB_MAX_BINDINGS; i++) {
        nb_endpoint_t parsed;

        if (nb_endpoint_parse(peer->endpoints[i].uri, &parsed) != NB_OK) {
            continue;
        }
        if (nb_endpoint_is_network(&parsed)) {
            /* The binding host is usually 0.0.0.0 or a local address; the
             * datagram source is authoritative for a remote node. */
            nb_copy_str(parsed.host, sizeof(parsed.host), peer->host, "0.0.0.0");
            (void)nb_endpoint_refresh(&parsed);
        }
        endpoints[endpoint_count++] = parsed;
    }
    if (endpoint_count == 0) {
        nb_log_debug("mesh: peer %08x has no usable endpoint", (unsigned)node_id);
        return;
    }
    nb_log_debug("mesh: peer %08x %s services=%zu endpoints=%zu",
                (unsigned)node_id,
                instance_name,
                key_count,
                endpoint_count);
    (void)nb_registry_replace_node(nb_node_registry(node),
                                   node_id,
                                   instance_name,
                                   services,
                                   key_count,
                                   endpoints,
                                   endpoint_count,
                                   nb_now_ms());
}

static void disc_on_peer(void *user, const ntk_disc_peer_info_t *peer, int removed)
{
    nb_node_t *node = (nb_node_t *)user;
    nb_discovery_state_t *st;
    ntk_disc_key_info_t keys[NB_DISCOVERY_MAX_SERVICES_PER_PEER];
    uint32_t node_id;
    char instance_name[NB_NAME_MAX];
    size_t key_count;
    int n;

    if (!node || !peer) {
        return;
    }
    st = nb_node_discovery(node);
    node_id = disc_instance_node_id(peer->instance);
    if (removed) {
        nb_log_debug("mesh: peer %08x removed", (unsigned)node_id);
        nb_registry_remove_node(nb_node_registry(node), node_id);
        return;
    }
    key_count = st && st->disc
                    ? ntk_discovery_peer_keys(st->disc, peer->instance, keys,
                                              NB_DISCOVERY_MAX_SERVICES_PER_PEER)
                    : 0;
    n = snprintf(instance_name,
                 sizeof(instance_name),
                 "%s-%08x",
                 peer->name[0] ? peer->name : "node",
                 (unsigned)node_id);
    if (n <= 0 || (size_t)n >= sizeof(instance_name)) {
        return;
    }
    disc_registry_replace(node, node_id, instance_name, peer, keys, key_count);
}

int nb_discovery_init(nb_node_t *node)
{
    nb_discovery_state_t *st = nb_node_discovery(node);
    ntk_discovery_config_t config;
    uint8_t instance[NTK_DISC_INSTANCE_SIZE];
    uint16_t port;

    if (!st) {
        return NB_ERR_INVALID;
    }
    memset(&config, 0, sizeof(config));
    ntk_discovery_config_init(&config);
    config.bind_host = "0.0.0.0";
    config.port = nb_node_discovery_port(node);
    config.broadcast = nb_node_discovery_host(node);
    if (!config.broadcast || !config.broadcast[0]) {
        config.broadcast = "255.255.255.255";
    }
    config.ttl_ms = nb_node_ttl_ms(node);
    config.max_peers = 256;
    config.max_keys = 4096;
    config.callbacks.on_peer = disc_on_peer;
    config.user = node;

    st->disc = ntk_discovery_create(nb_node_poller(node), &config);
    if (!st->disc) {
        return NB_ERR_IO;
    }
    disc_instance_from_node(nb_node_id(node), instance);
    (void)ntk_discovery_set_identity(st->disc, instance, nb_node_name(node));
    port = ntk_discovery_port(st->disc);
    if (port != 0) {
        nb_node_set_discovery_port(node, port);
    }
    disc_sync_local(node);
    nb_log_debug("mesh: init port=%u name=%s bindings=%zu",
                (unsigned)port,
                nb_node_name(node),
                nb_node_binding_count(node));
    return NB_OK;
}

void nb_discovery_shutdown(nb_node_t *node)
{
    nb_discovery_state_t *st = nb_node_discovery(node);
    if (st && st->disc) {
        ntk_discovery_destroy(st->disc);
        st->disc = NULL;
    }
}

void nb_discovery_send_hello(nb_node_t *node, int bye)
{
    nb_discovery_state_t *st = nb_node_discovery(node);
    int i;

    if (!st || !st->disc) {
        return;
    }
    if (st->targets_lock) {
        ntk_mutex_lock(st->targets_lock);
        for (i = 0; i < (int)NB_DISCOVERY_MAX_TARGETS; i++) {
            if (st->targets[i].used) {
                (void)ntk_discovery_add_peer(st->disc,
                                             st->targets[i].ip,
                                             st->targets[i].port);
            }
        }
        ntk_mutex_unlock(st->targets_lock);
    }
    if (bye) {
        nb_log_debug("mesh: bye");
        (void)ntk_discovery_bye(st->disc);
        return;
    }
    disc_sync_local(node);
    nb_log_debug("mesh: announce port=%u", (unsigned)ntk_discovery_port(st->disc));
    (void)ntk_discovery_announce(st->disc);
}

void nb_discovery_tick(nb_node_t *node, uint64_t now_ms)
{
    nb_discovery_state_t *st = nb_node_discovery(node);

    /* The node tick passes a monotonic clock; the library timestamps peers
     * with the wall clock, so translate here and keep the registry on the
     * node's monotonic clock. */
    (void)now_ms;
    if (st && st->disc) {
        ntk_discovery_tick(st->disc, ntk_wall_ms());
    }
    nb_registry_expire(nb_node_registry(node), nb_now_ms(), nb_node_ttl_ms(node));
}

int nb_discovery_find(nb_node_t *node,
                       const char *service,
                       char *ip_out,
                       size_t ip_cap,
                       uint16_t *port_out,
                       nb_transport_kind_t *transport_out)
{
    nb_endpoint_t endpoint;
    if (!node || !service || !ip_out || !port_out) {
        return NB_ERR_INVALID;
    }
    if (nb_registry_select(nb_node_registry(node), service, &endpoint) == NB_OK) {
        nb_copy_str(ip_out, ip_cap, endpoint.host, "0.0.0.0");
        *port_out = endpoint.port;
        if (transport_out) {
            *transport_out = endpoint.transport;
        }
        return NB_OK;
    }
    return NB_ERR_NOTFOUND;
}

static int nb_mesh_backend_start(nb_node_t *node);
static void nb_mesh_backend_stop(nb_node_t *node);
static void nb_mesh_backend_tick(nb_node_t *node, uint64_t now_ms);
static void nb_mesh_backend_announce(nb_node_t *node, int bye);

static const nb_registry_backend_ops_t nb_g_mesh_ops = {
    nb_mesh_backend_start,
    nb_mesh_backend_stop,
    nb_mesh_backend_tick,
    nb_mesh_backend_announce,
};

static const nb_registry_backend_t nb_g_mesh_backend = {
    "mesh",
    &nb_g_mesh_ops,
};

const nb_registry_backend_t *nb_registry_backend_mesh(void)
{
    return &nb_g_mesh_backend;
}

static int nb_mesh_backend_start(nb_node_t *node)
{
    return nb_discovery_init(node);
}

static void nb_mesh_backend_stop(nb_node_t *node)
{
    nb_discovery_shutdown(node);
}

static void nb_mesh_backend_tick(nb_node_t *node, uint64_t now_ms)
{
    nb_discovery_tick(node, now_ms);
}

static void nb_mesh_backend_announce(nb_node_t *node, int bye)
{
    nb_discovery_send_hello(node, bye);
}
