/* mesh 服务发现：UDP 广播 HELLO/BYE + 对端注册表。
 *
 * 只通过 nb_node_access.h 的门面访问 node 状态，不穿透 struct nb_node。 */
#include "discovery.h"

#include <nb/node.h>

#include "node_access.h"
#include "registry_backend.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

#define NB_DISCOVERY_MAGIC 0x4e424453u /* 'NBDS' */
#define NB_DISCOVERY_VERSION 4u
#define NB_DISCOVERY_FLAG_HELLO 0x01u
#define NB_DISCOVERY_FLAG_BYE 0x02u
#define NB_DISCOVERY_FRAME_MAX_BYTES 1400u

/* 构建 HELLO/BYE 帧。返回长度，0 失败。 */
static size_t nb_disc_build(nb_node_t *node, int bye, uint8_t *buf, size_t cap)
{
    size_t n = 0;
    size_t svc_count = nb_node_service_count(node);
    const char *name = nb_node_name(node);
    size_t name_len = name ? strlen(name) : 0;
    size_t binding_count = nb_node_binding_count(node);
    size_t i;

    if (name_len > 255) {
        name_len = 255;
    }
    if (svc_count > 0xffffu) {
        svc_count = 0xffffu;
    }
    if (binding_count > 255u) {
        binding_count = 255u;
    }
    if (cap < 12 + name_len + 1 + 2) {
        return 0;
    }
    nb_put_u32(buf + n, NB_DISCOVERY_MAGIC);
    n += 4;
    buf[n++] = NB_DISCOVERY_VERSION;
    buf[n++] = (uint8_t)(bye ? NB_DISCOVERY_FLAG_BYE : NB_DISCOVERY_FLAG_HELLO);
    nb_put_u32(buf + n, nb_node_id(node));
    n += 4;
    buf[n++] = (uint8_t)name_len;
    if (name_len) {
        memcpy(buf + n, name, name_len);
        n += name_len;
    }
    {
        char uris[NB_MAX_BINDINGS][NB_ENDPOINT_URI_MAX];
        uint8_t valid_count = 0;
        uint8_t valid[NB_MAX_BINDINGS];

        if (binding_count > NB_MAX_BINDINGS) {
            binding_count = NB_MAX_BINDINGS;
        }
        memset(valid, 0, sizeof(valid));
        for (i = 0; i < binding_count; i++) {
            if (nb_node_binding_uri(node, i, uris[i], sizeof(uris[i])) == NB_OK &&
                uris[i][0]) {
                valid[i] = 1;
                valid_count++;
            }
        }
        buf[n++] = valid_count;
        for (i = 0; i < binding_count; i++) {
            size_t len;
            if (!valid[i]) {
                continue;
            }
            len = strlen(uris[i]);
            if (len == 0 || len > 0xffffu || n + 2 + len > cap) {
                return 0;
            }
            nb_put_u16(buf + n, (uint16_t)len);
            n += 2;
            memcpy(buf + n, uris[i], len);
            n += len;
        }
    }
    nb_put_u16(buf + n, (uint16_t)svc_count);
    n += 2;

    for (i = 0; i < svc_count; i++) {
        const char *svc_name = nb_node_service_name(node, i);
        size_t sl = svc_name ? strlen(svc_name) : 0;
        if (sl > 255) {
            sl = 255;
        }
        if (n + 1 + sl > cap) {
            break;
        }
        buf[n++] = (uint8_t)sl;
        memcpy(buf + n, svc_name, sl);
        n += sl;
    }
    return n;
}

static void nb_disc_send_to(
    nb_node_t *node, const char *ip, uint16_t port, const uint8_t *buf, size_t len)
{
    nb_discovery_state_t *st = nb_node_discovery(node);
    if (st && st->sock && ip && port) {
        (void)ntk_socket_sendto(st->sock, buf, len, ip, port);
    }
}

void nb_discovery_send_hello(nb_node_t *node, int bye)
{
    uint8_t buf[NB_DISCOVERY_FRAME_MAX_BYTES];
    nb_disc_target_t targets[NB_DISCOVERY_MAX_TARGETS];
    nb_discovery_state_t *st;
    const char *host;
    size_t n;
    size_t target_count = 0;
    int i;

    if (!node) {
        return;
    }
    st = nb_node_discovery(node);
    if (!st || !st->sock) {
        return;
    }
    n = nb_disc_build(node, bye, buf, sizeof(buf));
    if (n == 0) {
        return;
    }
    host = nb_node_discovery_host(node);
    nb_disc_send_to(
        node, host && host[0] ? host : "255.255.255.255", nb_node_discovery_port(node), buf, n);
    if (st->targets_lock) {
        ntk_mutex_lock(st->targets_lock);
        for (i = 0; i < NB_DISCOVERY_MAX_TARGETS; i++) {
            if (st->targets[i].used) {
                targets[target_count++] = st->targets[i];
            }
        }
        ntk_mutex_unlock(st->targets_lock);
    }
    for (i = 0; i < (int)target_count; i++) {
        nb_disc_send_to(node, targets[i].ip, targets[i].port, buf, n);
    }
}

