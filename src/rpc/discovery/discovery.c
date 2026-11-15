/* mesh 服务发现：nb_mesh 的适配层。
 *
 * 三个要点：
 *   1. 位图快速路径：心跳带 64 位服务掩码（bit = 服务名哈希低位）。位命中
 *      即认为对端提供该服务，registry 直接映射，零单播。
 *   2. 按需内省：掩码为 0（对端没通告）或版本号与本地缓存不一致时，单播拉
 *      一次对端服务清单并缓存；版本号变了才重拉。
 *   3. 端点如何拼：心跳带 rpc_port 与显式 IPv4；scheme 取本节点主绑定
 *      （tcp 优先），因此编队内约定“每个节点一个 RPC 服务端口”。
 *
 * 注意：这里从不拿“对端版本号”和“本地版本号”相比——两者本来就不同（各
 * 自提供不同的服务集）。版本号只用于判断本地缓存是否过期。
 */
#include "discovery.h"

#include <nb/rpc/node.h>

#include "node_access.h"
#include "registry_backend.h"
#include "util.h"

#define NB_LOG_TAG "discovery"
#include <nb/base/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RPC node id 是 32 位；mesh 用 64 位 node_id，高 32 位保留给将来的编队序号。 */
static uint64_t disc_node_id64(uint32_t node_id)
{
    return (uint64_t)node_id;
}

static uint32_t disc_node_id32(uint64_t node_id)
{
    return (uint32_t)(node_id & 0xffffffffu);
}

/* 本节点对外宣告的 RPC 服务端口与 scheme：取主绑定，tcp 优先。 */
static void disc_primary_endpoint(nb_node_t *node, char *scheme, size_t scheme_cap,
                                  uint16_t *port)
{
    size_t count = nb_node_binding_count(node);
    size_t i;
    const char *picked = NULL;
    uint16_t picked_port = 0;

    for (i = 0; i < count; i++) {
        const char *s = nb_node_binding_scheme(node, i);
        uint16_t p = nb_node_binding_port(node, i);

        if (!s || p == 0) {
            continue;
        }
        if (strcmp(s, "tcp") == 0) {
            picked = s;
            picked_port = p;
            break;
        }
        if (!picked) {
            picked = s;
            picked_port = p;
        }
    }
    if (picked) {
        nb_copy_str(scheme, scheme_cap, picked, "udp");
        *port = picked_port;
    } else {
        nb_copy_str(scheme, scheme_cap, "udp", "udp");
        *port = 0;
    }
}

/* 本地服务清单（堆上指针数组，调用方用 disc_free_names 释放）。 */
static char **disc_local_names(nb_node_t *node, size_t *count_out)
{
    size_t count = nb_node_service_count(node);
    char **names;
    size_t i;

    *count_out = 0;
    if (count == 0) {
        return NULL;
    }
    names = (char **)calloc(count, sizeof(*names));
    if (!names) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        const char *name = nb_node_service_name(node, i);

        if (name && name[0]) {
            names[*count_out] = (char *)name;
            (*count_out)++;
        }
    }
    return names;
}

static void disc_free_names(char **names)
{
    free(names);
}

/* 本地服务位图：bit = 服务名哈希低位。 */
static void disc_update_mask(nb_discovery_state_t *st,
                             const char *const *names,
                             size_t count)
{
    uint64_t mask = 0;
    size_t i;

    for (i = 0; i < count; i++) {
        mask |= nb_mesh_service_bit(names[i]);
    }
    (void)nb_mesh_set_service_mask(st->mesh, mask);
}

/* 把“服务名清单 + 对端”写入 registry。 */
static void disc_registry_apply(nb_node_t *node,
                                uint32_t node_id,
                                const char *const *services,
                                size_t service_count,
                                const char *host,
                                uint16_t rpc_port,
                                const char *scheme)
{
    nb_endpoint_t endpoint;
    char instance_name[NB_NAME_MAX];
    int n;

    if (service_count == 0 || !host || !host[0] || rpc_port == 0) {
        return;
    }
    if (nb_endpoint_from_parts(&endpoint,
                               scheme && scheme[0] ? scheme : "udp",
                               host,
                               rpc_port,
                               nb_transport_kind_from_scheme(scheme && scheme[0] ? scheme : "udp")) !=
        NB_OK) {
        return;
    }
    n = snprintf(instance_name, sizeof(instance_name), "%08x", (unsigned)node_id);
    if (n <= 0 || (size_t)n >= sizeof(instance_name)) {
        return;
    }
    nb_log_debug("mesh: peer %08x services=%zu via %s:%u",
                 (unsigned)node_id,
                 service_count,
                 host,
                 (unsigned)rpc_port);
    (void)nb_registry_replace_node(nb_node_registry(node),
                                   node_id,
                                   instance_name,
                                   services,
                                   service_count,
                                   &endpoint,
                                   1,
                                   nb_now_ms());
}

