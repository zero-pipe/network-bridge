#include "zb/os/socket.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static zb_err_t map_errno(void)
{
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return ZB_ERR_AGAIN;
    if (errno == ETIMEDOUT)
        return ZB_ERR_TIMEOUT;
    return ZB_ERR_IO;
}

static int is_any_host(const char *host)
{
    return !host || !host[0] || strcmp(host, "0.0.0.0") == 0 || strcmp(host, "::") == 0;
}

static zb_err_t set_nonblock(int fd, int on)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return ZB_ERR_PLATFORM;
    if (on)
        flags |= O_NONBLOCK;
    else
        flags &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags) == 0 ? ZB_OK : ZB_ERR_PLATFORM;
}

static zb_err_t set_cloexec(int fd)
{
#ifdef FD_CLOEXEC
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0)
        return ZB_ERR_PLATFORM;
    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0)
        return ZB_ERR_PLATFORM;
#endif
    return ZB_OK;
}

static zb_err_t set_reuseaddr(int fd, int on)
{
    int v = on ? 1 : 0;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v)) == 0 ? ZB_OK : ZB_ERR_PLATFORM;
}

static zb_err_t set_reuseport(int fd, int on)
{
#if defined(SO_REUSEPORT)
    int v = on ? 1 : 0;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &v, sizeof(v)) == 0 ? ZB_OK : ZB_ERR_PLATFORM;
#else
    (void)fd;
    (void)on;
    return on ? ZB_ERR_NOT_IMPL : ZB_OK;
#endif
}

static zb_err_t set_nodelay(int fd)
{
    int v = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)) == 0 ? ZB_OK : ZB_ERR_PLATFORM;
}

zb_err_t zb_sock_init(void)
{
    return ZB_OK;
}

void zb_sock_fini(void) {}

zb_err_t zb_sock_apply_tcp_server_opts(int fd)
{
    if (set_nonblock(fd, 1) != ZB_OK)
        return ZB_ERR_PLATFORM;
    if (set_cloexec(fd) != ZB_OK)
        return ZB_ERR_PLATFORM;
    set_reuseaddr(fd, 1);
    return ZB_OK;
}

zb_err_t zb_sock_apply_tcp_client_opts(int fd)
{
    if (set_nonblock(fd, 1) != ZB_OK)
        return ZB_ERR_PLATFORM;
    if (set_cloexec(fd) != ZB_OK)
        return ZB_ERR_PLATFORM;
    set_nodelay(fd);
    return ZB_OK;
}

zb_err_t zb_sock_apply_udp_opts(int fd)
{
    if (set_nonblock(fd, 1) != ZB_OK)
        return ZB_ERR_PLATFORM;
    if (set_cloexec(fd) != ZB_OK)
        return ZB_ERR_PLATFORM;
    return ZB_OK;
}

static zb_err_t resolve_bind(const char *host, uint16_t port, int type, int passive,
                               struct addrinfo **out)
{
    struct addrinfo hints;
    char port_str[16];
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = type;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    if (is_any_host(host))
        hints.ai_family = AF_INET;
    else
        hints.ai_family = AF_UNSPEC;

    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    int r = getaddrinfo(is_any_host(host) ? NULL : host, port_str, &hints, out);
    if (r != 0)
        return ZB_ERR_IO;
    return ZB_OK;
}

static zb_err_t resolve_connect(const char *host, uint16_t port, struct addrinfo **out)
{
    struct addrinfo hints;
    char port_str[16];
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    int r = getaddrinfo(host, port_str, &hints, out);
    if (r != 0)
        return ZB_ERR_IO;
    return ZB_OK;
}

