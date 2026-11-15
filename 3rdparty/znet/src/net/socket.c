#include "zn/net/socket.h"
#include "zn/poller/poller.h"
#include "zn/util/log.h"
#include "internal/sock_platform.h"
#include <stdlib.h>
#include <string.h>

struct zn_socket {
    int fd;
    zn_sock_type_t type;
    zn_poller *poller;
    zn_socket_callbacks_t cb;
    void *user;
    unsigned poll_events;
    int attached;
    int connect_pending;
};

static int s_sockplat_inited;

static zn_err_t ensure_sockplat(void)
{
    if (s_sockplat_inited)
        return ZN_OK;
    zn_err_t err = zn_sockplat_init();
    if (err == ZN_OK)
        s_sockplat_inited = 1;
    return err;
}

static void close_fd(zn_socket *sock)
{
    if (!sock || sock->fd < 0)
        return;
    zn_sockplat_close(sock->fd);
    sock->fd = -1;
}

static unsigned default_poll_events(const zn_socket *sock)
{
    unsigned ev = ZN_POLL_IN | ZN_POLL_ERR | ZN_POLL_HUP;
    if (sock->connect_pending)
        ev |= ZN_POLL_OUT;
    return ev;
}

static void socket_poller_cb(int fd, unsigned events, void *user)
{
    zn_socket *sock = (zn_socket *)user;
    if (!sock || sock->fd != fd)
        return;

    if (events & (ZN_POLL_ERR | ZN_POLL_HUP)) {
        if (sock->cb.on_error)
            sock->cb.on_error(sock, sock->user);
        return;
    }

    if ((events & ZN_POLL_OUT) && sock->connect_pending) {
        if (zn_socket_check_connect(sock) == ZN_OK) {
            sock->connect_pending = 0;
            sock->type = ZN_SOCK_TCP_CONNECTED;
            if (sock->attached)
                zn_socket_mod_events(sock, default_poll_events(sock));
        } else if (sock->cb.on_error) {
            sock->cb.on_error(sock, sock->user);
            return;
        }
    }

    if (events & ZN_POLL_OUT) {
        if (sock->cb.on_writable)
            sock->cb.on_writable(sock, sock->user);
    }

    if (events & ZN_POLL_IN) {
        if (sock->cb.on_readable)
            sock->cb.on_readable(sock, sock->user);
    }
}

zn_socket *zn_socket_create(void)
{
    zn_socket *sock = (zn_socket *)calloc(1, sizeof(*sock));
    if (sock)
        sock->fd = -1;
    return sock;
}

void zn_socket_destroy(zn_socket *sock)
{
    if (!sock)
        return;
    if (zn_socket_detach_poller(sock) != ZN_OK) {
        /* poller 未能在超时内确认删除：宁可泄漏也不能留下悬空回调。 */
        zn_error("zn_socket_destroy: detach fd=%d timeout, leak socket", sock->fd);
        return;
    }
    close_fd(sock);
    free(sock);
}

zn_err_t zn_socket_adopt(zn_socket *sock, int fd, zn_sock_type_t type)
{
    if (!sock || fd < 0)
        return ZN_ERR_INVALID;
    if (ensure_sockplat() != ZN_OK)
        return ZN_ERR_PLATFORM;

    zn_socket_detach_poller(sock);
    close_fd(sock);

    zn_err_t err = ZN_OK;
    switch (type) {
    case ZN_SOCK_TCP_LISTEN:
        err = zn_sockplat_apply_tcp_server_opts(fd);
        break;
    case ZN_SOCK_TCP_CLIENT:
    case ZN_SOCK_TCP_CONNECTED:
        err = zn_sockplat_apply_tcp_client_opts(fd);
        break;
    case ZN_SOCK_UDP:
        err = zn_sockplat_apply_udp_opts(fd);
        break;
    default:
        return ZN_ERR_INVALID;
    }
    if (err != ZN_OK)
        return err;

    sock->fd = fd;
    sock->type = type;
    sock->connect_pending = 0;
    return ZN_OK;
}

int zn_socket_fd(const zn_socket *sock)
{
    return sock ? sock->fd : -1;
}

zn_sock_type_t zn_socket_type(const zn_socket *sock)
{
    return sock ? sock->type : ZN_SOCK_NONE;
}

zn_err_t zn_socket_listen(zn_socket *sock, const char *host, uint16_t port, int backlog)
{
    if (!sock)
        return ZN_ERR_INVALID;
    if (ensure_sockplat() != ZN_OK)
        return ZN_ERR_PLATFORM;

    zn_socket_detach_poller(sock);
    close_fd(sock);

    int fd = -1;
    zn_err_t err = zn_sockplat_listen(&fd, host, port, backlog);
    if (err != ZN_OK)
        return err;

    sock->fd = fd;
    sock->type = ZN_SOCK_TCP_LISTEN;
    sock->connect_pending = 0;
    return ZN_OK;
}