/* 位图命中：只把对端掩码里声明过的服务映射过去。 */
static void disc_apply_from_mask(nb_node_t *node, const nb_mesh_peer_info_t *peer)
{
    char scheme[NB_SCHEME_MAX];
    uint16_t local_port = 0;
    char **names;
    const char *offered[NB_DISCOVERY_MAX_SERVICES_PER_PEER];
    size_t count = 0;
    size_t offered_count = 0;
    size_t i;

    disc_primary_endpoint(node, scheme, sizeof(scheme), &local_port);
    names = disc_local_names(node, &count);
    if (!names) {
        return;
    }
    for (i = 0; i < count && offered_count < NB_DISCOVERY_MAX_SERVICES_PER_PEER; i++) {
        if ((peer->service_mask & nb_mesh_service_bit(names[i])) != 0) {
            offered[offered_count++] = names[i];
        }
    }
    /* 一个都不命中：可能对端只是没通告掩码，交给按需内省。 */
    if (offered_count > 0) {
        disc_registry_apply(node,
                            disc_node_id32(peer->node_id),
                            offered,
                            offered_count,
                            peer->host,
                            peer->rpc_port,
                            scheme);
    }
    disc_free_names(names);
}

static void disc_on_peer(void *user, const nb_mesh_peer_info_t *peer, int removed)
{
    nb_node_t *node = (nb_node_t *)user;
    uint32_t node_id;

    if (!node || !peer) {
        return;
    }
    node_id = disc_node_id32(peer->node_id);
    if (removed) {
        nb_log_debug("mesh: peer %08x removed", (unsigned)node_id);
        nb_registry_remove_node(nb_node_registry(node), node_id);
        return;
    }
    nb_log_debug("mesh: peer %08x %s rpc_port=%u mask=0x%016llx hash=%08x",
                 (unsigned)node_id,
                 peer->host,
                 (unsigned)peer->rpc_port,
                 (unsigned long long)peer->service_mask,
                 (unsigned)peer->service_hash);
    if (peer->service_mask != 0) {
        disc_apply_from_mask(node, peer);
    }
    /* 掩码为 0（对端没通告）时不做任何事：等真正要调用时按需内省。 */
}

static void disc_on_manifest(void *user,
                             const nb_mesh_peer_info_t *peer,
                             const char *const *services,
                             size_t count)
{
    nb_node_t *node = (nb_node_t *)user;
    char scheme[NB_SCHEME_MAX];
    uint16_t local_port = 0;

    if (!node || !peer || !services || count == 0) {
        return;
    }
    disc_primary_endpoint(node, scheme, sizeof(scheme), &local_port);
    nb_log_debug("mesh: manifest from %08x: %zu services",
                 (unsigned)disc_node_id32(peer->node_id),
                 count);
    disc_registry_apply(node,
                        disc_node_id32(peer->node_id),
                        services,
                        count,
                        peer->host,
                        peer->rpc_port,
                        scheme);
}