static zb_err_t bind_and_listen(const char *host, uint16_t port, int backlog, int *out_fd)
{
    struct addrinfo *res = NULL;
    zb_err_t err = resolve_bind(host, port, SOCK_STREAM, 1, &res);
    if (err != ZB_OK)
        return err;

    int fd = -1;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
            continue;
        if (zb_sock_apply_tcp_server_opts(fd) != ZB_OK) {
            close(fd);
            fd = -1;
            continue;
        }
        set_reuseaddr(fd, 1);
        if (bind(fd, p->ai_addr, (socklen_t)p->ai_addrlen) != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        if (listen(fd, backlog > 0 ? backlog : ZB_TCP_DEFAULT_BACKLOG) != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return map_errno();
    *out_fd = fd;
    return ZB_OK;
}

zb_err_t zb_sock_listen(int *out_fd, const char *host, uint16_t port, int backlog)
{
    if (!out_fd)
        return ZB_ERR_INVALID;
    return bind_and_listen(host, port, backlog, out_fd);
}

zb_err_t zb_sock_bind_udp(int *out_fd, const char *host, uint16_t port, int reuse_addr,
                                  int reuse_port)
{
    if (!out_fd)
        return ZB_ERR_INVALID;

    struct addrinfo *res = NULL;
    zb_err_t err = resolve_bind(host, port, SOCK_DGRAM, 1, &res);
    if (err != ZB_OK)
        return err;

    int fd = -1;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
            continue;
        if (zb_sock_apply_udp_opts(fd) != ZB_OK) {
            close(fd);
            fd = -1;
            continue;
        }
        if (reuse_addr)
            set_reuseaddr(fd, 1);
        if (reuse_port) {
            err = set_reuseport(fd, 1);
            if (err != ZB_OK) {
                close(fd);
                fd = -1;
                continue;
            }
        }
        if (bind(fd, p->ai_addr, (socklen_t)p->ai_addrlen) != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return map_errno();
    *out_fd = fd;
    return ZB_OK;
}

zb_err_t zb_sock_connect(int *out_fd, const char *host, uint16_t port, int *in_progress)
{
    if (!out_fd || !host)
        return ZB_ERR_INVALID;
    if (in_progress)
        *in_progress = 0;

    struct addrinfo *res = NULL;
    zb_err_t err = resolve_connect(host, port, &res);
    if (err != ZB_OK)
        return err;

    int fd = -1;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
            continue;
        if (zb_sock_apply_tcp_client_opts(fd) != ZB_OK) {
            close(fd);
            fd = -1;
            continue;
        }
        int r = connect(fd, p->ai_addr, (socklen_t)p->ai_addrlen);
        if (r == 0) {
            break;
        }
        if (errno == EINPROGRESS) {
            if (in_progress)
                *in_progress = 1;
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return map_errno();
    *out_fd = fd;
    return ZB_OK;
}

zb_err_t zb_sock_accept(int listen_fd, int *out_fd)
{
    if (!out_fd)
        return ZB_ERR_INVALID;
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0)
        return map_errno();
    zb_sock_apply_tcp_client_opts(fd);
    *out_fd = fd;
    return ZB_OK;
}

static zb_err_t addr_to_string(const struct sockaddr *sa, socklen_t len,
                                 char *ip, size_t ip_len, uint16_t *port)
{
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        if (ip && ip_len > 0) {
            if (!inet_ntop(AF_INET, &in->sin_addr, ip, ip_len))
                return ZB_ERR_PLATFORM;
        }
        if (port)
            *port = ntohs(in->sin_port);
        return ZB_OK;
    }
#if defined(AF_INET6)
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
        if (ip && ip_len > 0) {
            if (!inet_ntop(AF_INET6, &in6->sin6_addr, ip, ip_len))
                return ZB_ERR_PLATFORM;
        }
        if (port)
            *port = ntohs(in6->sin6_port);
        return ZB_OK;
    }
#endif
    (void)len;
    return ZB_ERR_PLATFORM;
}

zb_err_t zb_sock_get_local(int fd, char *ip, size_t ip_len, uint16_t *port)
{
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0)
        return map_errno();
    return addr_to_string((struct sockaddr *)&addr, len, ip, ip_len, port);
}

zb_err_t zb_sock_get_peer(int fd, char *ip, size_t ip_len, uint16_t *port)
{
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getpeername(fd, (struct sockaddr *)&addr, &len) != 0)
        return map_errno();
    return addr_to_string((struct sockaddr *)&addr, len, ip, ip_len, port);
}

zb_err_t zb_sock_check_connect(int fd)
{
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getpeername(fd, (struct sockaddr *)&addr, &len) == 0)
        return ZB_OK;

    int err = 0;
    socklen_t elen = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0)
        return map_errno();
    if (err == 0)
        return ZB_ERR_AGAIN;
    errno = err;
    return map_errno();
}

