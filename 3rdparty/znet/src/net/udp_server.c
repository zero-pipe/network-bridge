#include "zn/net/udp_server.h"
#include "zn/net/socket.h"
#include "zn/poller/poller.h"
#include "zn/poller/poller_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZN_UDP_RECV 65536

typedef struct zn_udp_slot {
    zn_udp_server *server;
    zn_socket *sock;
    zn_poller *poller;
    /** 收包缓冲（堆上，每 slot 一份）：64KB 量级不能放在 poller 栈上。 */
    char *rx;
} zn_udp_slot;

struct zn_udp_server {
    zn_udp_slot *slots;
    unsigned slot_count;

    zn_udp_server_ops_t ops;
    void *user;

    char host[64];
    uint16_t port;
    int reuse_addr;
    int reuse_port;
    /** 无 SO_REUSEPORT 时多 poller 共享同一 fd（非内核级抢占） */
    int shared_fd_mode;
    int started;
};

static int sockplat_has_reuseport(void)
{
#if defined(SO_REUSEPORT)
    return 1;
#else
    return 0;
#endif
}

static void udp_on_readable(int fd, unsigned events, void *user)
{
    zn_udp_slot *slot = (zn_udp_slot *)user;
    zn_udp_server *srv;
    (void)events;

    if (!slot || !slot->sock || !slot->server)
        return;
    if (zn_socket_fd(slot->sock) != fd)
        return;

    srv = slot->server;
    if (!srv->ops.on_packet)
        return;

    zn_socket *sock = srv->shared_fd_mode ? srv->slots[0].sock : slot->sock;

    char peer_ip[64];
    uint16_t peer_port = 0;
    char *buf;

    if (!slot->rx) {
        slot->rx = (char *)malloc(ZN_UDP_RECV);
        if (!slot->rx)
            return;
    }
    buf = slot->rx;

    for (;;) {
        zn_ssize_t n = zn_socket_recvfrom(sock, buf, ZN_UDP_RECV, peer_ip, sizeof(peer_ip), &peer_port);
        if (n > 0) {
            srv->ops.on_packet(srv, peer_ip, peer_port, buf, (size_t)n, srv->user);
            continue;
        }
        if (n == ZN_ERR_AGAIN)
            break;
        break;
    }
}

static zn_poller *resolve_poller(const zn_udp_server_opts_t *opts, unsigned index)
{
    if (opts->poller_pool)
        return zn_poller_pool_at(opts->poller_pool, index);
    if (opts->poller && index == 0)
        return opts->poller;
    return NULL;
}

static unsigned resolve_poller_count(const zn_udp_server_opts_t *opts)
{
    if (opts->poller_pool)
        return zn_poller_pool_size(opts->poller_pool);
    return opts->poller ? 1 : 0;
}

zn_udp_server *zn_udp_server_create(const zn_udp_server_opts_t *opts)
{
    if (!opts || !opts->ops)
        return NULL;

    unsigned n = resolve_poller_count(opts);
    if (n == 0)
        return NULL;

    zn_udp_server *srv = (zn_udp_server *)calloc(1, sizeof(*srv));
    if (!srv)
        return NULL;

    srv->slot_count = n;
    srv->slots = (zn_udp_slot *)calloc(n, sizeof(*srv->slots));
    if (!srv->slots) {
        free(srv);
        return NULL;
    }

    for (unsigned i = 0; i < n; ++i) {
        srv->slots[i].server = srv;
        srv->slots[i].poller = resolve_poller(opts, i);
        srv->slots[i].sock = zn_socket_create();
        if (!srv->slots[i].poller || !srv->slots[i].sock) {
            zn_udp_server_destroy(srv);
            return NULL;
        }
    }

    srv->ops = *opts->ops;
    srv->user = opts->user;
    srv->reuse_addr = opts->reuse ? 1 : 0;
    srv->reuse_port = opts->reuse_port ? 1 : 0;
    if (n > 1) {
        srv->reuse_addr = 1;
        srv->reuse_port = 1;
    }
    if (opts->host)
        snprintf(srv->host, sizeof(srv->host), "%s", opts->host);
    srv->port = opts->port;
    return srv;
}