zn_err_t zn_socket_bind_udp_ex(zn_socket *sock, const char *host, uint16_t port, int reuse_addr,
                                   int reuse_port)
{
    if (!sock)
        return ZN_ERR_INVALID;
    if (ensure_sockplat() != ZN_OK)
        return ZN_ERR_PLATFORM;

    zn_socket_detach_poller(sock);
    close_fd(sock);

    int fd = -1;
    zn_err_t err = zn_sockplat_bind_udp(&fd, host, port, reuse_addr, reuse_port);
    if (err != ZN_OK)
        return err;

    sock->fd = fd;
    sock->type = ZN_SOCK_UDP;
    sock->connect_pending = 0;
    return ZN_OK;
}

zn_err_t zn_socket_bind_udp(zn_socket *sock, const char *host, uint16_t port, int reuse)
{
    return zn_socket_bind_udp_ex(sock, host, port, reuse, 0);
}

zn_err_t zn_socket_connect(zn_socket *sock, const char *host, uint16_t port, int *in_progress)
{
    if (!sock || !host)
        return ZN_ERR_INVALID;
    if (ensure_sockplat() != ZN_OK)
        return ZN_ERR_PLATFORM;

    zn_socket_detach_poller(sock);
    close_fd(sock);

    int fd = -1;
    int pending = 0;
    zn_err_t err = zn_sockplat_connect(&fd, host, port, &pending);
    if (err != ZN_OK)
        return err;

    sock->fd = fd;
    sock->type = pending ? ZN_SOCK_TCP_CLIENT : ZN_SOCK_TCP_CONNECTED;
    sock->connect_pending = pending ? 1 : 0;
    if (in_progress)
        *in_progress = pending;
    return ZN_OK;
}

zn_err_t zn_socket_check_connect(zn_socket *sock)
{
    if (!sock || sock->fd < 0)
        return ZN_ERR_INVALID;
    if (!sock->connect_pending)
        return ZN_OK;
    zn_err_t err = zn_sockplat_check_connect(sock->fd);
    if (err == ZN_OK) {
        sock->connect_pending = 0;
        sock->type = ZN_SOCK_TCP_CONNECTED;
    }
    return err;
}

zn_err_t zn_socket_accept(zn_socket *listen_sock, zn_socket **out_client)
{
    if (!listen_sock || listen_sock->type != ZN_SOCK_TCP_LISTEN || !out_client)
        return ZN_ERR_INVALID;

    int cfd = -1;
    zn_err_t err = zn_sockplat_accept(listen_sock->fd, &cfd);
    if (err == ZN_ERR_AGAIN)
        return ZN_ERR_AGAIN;
    if (err != ZN_OK)
        return err;

    zn_socket *client = zn_socket_create();
    if (!client) {
        zn_sockplat_close(cfd);
        return ZN_ERR_NOMEM;
    }
    client->fd = cfd;
    client->type = ZN_SOCK_TCP_CONNECTED;
    client->connect_pending = 0;
    *out_client = client;
    return ZN_OK;
}

zn_ssize_t zn_socket_send(zn_socket *sock, const void *data, size_t len)
{
    if (!sock || sock->fd < 0)
        return (zn_ssize_t)ZN_ERR_INVALID;
    return zn_sockplat_send(sock->fd, data, len);
}

zn_ssize_t zn_socket_sendv(zn_socket *sock, const zn_socket_iov *iov, unsigned count)
{
    zn_sock_iov plat[ZN_SOCK_IOV_MAX];
    unsigned i;

    if (!sock || sock->fd < 0)
        return (zn_ssize_t)ZN_ERR_INVALID;
    if (!iov || count == 0)
        return (zn_ssize_t)ZN_ERR_INVALID;
    if (count > ZN_SOCKET_IOV_MAX)
        count = ZN_SOCKET_IOV_MAX;

    for (i = 0; i < count; ++i) {
        plat[i].base = iov[i].base;
        plat[i].len = iov[i].len;
    }
    return zn_sockplat_sendv(sock->fd, plat, count);
}

zn_err_t zn_socket_set_bbr(zn_socket *sock, int on)
{
    if (!sock || sock->fd < 0)
        return ZN_ERR_INVALID;
    if (sock->type != ZN_SOCK_TCP_CONNECTED && sock->type != ZN_SOCK_TCP_CLIENT)
        return ZN_ERR_INVALID;
    return zn_sockplat_set_tcp_bbr(sock->fd, on);
}

zn_ssize_t zn_socket_recv(zn_socket *sock, void *buf, size_t len)
{
    if (!sock || sock->fd < 0)
        return (zn_ssize_t)ZN_ERR_INVALID;
    return zn_sockplat_recv(sock->fd, buf, len);
}

zn_ssize_t zn_socket_sendto(zn_socket *sock, const void *data, size_t len, const char *ip, uint16_t port)
{
    if (!sock || sock->type != ZN_SOCK_UDP)
        return (zn_ssize_t)ZN_ERR_INVALID;
    return zn_sockplat_sendto(sock->fd, data, len, ip, port);
}

zn_ssize_t zn_socket_recvfrom(zn_socket *sock, void *buf, size_t len,
                                  char *ip, size_t ip_len, uint16_t *port)
{
    if (!sock || sock->type != ZN_SOCK_UDP)
        return (zn_ssize_t)ZN_ERR_INVALID;
    return zn_sockplat_recvfrom(sock->fd, buf, len, ip, ip_len, port);
}

