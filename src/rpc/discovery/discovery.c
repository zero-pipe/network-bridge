/* mesh 服务发现：nb_mesh 的薄适配层。
 *
 * 一、服务名 -> 位
 *   有共享 nb_service_table 时用服务 ID 定位（编译期一致的稳定编号），否则
 *   用 nb_mesh_service_bit(name) 的名字哈希位。两端对同一服务名算出同一位，
 *   于是"掩码命中 = 对端提供该服务"，调用路径零握手。
 *
 * 二、端点如何拼
 *   心跳里带 rpc_port 与显式 IPv4；scheme 取本节点主绑定（tcp 优先），因此
 *   编队内约定"每个节点一个 RPC 服务端口"。
 *
 * 三、为什么不预填 registry
 *   旧实现按"本地服务名 ∩ 对端掩码"往 registry 里塞条目，客户端本地没有这个
 *   服务名时永远学不到远端有它——异构服务集因此失明。现在改为按需查询：
 *   nb_node_resolve() 路由/registry 都不命中时直接问邻居表。
 */
#include "discovery.h"

#include <nb/rpc/node.h>
#include <nb/rpc/service_table.h>

#include "node_access.h"
#include "registry_backend.h"
#include "util.h"

#define NB_LOG_TAG "discovery"
#include <nb/base/log.h>

#include <string.h>

/* 本节点对外宣告的 RPC 服务端口与 scheme：取主绑定，tcp 优先。 */
/*
 * 本节点的主绑定：tcp 优先，其次任意一个。scheme/port 决定心跳里通告的服务
 * 端口；host 只对"非网络"scheme 有意义（shm 的 host 是 channel 名，不是 IP）。
 */
typedef struct disc_primary {
    char scheme[NB_SCHEME_MAX];
    char host[NB_HOST_MAX];
    uint16_t port;
} disc_primary_t;

static void disc_primary_endpoint(nb_node_t *node, disc_primary_t *out)
{
    size_t count = nb_node_binding_count(node);
    size_t i;
    size_t picked = SIZE_MAX;

    memset(out, 0, sizeof(*out));
    nb_copy_str(out->scheme, sizeof(out->scheme), "udp", "udp");
    for (i = 0; i < count; i++) {
        const char *s = nb_node_binding_scheme(node, i);
        uint16_t p = nb_node_binding_port(node, i);

        if (!s || p == 0) {
            continue;
        }
        if (strcmp(s, "tcp") == 0) {
            picked = i;
            break;
        }
        if (picked == SIZE_MAX) {
            picked = i;
        }
    }
    if (picked == SIZE_MAX) {
        return;
    }
    nb_copy_str(out->scheme, sizeof(out->scheme), nb_node_binding_scheme(node, picked), "udp");
    out->port = nb_node_binding_port(node, picked);
    /* shm 之类的自定义传输：地址不是 ip:port，需要 binding 里的 channel。 */
    if (nb_transport_kind_from_scheme(out->scheme) == NB_TRANSPORT_KIND_CUSTOM) {
        char uri[NB_ENDPOINT_URI_MAX];
        nb_endpoint_t parsed;

        if (nb_node_binding_uri(node, picked, uri, sizeof(uri)) == NB_OK &&
            nb_endpoint_parse(uri, &parsed) == NB_OK) {
            nb_copy_str(out->host, sizeof(out->host), parsed.host, "");
        }
    }
}

/* 服务名 -> 服务位：优先用共享服务表的服务 ID，否则用名字哈希位。 */
static uint32_t disc_service_bit(const char *service)
{
    uint16_t id = 0;

    if (!service) {
        return 0;
    }
    if (nb_service_table_find_service_id(service, &id) == NB_OK &&
        id < NB_MESH_SERVICE_BITS) {
        return 1u << id;
    }
    return nb_mesh_service_bit(service);
}