static void nb_peer_store(nb_node_t *node,
                       const char *ip,
                       uint32_t node_id,
                       const char *remote_name,
                       const uint8_t *binding_blob,
                       size_t binding_len,
                       const uint8_t *svc_blob,
                       int svc_count,
                       size_t svc_len,
                       uint64_t now_ms)
{
    nb_discovery_state_t *st = nb_node_discovery(node);
    nb_peer_t *p = NULL;
    int i;
    size_t off = 0;
    size_t binding_off = 0;

    if (!st) {
        return;
    }
    for (i = 0; i < NB_DISCOVERY_MAX_PEERS; i++) {
        if (st->peers[i].used && st->peers[i].node_id == node_id) {
            p = &st->peers[i];
            break;
        }
    }
    if (!p) {
        for (i = 0; i < NB_DISCOVERY_MAX_PEERS; i++) {
            if (!st->peers[i].used) {
                p = &st->peers[i];
                memset(p, 0, sizeof(*p));
                p->used = 1;
                break;
            }
        }
    }
    if (!p) {
        return;
    }
    p->node_id = node_id;
    p->last_seen_ms = now_ms;
    snprintf(p->ip_s, sizeof(p->ip_s), "%s", ip);

    p->endpoint_count = 0;
    while (binding_off + 2 <= binding_len && p->endpoint_count < NB_MAX_BINDINGS) {
        char uri[NB_ENDPOINT_URI_MAX];
        uint16_t ul = nb_get_u16(binding_blob + binding_off);
        nb_endpoint_t parsed;

        binding_off += 2;
        if (ul == 0 || binding_off + ul > binding_len || ul >= sizeof(uri)) {
            break;
        }
        memcpy(uri, binding_blob + binding_off, ul);
        uri[ul] = '\0';
        binding_off += ul;
        if (nb_endpoint_parse(uri, &parsed) != NB_OK) {
            continue;
        }
        if (nb_endpoint_is_network(&parsed)) {
            /* Binding host is usually 0.0.0.0; the discovery source is authoritative. */
            nb_copy_str(parsed.host, sizeof(parsed.host), ip, "0.0.0.0");
            (void)nb_endpoint_refresh(&parsed);
        }
        p->endpoints[p->endpoint_count++] = parsed;
    }

    p->service_count = 0;
    while (off < svc_len && p->service_count < NB_DISCOVERY_MAX_SERVICES_PER_PEER) {
        uint8_t sl = svc_blob[off];
        off++;
        if (off + sl > svc_len) {
            break;
        }
        if (sl > 0 && sl < NB_SERVICE_NAME_MAX) {
            memcpy(p->services[p->service_count], svc_blob + off, sl);
            p->services[p->service_count][sl] = '\0';
            p->service_count++;
        }
        off += sl;
    }
    {
        char instance_id[NB_NAME_MAX];
        int n = snprintf(instance_id,
                         sizeof(instance_id),
                         "%s-%08x",
                         remote_name && remote_name[0] ? remote_name : "node",
                         (unsigned)node_id);
        if (n > 0 && (size_t)n < sizeof(instance_id) && p->endpoint_count > 0) {
            const char *services[NB_DISCOVERY_MAX_SERVICES_PER_PEER];
            for (i = 0; i < p->service_count; i++) {
                services[i] = p->services[i];
            }
            (void)nb_registry_replace_node(nb_node_registry(node),
                                            node_id,
                                            instance_id,
                                            services,
                                            (size_t)p->service_count,
                                            p->endpoints,
                                            p->endpoint_count,
                                            now_ms);
        }
    }
    (void)svc_count;
}