zn_err_t zn_socket_get_local(const zn_socket *sock, char *ip, size_t ip_len, uint16_t *port)
{
    if (!sock || sock->fd < 0)
        return ZN_ERR_INVALID;
    return zn_sockplat_get_local(sock->fd, ip, ip_len, port);
}

zn_err_t zn_socket_get_peer(const zn_socket *sock, char *ip, size_t ip_len, uint16_t *port)
{
    if (!sock || sock->fd < 0)
        return ZN_ERR_INVALID;
    return zn_sockplat_get_peer(sock->fd, ip, ip_len, port);
}

zn_err_t zn_socket_attach_poller(zn_socket *sock, zn_poller *poller,
                                     const zn_socket_callbacks_t *cb, void *user)
{
    if (!sock || sock->fd < 0 || !poller)
        return ZN_ERR_INVALID;

    zn_socket_detach_poller(sock);

    if (cb)
        sock->cb = *cb;
    else
        memset(&sock->cb, 0, sizeof(sock->cb));
    sock->user = user;
    sock->poller = poller;
    sock->poll_events = default_poll_events(sock);

    zn_err_t err = zn_poller_add(poller, sock->fd, sock->poll_events, socket_poller_cb, sock);
    if (err != ZN_OK)
        return err;
    sock->attached = 1;
    return ZN_OK;
}

zn_err_t zn_socket_detach_poller(zn_socket *sock)
{
    if (!sock || !sock->attached || !sock->poller || sock->fd < 0)
        return ZN_OK;

    zn_err_t err = zn_poller_del_sync(sock->poller, sock->fd);
    if (err != ZN_OK)
        return err;
    sock->attached = 0;
    sock->poller = NULL;
    return ZN_OK;
}

zn_err_t zn_socket_mod_events(zn_socket *sock, unsigned events)
{
    if (!sock || !sock->attached || !sock->poller)
        return ZN_ERR_STATE;
    sock->poll_events = events;
    return zn_poller_mod(sock->poller, sock->fd, events);
}

static zn_err_t udp_fd_check(const zn_socket *sock)
{
    if (!sock || sock->type != ZN_SOCK_UDP || sock->fd < 0)
        return ZN_ERR_INVALID;
    return ZN_OK;
}

zn_err_t zn_socket_set_broadcast(zn_socket *sock, int on)
{
    zn_err_t err = udp_fd_check(sock);
    if (err != ZN_OK)
        return err;
    return zn_sockplat_set_broadcast(sock->fd, on);
}

zn_err_t zn_socket_set_multicast_ttl(zn_socket *sock, uint8_t ttl)
{
    zn_err_t err = udp_fd_check(sock);
    if (err != ZN_OK)
        return err;
    return zn_sockplat_set_multicast_ttl(sock->fd, ttl);
}

zn_err_t zn_socket_set_multicast_if(zn_socket *sock, const char *local_ip)
{
    zn_err_t err = udp_fd_check(sock);
    if (err != ZN_OK)
        return err;
    return zn_sockplat_set_multicast_if(sock->fd, local_ip);
}

zn_err_t zn_socket_set_multicast_loop(zn_socket *sock, int on)
{
    zn_err_t err = udp_fd_check(sock);
    if (err != ZN_OK)
        return err;
    return zn_sockplat_set_multicast_loop(sock->fd, on);
}

zn_err_t zn_socket_join_multicast(zn_socket *sock, const char *group_addr, const char *local_if)
{
    zn_err_t err = udp_fd_check(sock);
    if (err != ZN_OK || !group_addr)
        return err != ZN_OK ? err : ZN_ERR_INVALID;
    return zn_sockplat_join_multicast(sock->fd, group_addr, local_if);
}

zn_err_t zn_socket_leave_multicast(zn_socket *sock, const char *group_addr, const char *local_if)
{
    zn_err_t err = udp_fd_check(sock);
    if (err != ZN_OK || !group_addr)
        return err != ZN_OK ? err : ZN_ERR_INVALID;
    return zn_sockplat_leave_multicast(sock->fd, group_addr, local_if);
}

zn_err_t zn_socket_join_multicast_filter(zn_socket *sock, const char *group_addr,
                                             const char *src_ip, const char *local_if)
{
    zn_err_t err = udp_fd_check(sock);
    if (err != ZN_OK || !group_addr || !src_ip)
        return err != ZN_OK ? err : ZN_ERR_INVALID;
    return zn_sockplat_join_multicast_filter(sock->fd, group_addr, src_ip, local_if);
}

zn_err_t zn_socket_leave_multicast_filter(zn_socket *sock, const char *group_addr,
                                              const char *src_ip, const char *local_if)
{
    zn_err_t err = udp_fd_check(sock);
    if (err != ZN_OK || !group_addr || !src_ip)
        return err != ZN_OK ? err : ZN_ERR_INVALID;
    return zn_sockplat_leave_multicast_filter(sock->fd, group_addr, src_ip, local_if);
}