zb_ssize_t zb_sock_send(int fd, const void *buf, size_t len)
{
    ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
    if (n < 0)
        return (zb_ssize_t)map_errno();
    return (zb_ssize_t)n;
}

zb_ssize_t zb_sock_sendv(int fd, const zb_sock_iov *iov, unsigned count)
{
    struct iovec sys_iov[ZB_SOCK_IOV_MAX];
    unsigned i;
    size_t total = 0;

    if (!iov || count == 0)
        return (zb_ssize_t)ZB_ERR_INVALID;
    if (count > ZB_SOCK_IOV_MAX)
        count = ZB_SOCK_IOV_MAX;

    for (i = 0; i < count; ++i) {
        sys_iov[i].iov_base = (void *)iov[i].base;
        sys_iov[i].iov_len = iov[i].len;
        total += iov[i].len;
    }

    ssize_t n = writev(fd, sys_iov, (int)count);
    if (n < 0)
        return (zb_ssize_t)map_errno();
    return (zb_ssize_t)n;
}

zb_err_t zb_sock_set_tcp_bbr(int fd, int on)
{
#if defined(TCP_CONGESTION)
    const char *algo = on ? "bbr" : "cubic";
    if (setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, algo, (socklen_t)(strlen(algo) + 1)) != 0)
        return map_errno();
    return ZB_OK;
#else
    (void)fd;
    (void)on;
    return ZB_ERR_NOT_IMPL;
#endif
}

zb_ssize_t zb_sock_recv(int fd, void *buf, size_t len)
{
    ssize_t n = recv(fd, buf, len, 0);
    if (n < 0)
        return (zb_ssize_t)map_errno();
    return (zb_ssize_t)n;
}

static zb_err_t resolve_endpoint(const char *ip, uint16_t port, struct addrinfo **out)
{
    struct addrinfo hints;
    char port_str[16];
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = AF_UNSPEC;
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    if (getaddrinfo(ip, port_str, &hints, out) != 0)
        return ZB_ERR_IO;
    return ZB_OK;
}

zb_ssize_t zb_sock_sendto(int fd, const void *buf, size_t len, const char *ip, uint16_t port)
{
    struct addrinfo *res = NULL;
    if (resolve_endpoint(ip, port, &res) != ZB_OK)
        return (ssize_t)ZB_ERR_IO;

    ssize_t n = -1;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        n = sendto(fd, buf, len, MSG_NOSIGNAL, p->ai_addr, (socklen_t)p->ai_addrlen);
        if (n >= 0)
            break;
    }
    freeaddrinfo(res);
    if (n < 0)
        return (zb_ssize_t)map_errno();
    return (zb_ssize_t)n;
}

zb_ssize_t zb_sock_recvfrom(int fd, void *buf, size_t len, char *ip, size_t ip_len, uint16_t *port)
{
    struct sockaddr_storage addr;
    socklen_t slen = sizeof(addr);
    ssize_t n = recvfrom(fd, buf, len, 0, (struct sockaddr *)&addr, &slen);
    if (n < 0)
        return (zb_ssize_t)map_errno();
    if (ip && ip_len > 0)
        addr_to_string((struct sockaddr *)&addr, slen, ip, ip_len, port);
    return (zb_ssize_t)n;
}

zb_err_t zb_sock_close(int fd)
{
    if (fd < 0)
        return ZB_OK;
    return close(fd) == 0 ? ZB_OK : map_errno();
}

static in_addr_t multicast_if_addr(const char *local_if)
{
    if (!local_if || !local_if[0] || strcmp(local_if, "0.0.0.0") == 0)
        return htonl(INADDR_ANY);
    return inet_addr(local_if);
}

#if defined(IP_MULTICAST_ALL)
static void clear_multicast_all(int fd)
{
    int v = 0;
    (void)setsockopt(fd, IPPROTO_IP, IP_MULTICAST_ALL, &v, sizeof(v));
}
#else
static void clear_multicast_all(int fd)
{
    (void)fd;
}
#endif

zb_err_t zb_sock_set_broadcast(int fd, int on)
{
    int v = on ? 1 : 0;
    return setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &v, sizeof(v)) == 0 ? ZB_OK : ZB_ERR_PLATFORM;
}