void zn_udp_server_destroy(zn_udp_server *srv)
{
    if (!srv)
        return;
    zn_udp_server_stop(srv);
    if (srv->slots) {
        for (unsigned i = 0; i < srv->slot_count; ++i) {
            if (srv->slots[i].sock)
                zn_socket_destroy(srv->slots[i].sock);
            free(srv->slots[i].rx);
        }
        free(srv->slots);
    }
    free(srv);
}

static zn_err_t udp_server_register_slot(zn_udp_server *srv, zn_udp_slot *slot)
{
    int fd = zn_socket_fd(slot->sock);
    return zn_poller_add(slot->poller, fd, ZN_POLL_IN | ZN_POLL_ERR | ZN_POLL_HUP, udp_on_readable,
                           slot);
}

zn_err_t zn_udp_server_start(zn_udp_server *srv)
{
    if (!srv || srv->started)
        return srv && srv->started ? ZN_OK : ZN_ERR_INVALID;

    const char *host = srv->host[0] ? srv->host : NULL;
    srv->shared_fd_mode = 0;

    if (srv->slot_count > 1 && srv->reuse_port && !sockplat_has_reuseport())
        srv->shared_fd_mode = 1;

    if (srv->shared_fd_mode) {
        zn_udp_slot *slot0 = &srv->slots[0];
        zn_err_t err =
            zn_socket_bind_udp_ex(slot0->sock, host, srv->port, srv->reuse_addr, 0);
        if (err != ZN_OK)
            return err;
        if (zn_socket_get_local(slot0->sock, NULL, 0, &srv->port) != ZN_OK)
            srv->port = 0;

        {
            int fd = zn_socket_fd(slot0->sock);
            for (unsigned i = 0; i < srv->slot_count; ++i) {
                err = zn_poller_add(srv->slots[i].poller, fd, ZN_POLL_IN | ZN_POLL_ERR | ZN_POLL_HUP,
                                      udp_on_readable, &srv->slots[i]);
                if (err != ZN_OK) {
                    zn_udp_server_stop(srv);
                    return err;
                }
            }
        }
        srv->started = 1;
        return ZN_OK;
    }

    for (unsigned i = 0; i < srv->slot_count; ++i) {
        zn_udp_slot *slot = &srv->slots[i];
        zn_err_t err =
            zn_socket_bind_udp_ex(slot->sock, host, srv->port, srv->reuse_addr, srv->reuse_port);
        if (err != ZN_OK) {
            zn_udp_server_stop(srv);
            return err;
        }

        if (i == 0) {
            if (zn_socket_get_local(slot->sock, NULL, 0, &srv->port) != ZN_OK)
                srv->port = 0;
        }

        err = udp_server_register_slot(srv, slot);
        if (err != ZN_OK) {
            zn_udp_server_stop(srv);
            return err;
        }
    }

    srv->started = 1;
    return ZN_OK;
}

void zn_udp_server_stop(zn_udp_server *srv)
{
    if (!srv || !srv->started)
        return;

    if (srv->shared_fd_mode) {
        int fd = zn_socket_fd(srv->slots[0].sock);
        for (unsigned i = 0; i < srv->slot_count; ++i) {
            if (srv->slots[i].poller)
                zn_poller_del_sync(srv->slots[i].poller, fd);
        }
    } else {
        for (unsigned i = 0; i < srv->slot_count; ++i) {
            zn_udp_slot *slot = &srv->slots[i];
            if (slot->sock && slot->poller)
                zn_poller_del_sync(slot->poller, zn_socket_fd(slot->sock));
        }
    }

    srv->started = 0;
    srv->shared_fd_mode = 0;
}

uint16_t zn_udp_server_port(const zn_udp_server *srv)
{
    return srv ? srv->port : 0;
}

unsigned zn_udp_server_poller_count(const zn_udp_server *srv)
{
    return srv ? srv->slot_count : 0;
}

zn_poller *zn_udp_server_poller(const zn_udp_server *srv, unsigned index)
{
    if (!srv || index >= srv->slot_count)
        return NULL;
    return srv->slots[index].poller;
}

zn_err_t zn_udp_server_sendto(zn_udp_server *srv, const char *ip, uint16_t port, const void *data,
                                  size_t len)
{
    if (!srv || !srv->started || !ip || srv->slot_count == 0)
        return ZN_ERR_STATE;
    /* 任一同端口 socket 均可发送 */
    zn_ssize_t n = zn_socket_sendto(srv->slots[0].sock, data, len, ip, port);
    if (n < 0)
        return (zn_err_t)n;
    return ZN_OK;
}