static void nb_disc_handle_frame(nb_node_t *node, const char *ip, const uint8_t *buf, size_t len)
{
    nb_discovery_state_t *st = nb_node_discovery(node);
    uint32_t magic;
    uint32_t node_id;
    uint8_t flags;
    size_t off;
    uint8_t name_len;
    char remote_name[NB_NAME_MAX];
    uint16_t svc_count;
    size_t svc_len;
    size_t binding_len;
    size_t binding_start;
    uint64_t now = nb_now_ms();
    int i;

    if (!st || len < 13) {
        return;
    }
    magic = nb_get_u32(buf);
    if (magic != NB_DISCOVERY_MAGIC || buf[4] != NB_DISCOVERY_VERSION) {
        return;
    }
    flags = buf[5];
    node_id = nb_get_u32(buf + 6);
    if (node_id == nb_node_id(node)) {
        return; /* 自己 */
    }
    off = 10;
    name_len = buf[off++];
    if (off + name_len + 2 > len) {
        return;
    }
    if (name_len >= sizeof(remote_name)) {
        return;
    }
    memcpy(remote_name, buf + off, name_len);
    remote_name[name_len] = '\0';
    off += name_len;
    if (off >= len) {
        return;
    }
    {
        uint8_t binding_count = buf[off++];
        binding_start = off;
        for (i = 0; i < binding_count; i++) {
            uint16_t ul;
            if (off + 2 > len) {
                return;
            }
            ul = nb_get_u16(buf + off);
            off += 2;
            if (ul == 0 || off + ul > len) {
                return;
            }
            off += ul;
        }
        binding_len = off - binding_start;
    }
    if (off + 2 > len) {
        return;
    }
    svc_count = nb_get_u16(buf + off);
    off += 2;
    svc_len = len - off;

    if (flags & NB_DISCOVERY_FLAG_BYE) {
        for (i = 0; i < NB_DISCOVERY_MAX_PEERS; i++) {
            if (st->peers[i].used && st->peers[i].node_id == node_id) {
                st->peers[i].used = 0;
                break;
            }
        }
        nb_registry_remove_node(nb_node_registry(node), node_id);
        return;
    }
    if (flags & NB_DISCOVERY_FLAG_HELLO) {
        nb_peer_store(node,
                   ip,
                   node_id,
                   remote_name,
                   buf + binding_start,
                   binding_len,
                   buf + off,
                   svc_count,
                   svc_len,
                   now);
    }
}

static void nb_disc_recv_cb(ntk_socket *sock, void *user)
{
    nb_node_t *node = (nb_node_t *)user;
    nb_discovery_state_t *st = nb_node_discovery(node);
    uint8_t buf[NB_DISCOVERY_FRAME_MAX_BYTES];
    (void)sock;
    if (!st || !st->sock) {
        return;
    }
    for (;;) {
        char ip[NB_HOST_MAX];
        uint16_t port = 0;
        ntk_ssize_t n = ntk_socket_recvfrom(st->sock, buf, sizeof(buf), ip, sizeof(ip), &port);
        if (n <= 0) {
            break;
        }
        nb_disc_handle_frame(node, ip, buf, (size_t)n);
    }
}

int nb_discovery_init(nb_node_t *node)
{
    nb_discovery_state_t *st = nb_node_discovery(node);
    ntk_socket_callbacks_t cbs;
    uint16_t actual = 0;
#if defined(_WIN32)
    const int reuse_port = 0;
#else
    const int reuse_port = 1;
#endif

    if (!st) {
        return NB_ERR_INVALID;
    }
    st->sock = ntk_socket_create();
    if (!st->sock) {
        return NB_ERR_NOMEM;
    }
    if (ntk_socket_bind_udp_ex(st->sock, "0.0.0.0", nb_node_discovery_port(node), 1, reuse_port) !=
        NTK_OK) {
        return NB_ERR_IO;
    }
    (void)ntk_socket_set_broadcast(st->sock, 1);
    if (ntk_socket_get_local(st->sock, NULL, 0, &actual) == NTK_OK) {
        nb_node_set_discovery_port(node, actual);
    }
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_readable = nb_disc_recv_cb;
    if (ntk_socket_attach_poller(st->sock, nb_node_poller(node), &cbs, node) != NTK_OK) {
        return NB_ERR_IO;
    }
    return NB_OK;
}

void nb_discovery_shutdown(nb_node_t *node)
{
    nb_discovery_state_t *st = nb_node_discovery(node);
    if (st && st->sock) {
        ntk_socket_detach_poller(st->sock);
        ntk_socket_destroy(st->sock);
        st->sock = NULL;
    }
}

void nb_discovery_on_readable(nb_node_t *node)
{
    nb_discovery_state_t *st = nb_node_discovery(node);
    if (st && st->sock) {
        nb_disc_recv_cb(st->sock, node);
    }
}

void nb_discovery_tick(nb_node_t *node, uint64_t now_ms)
{
    nb_discovery_state_t *st = nb_node_discovery(node);
    uint32_t ttl;
    int i;
    if (!st) {
        return;
    }
    ttl = nb_node_ttl_ms(node);
    for (i = 0; i < NB_DISCOVERY_MAX_PEERS; i++) {
        nb_peer_t *p = &st->peers[i];
        if (p->used && now_ms - p->last_seen_ms > ttl) {
            p->used = 0;
        }
    }
    nb_registry_expire(nb_node_registry(node), now_ms, ttl);
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