/* 本机服务位图：配置里显式给的位 + 本地已注册服务的位。 */
static uint32_t disc_local_mask(nb_node_t *node)
{
    uint32_t mask = nb_node_discovery_service_mask(node);
    size_t count = nb_node_service_count(node);
    size_t i;

    for (i = 0; i < count; i++) {
        const char *name = nb_node_service_name(node, i);

        if (name && name[0]) {
            mask |= disc_service_bit(name);
        }
    }
    return mask;
}

int nb_discovery_init(nb_node_t *node)
{
    nb_discovery_state_t *st = nb_node_discovery(node);
    nb_mesh_config_t config;
    disc_primary_t local;
    int rc;

    if (!st) {
        return NB_ERR_INVALID;
    }
    if (st->mesh) {
        return NB_OK;
    }
    disc_primary_endpoint(node, &local);

    nb_mesh_config_init(&config);
    config.formation_id = nb_node_discovery_formation(node);
    /* RPC node id 是 32 位；mesh 用 64 位，高 32 位留给将来的编队序号。 */
    config.node_id = (uint64_t)nb_node_id(node);
    config.rpc_port = local.port;
    config.service_mask = disc_local_mask(node);
    /* 组播组：NULL = 由 formation 推导；"" = 只走单播目标（本机测试常用）。 */
    config.multicast_group = nb_node_discovery_host(node);
    config.bind_host = "0.0.0.0";
    config.port = nb_node_discovery_port(node);
    config.heartbeat_ms = nb_node_hello_interval_ms(node);
    config.lease_ms = nb_node_ttl_ms(node);
    /* 0 = 库默认容量（8192 桶 / 8192 邻居）。 */
    config.max_peers = 0;

    rc = nb_mesh_start(&st->mesh, &config);
    if (rc != NB_MESH_OK) {
        st->mesh = NULL;
        nb_log_error("mesh: start failed: %s", nb_mesh_strerror(rc));
        return NB_ERR_IO;
    }
    if (nb_mesh_port(st->mesh) != 0) {
        nb_node_set_discovery_port(node, nb_mesh_port(st->mesh));
    }
    nb_log_debug("mesh: init port=%u formation=%u node=%08x rpc_port=%u mask=0x%08x",
                 (unsigned)nb_mesh_port(st->mesh),
                 (unsigned)config.formation_id,
                 (unsigned)nb_node_id(node),
                 (unsigned)local.port,
                 (unsigned)config.service_mask);
    return NB_OK;
}

void nb_discovery_shutdown(nb_node_t *node)
{
    nb_discovery_state_t *st = nb_node_discovery(node);

    if (st && st->mesh) {
        nb_mesh_stop(st->mesh);
        st->mesh = NULL;
    }
}

void nb_discovery_send_hello(nb_node_t *node, int bye)
{
    nb_discovery_state_t *st = nb_node_discovery(node);
    int i;

    if (!st || !st->mesh) {
        return;
    }
    if (bye) {
        nb_log_debug("mesh: leave");
        (void)nb_mesh_leave(st->mesh);
        return;
    }
    /* 服务可能在运行期增删：公告前刷新本机位图。 */
    (void)nb_mesh_set_service_mask(st->mesh, disc_local_mask(node));
    if (st->targets_lock) {
        zb_mutex_lock(st->targets_lock);
    }
    for (i = 0; i < (int)NB_DISCOVERY_MAX_TARGETS; i++) {
        if (st->targets[i].used) {
            (void)nb_mesh_add_peer(st->mesh, st->targets[i].ip, st->targets[i].port);
        }
    }
    if (st->targets_lock) {
        zb_mutex_unlock(st->targets_lock);
    }
    (void)nb_mesh_announce(st->mesh);
}

void nb_discovery_tick(nb_node_t *node, uint64_t now_ms)
{
    /* mesh 自己维护心跳/租约（内部 poller 线程），这里只做 registry 的兜底过期。 */
    (void)now_ms;
    nb_registry_expire(nb_node_registry(node), nb_now_ms(), nb_node_ttl_ms(node));
}