zb_err_t zb_sock_set_multicast_ttl(int fd, uint8_t ttl)
{
#if defined(IP_MULTICAST_TTL)
    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) != 0)
        return ZB_ERR_PLATFORM;
    clear_multicast_all(fd);
    return ZB_OK;
#else
    (void)fd;
    (void)ttl;
    return ZB_ERR_NOT_IMPL;
#endif
}

zb_err_t zb_sock_set_multicast_if(int fd, const char *local_ip)
{
#if defined(IP_MULTICAST_IF)
    struct in_addr addr;
    addr.s_addr = multicast_if_addr(local_ip);
    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &addr, sizeof(addr)) != 0)
        return ZB_ERR_PLATFORM;
    clear_multicast_all(fd);
    return ZB_OK;
#else
    (void)fd;
    (void)local_ip;
    return ZB_ERR_NOT_IMPL;
#endif
}

zb_err_t zb_sock_set_multicast_loop(int fd, int on)
{
#if defined(IP_MULTICAST_LOOP)
    unsigned char v = on ? 1 : 0;
    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &v, sizeof(v)) != 0)
        return ZB_ERR_PLATFORM;
    clear_multicast_all(fd);
    return ZB_OK;
#else
    (void)fd;
    (void)on;
    return ZB_ERR_NOT_IMPL;
#endif
}

zb_err_t zb_sock_join_multicast(int fd, const char *group_addr, const char *local_if)
{
#if defined(IP_ADD_MEMBERSHIP)
    struct ip_mreq imr;
    memset(&imr, 0, sizeof(imr));
    imr.imr_multiaddr.s_addr = inet_addr(group_addr);
    imr.imr_interface.s_addr = multicast_if_addr(local_if);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &imr, sizeof(imr)) != 0)
        return map_errno();
    clear_multicast_all(fd);
    return ZB_OK;
#else
    (void)fd;
    (void)group_addr;
    (void)local_if;
    return ZB_ERR_NOT_IMPL;
#endif
}

zb_err_t zb_sock_leave_multicast(int fd, const char *group_addr, const char *local_if)
{
#if defined(IP_DROP_MEMBERSHIP)
    struct ip_mreq imr;
    memset(&imr, 0, sizeof(imr));
    imr.imr_multiaddr.s_addr = inet_addr(group_addr);
    imr.imr_interface.s_addr = multicast_if_addr(local_if);
    if (setsockopt(fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &imr, sizeof(imr)) != 0)
        return map_errno();
    return ZB_OK;
#else
    (void)fd;
    (void)group_addr;
    (void)local_if;
    return ZB_ERR_NOT_IMPL;
#endif
}

zb_err_t zb_sock_join_multicast_filter(int fd, const char *group_addr, const char *src_ip,
                                               const char *local_if)
{
#if defined(IP_ADD_SOURCE_MEMBERSHIP)
    struct ip_mreq_source imr;
    memset(&imr, 0, sizeof(imr));
    imr.imr_multiaddr.s_addr = inet_addr(group_addr);
    imr.imr_sourceaddr.s_addr = inet_addr(src_ip);
    imr.imr_interface.s_addr = multicast_if_addr(local_if);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP, &imr, sizeof(imr)) != 0)
        return map_errno();
    clear_multicast_all(fd);
    return ZB_OK;
#else
    (void)fd;
    (void)group_addr;
    (void)src_ip;
    (void)local_if;
    return ZB_ERR_NOT_IMPL;
#endif
}

zb_err_t zb_sock_leave_multicast_filter(int fd, const char *group_addr, const char *src_ip,
                                                const char *local_if)
{
#if defined(IP_DROP_SOURCE_MEMBERSHIP)
    struct ip_mreq_source imr;
    memset(&imr, 0, sizeof(imr));
    imr.imr_multiaddr.s_addr = inet_addr(group_addr);
    imr.imr_sourceaddr.s_addr = inet_addr(src_ip);
    imr.imr_interface.s_addr = multicast_if_addr(local_if);
    if (setsockopt(fd, IPPROTO_IP, IP_DROP_SOURCE_MEMBERSHIP, &imr, sizeof(imr)) != 0)
        return map_errno();
    return ZB_OK;
#else
    (void)fd;
    (void)group_addr;
    (void)src_ip;
    (void)local_if;
    return ZB_ERR_NOT_IMPL;
#endif
}