int nb_discovery_init(nb_node_t *node)
{
    nb_discovery_state_t *st = nb_node_discovery(node);
    nb_mesh_config_t config;
    char scheme[NB_SCHEME_MAX];
    uint16_t rpc_port = 0;
    char **names;
    size_t name_count = 0;
    int rc = NB_MESH_OK;

    if (!st) {
        return NB_ERR_INVALID;
    }
    disc_primary_endpoint(node, scheme, sizeof(scheme), &rpc_port);

    memset(&config, 0, sizeof(config));
    nb_mesh_config_init(&config);
    config.bind_host = "0.0.0.0";
    config.port = nb_node_discovery_port(node);
    /* 空字符串 = 不加入组播，只用单播目标（本机测试常用）。 */
    config.multicast_group = nb_node_discovery_host(node);
    config.formation_id = nb_node_discovery_formation(node);
    config.node_id = disc_node_id64(nb_node_id(node));
    config.rpc_port = rpc_port;
    config.service_mask = nb_node_discovery_service_mask(node);
    config.heartbeat_ms = nb_node_hello_interval_ms(node);
    config.lease_ms = nb_node_ttl_ms(node);
    config.max_peers = NB_DISCOVERY_MAX_PEERS;
    config.callbacks.on_peer = disc_on_peer;
    config.callbacks.on_manifest = disc_on_manifest;
    config.user = node;

    st->mesh = nb_mesh_create(nb_node_poller(node), &config, &rc);
    if (!st->mesh) {
        nb_log_error("mesh: create failed: %s", nb_mesh_strerror(rc));
        return NB_ERR_IO;
    }
    names = disc_local_names(node, &name_count);
    (void)nb_mesh_set_services(st->mesh, (const char *const *)names, name_count);
    disc_update_mask(st, (const char *const *)names, name_count);
    disc_free_names(names);

    if (nb_mesh_port(st->mesh) != 0) {
        nb_node_set_discovery_port(node, nb_mesh_port(st->mesh));
    }
    nb_log_debug("mesh: init port=%u formation=%u node=%08x rpc_port=%u services=%zu",
                 (unsigned)nb_mesh_port(st->mesh),
                 (unsigned)nb_node_discovery_formation(node),
                 (unsigned)nb_node_id(node),
                 (unsigned)rpc_port,
                 name_count);
    return NB_OK;
}

void nb_discovery_shutdown(nb_node_t *node)
{
    nb_discovery_state_t *st = nb_node_discovery(node);
    if (st && st->mesh) {
        nb_mesh_destroy(st->mesh);
        st->mesh = NULL;
    }
}

void nb_discovery_send_hello(nb_node_t *node, int bye)
{
    nb_discovery_state_t *st = nb_node_discovery(node);
    char **names;
    size_t count = 0;
    int i;

    if (!st || !st->mesh) {
        return;
    }
    if (st->targets_lock) {
        zb_mutex_lock(st->targets_lock);
        for (i = 0; i < (int)NB_DISCOVERY_MAX_TARGETS; i++) {
            if (st->targets[i].used) {
                (void)nb_mesh_add_peer(st->mesh,
                                       st->targets[i].ip,
                                       st->targets[i].port);
            }
        }
        zb_mutex_unlock(st->targets_lock);
    }
    if (bye) {
        nb_log_debug("mesh: leave");
        (void)nb_mesh_leave(st->mesh);
        return;
    }
    /* 服务可能运行期增删：每次公告前刷新清单与 schema_hash。 */
    names = disc_local_names(node, &count);
    (void)nb_mesh_set_services(st->mesh, (const char *const *)names, count);
    disc_update_mask(st, (const char *const *)names, count);
    disc_free_names(names);
    (void)nb_mesh_announce(st->mesh);
}

void nb_discovery_tick(nb_node_t *node, uint64_t now_ms)
{
    nb_discovery_state_t *st = nb_node_discovery(node);

    /* mesh 内部用墙钟记录 last_seen；registry 用 node 的单调钟。 */
    (void)now_ms;
    if (st && st->mesh) {
        nb_mesh_tick(st->mesh, zn_wall_ms());
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
    nb_discovery_state_t *st;
    nb_mesh_peer_info_t peers[NB_DISCOVERY_MAX_PEERS];
    size_t count;
    size_t i;

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
    /*
     * 懒内省的触发点：本地没有这个服务，但可能有契约不一致、manifest 还没
     * 拉回来的对端。催它们立刻内省一次，调用方下一轮重试即可命中。
     */
    st = nb_node_discovery(node);
    if (st && st->mesh) {
        count = nb_mesh_peers(st->mesh, peers, NB_DISCOVERY_MAX_PEERS);
        for (i = 0; i < count; i++) {
            const nb_mesh_peer_info_t *peer = &peers[i];

            if (peer->manifest_state == NB_MESH_MANIFEST_READY) {
                /* 拉过了仍然没有这个服务：确实不提供。 */
                continue;
            }
            /* 缓存版本过期，或者对端根本没通告掩码：拉一次。 */
            if (peer->cached_service_hash != peer->service_hash ||
                peer->service_mask == 0) {
                (void)nb_mesh_request_manifest(st->mesh, peer->node_id);
            }
        }
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