int nb_discovery_find(nb_node_t *node, const char *service, nb_endpoint_t *out)
{
    nb_discovery_state_t *st;
    nb_mesh_target_t target;
    disc_primary_t local;
    char host[NB_HOST_MAX];
    uint16_t port = 0;
    uint32_t bit;
    int rc;

    if (!node || !service || !out) {
        return NB_ERR_INVALID;
    }
    st = nb_node_discovery(node);
    if (!st || !st->mesh) {
        return NB_ERR_NOTFOUND;
    }
    bit = disc_service_bit(service);
    if (bit == 0) {
        return NB_ERR_NOTFOUND;
    }
    rc = nb_mesh_find(st->mesh, bit, &target);
    if (rc != NB_MESH_OK) {
        return NB_ERR_NOTFOUND;
    }
    if (target.rpc_port == 0) {
        /* 对端没通告 RPC 端口：无法调用。 */
        return NB_ERR_NOTFOUND;
    }
    disc_primary_endpoint(node, &local);
    /*
     * 网络传输用对端通告的 IP；shm 之类的自定义传输没有 IP，地址是
     * (channel, 节点 id)，channel 取自本节点的绑定，id 取自心跳里的端口。
     */
    if (nb_transport_kind_from_scheme(local.scheme) == NB_TRANSPORT_KIND_CUSTOM &&
        local.host[0]) {
        nb_copy_str(host, sizeof(host), local.host, target.host);
        port = target.rpc_port;
    } else if (nb_transport_kind_from_scheme(local.scheme) == NB_TRANSPORT_KIND_CUSTOM) {
        return NB_ERR_NOTFOUND;
    } else {
        nb_copy_str(host, sizeof(host), target.host, "");
        port = target.rpc_port;
    }
    if (nb_endpoint_from_parts(out,
                               local.scheme,
                               host,
                               port,
                               nb_transport_kind_from_scheme(local.scheme)) != NB_OK) {
        return NB_ERR_INVALID;
    }
    nb_log_debug("mesh: '%s' -> %s:%u (node %llx, bit 0x%08x)",
                 service,
                 host,
                 (unsigned)port,
                 (unsigned long long)target.node_id,
                 (unsigned)bit);
    return NB_OK;
}

int nb_discovery_add_peer(nb_node_t *node, const char *ip, uint16_t discovery_port)
{
    nb_discovery_state_t *st;
    int i;

    if (!node || !ip || !ip[0] || discovery_port == 0) {
        return NB_ERR_INVALID;
    }
    if (strlen(ip) >= NB_HOST_MAX) {
        return NB_ERR_TOOBIG;
    }
    st = nb_node_discovery(node);
    if (!st || !st->targets_lock) {
        return NB_ERR_INVALID;
    }
    zb_mutex_lock(st->targets_lock);
    for (i = 0; i < (int)NB_DISCOVERY_MAX_TARGETS; i++) {
        if (st->targets[i].used && st->targets[i].port == discovery_port &&
            strcmp(st->targets[i].ip, ip) == 0) {
            zb_mutex_unlock(st->targets_lock);
            return NB_ERR_ALREADY;
        }
    }
    for (i = 0; i < (int)NB_DISCOVERY_MAX_TARGETS; i++) {
        if (!st->targets[i].used) {
            st->targets[i].used = 1;
            nb_copy_str(st->targets[i].ip, sizeof(st->targets[i].ip), ip, NULL);
            st->targets[i].port = discovery_port;
            zb_mutex_unlock(st->targets_lock);
            if (st->mesh) {
                (void)nb_mesh_add_peer(st->mesh, ip, discovery_port);
            }
            return NB_OK;
        }
    }
    zb_mutex_unlock(st->targets_lock);
    return NB_ERR_NOMEM;
}

/* ---- registry backend（start/stop/tick/announce） --------------------- */

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
