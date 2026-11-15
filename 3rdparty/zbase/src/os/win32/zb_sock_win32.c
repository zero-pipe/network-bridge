#include "zb/os/socket.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>

static int s_wsa_refs;

static zb_err_t map_wsa(int wsa_err)
{
    if (wsa_err == WSAEWOULDBLOCK)
        return ZB_ERR_AGAIN;
    if (wsa_err == WSAETIMEDOUT)
        return ZB_ERR_TIMEOUT;
    return ZB_ERR_IO;
}

static zb_err_t map_errno(void)
{
    return map_wsa(WSAGetLastError());
}

static SOCKET to_sock(int fd)
{
    return (SOCKET)fd;
}

static int is_any_host(const char *host)
{
    return !host || !host[0] || strcmp(host, "0.0.0.0") == 0 || strcmp(host, "::") == 0;
}

static zb_err_t set_nonblock(int fd, int on)
{
    u_long mode = on ? 1 : 0;
    return ioctlsocket(to_sock(fd), FIONBIO, &mode) == 0 ? ZB_OK : ZB_ERR_PLATFORM;
}

static zb_err_t set_cloexec(int fd)
{
    if (SetHandleInformation((HANDLE)to_sock(fd), HANDLE_FLAG_INHERIT, 0))
        return ZB_OK;
    return ZB_ERR_PLATFORM;
}

static zb_err_t set_reuseaddr(int fd, int on)
{
    BOOL v = on ? TRUE : FALSE;
    return setsockopt(to_sock(fd), SOL_SOCKET, SO_REUSEADDR, (const char *)&v, sizeof(v)) == 0 ? ZB_OK
                                                                                            : ZB_ERR_PLATFORM;
}

static zb_err_t set_reuseport(int fd, int on)
{
#if defined(SO_REUSEPORT)
    BOOL v = on ? TRUE : FALSE;
    return setsockopt(to_sock(fd), SOL_SOCKET, SO_REUSEPORT, (const char *)&v, sizeof(v)) == 0 ? ZB_OK
                                                                                             : ZB_ERR_PLATFORM;
#else
    (void)fd;
    (void)on;
    return on ? ZB_ERR_NOT_IMPL : ZB_OK;
#endif
}

static zb_err_t set_nodelay(int fd)
{
    BOOL v = TRUE;
    return setsockopt(to_sock(fd), IPPROTO_TCP, TCP_NODELAY, (const char *)&v, sizeof(v)) == 0 ? ZB_OK
                                                                                               : ZB_ERR_PLATFORM;
}

zb_err_t zb_sock_init(void)
{
    if (s_wsa_refs > 0)
        return ZB_OK;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return ZB_ERR_PLATFORM;
    s_wsa_refs = 1;
    return ZB_OK;
}

void zb_sock_fini(void)
{
    if (s_wsa_refs == 0)
        return;
    WSACleanup();
    s_wsa_refs = 0;
}

zb_err_t zb_sock_apply_tcp_server_opts(int fd)
{
    if (set_nonblock(fd, 1) != ZB_OK)
        return ZB_ERR_PLATFORM;
    set_cloexec(fd);
    set_reuseaddr(fd, 1);
    return ZB_OK;
}

zb_err_t zb_sock_apply_tcp_client_opts(int fd)
{
    if (set_nonblock(fd, 1) != ZB_OK)
        return ZB_ERR_PLATFORM;
    set_cloexec(fd);
    set_nodelay(fd);
    return ZB_OK;
}

zb_err_t zb_sock_apply_udp_opts(int fd)
{
    if (set_nonblock(fd, 1) != ZB_OK)
        return ZB_ERR_PLATFORM;
    set_cloexec(fd);
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
    hints.ai_family = is_any_host(host) ? AF_INET : AF_UNSPEC;

    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    if (getaddrinfo(is_any_host(host) ? NULL : host, port_str, &hints, out) != 0)
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
    if (getaddrinfo(host, port_str, &hints, out) != 0)
        return ZB_ERR_IO;
    return ZB_OK;
}

static zb_err_t bind_and_listen(const char *host, uint16_t port, int backlog, int *out_fd)
{
    struct addrinfo *res = NULL;
    zb_err_t err = resolve_bind(host, port, SOCK_STREAM, 1, &res);
    if (err != ZB_OK)
        return err;

    SOCKET fd = INVALID_SOCKET;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == INVALID_SOCKET)
            continue;
        int ifd = (int)fd;
        if (zb_sock_apply_tcp_server_opts(ifd) != ZB_OK) {
            closesocket(fd);
            fd = INVALID_SOCKET;
            continue;
        }
        set_reuseaddr(ifd, 1);
        if (bind(fd, p->ai_addr, (int)p->ai_addrlen) != 0) {
            closesocket(fd);
            fd = INVALID_SOCKET;
            continue;
        }
        if (listen(fd, backlog > 0 ? backlog : SOMAXCONN) != 0) {
            closesocket(fd);
            fd = INVALID_SOCKET;
            continue;
        }
        break;
    }
    freeaddrinfo(res);
    if (fd == INVALID_SOCKET)
        return map_errno();
    *out_fd = (int)fd;
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

    SOCKET fd = INVALID_SOCKET;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == INVALID_SOCKET)
            continue;
        int ifd = (int)fd;
        if (zb_sock_apply_udp_opts(ifd) != ZB_OK) {
            closesocket(fd);
            fd = INVALID_SOCKET;
            continue;
        }
        if (reuse_addr)
            set_reuseaddr(ifd, 1);
        if (reuse_port) {
            err = set_reuseport(ifd, 1);
            if (err != ZB_OK) {
                closesocket(fd);
                fd = INVALID_SOCKET;
                continue;
            }
        }
        if (bind(fd, p->ai_addr, (int)p->ai_addrlen) != 0) {
            closesocket(fd);
            fd = INVALID_SOCKET;
            continue;
        }
        break;
    }
    freeaddrinfo(res);
    if (fd == INVALID_SOCKET)
        return map_errno();
    *out_fd = (int)fd;
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

    SOCKET fd = INVALID_SOCKET;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == INVALID_SOCKET)
            continue;
        int ifd = (int)fd;
        if (zb_sock_apply_tcp_client_opts(ifd) != ZB_OK) {
            closesocket(fd);
            fd = INVALID_SOCKET;
            continue;
        }
        if (connect(fd, p->ai_addr, (int)p->ai_addrlen) == 0)
            break;
        int wsa = WSAGetLastError();
        if (wsa == WSAEWOULDBLOCK) {
            if (in_progress)
                *in_progress = 1;
            break;
        }
        closesocket(fd);
        fd = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (fd == INVALID_SOCKET)
        return map_errno();
    *out_fd = (int)fd;
    return ZB_OK;
}

zb_err_t zb_sock_accept(int listen_fd, int *out_fd)
{
    if (!out_fd)
        return ZB_ERR_INVALID;
    SOCKET fd = accept(to_sock(listen_fd), NULL, NULL);
    if (fd == INVALID_SOCKET)
        return map_errno();
    zb_sock_apply_tcp_client_opts((int)fd);
    *out_fd = (int)fd;
    return ZB_OK;
}

static zb_err_t addr_to_string(const struct sockaddr *sa, int len, char *ip, size_t ip_len,
                                 uint16_t *port)
{
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        if (ip && ip_len > 0) {
            if (!InetNtopA(AF_INET, &in->sin_addr, ip, (DWORD)ip_len))
                return ZB_ERR_PLATFORM;
        }
        if (port)
            *port = ntohs(in->sin_port);
        return ZB_OK;
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
        if (ip && ip_len > 0) {
            if (!InetNtopA(AF_INET6, &in6->sin6_addr, ip, (DWORD)ip_len))
                return ZB_ERR_PLATFORM;
        }
        if (port)
            *port = ntohs(in6->sin6_port);
        return ZB_OK;
    }
    (void)len;
    return ZB_ERR_PLATFORM;
}

zb_err_t zb_sock_get_local(int fd, char *ip, size_t ip_len, uint16_t *port)
{
    struct sockaddr_storage addr;
    int len = sizeof(addr);
    if (getsockname(to_sock(fd), (struct sockaddr *)&addr, &len) != 0)
        return map_errno();
    return addr_to_string((struct sockaddr *)&addr, len, ip, ip_len, port);
}

zb_err_t zb_sock_get_peer(int fd, char *ip, size_t ip_len, uint16_t *port)
{
    struct sockaddr_storage addr;
    int len = sizeof(addr);
    if (getpeername(to_sock(fd), (struct sockaddr *)&addr, &len) != 0)
        return map_errno();
    return addr_to_string((struct sockaddr *)&addr, len, ip, ip_len, port);
}

zb_err_t zb_sock_check_connect(int fd)
{
    struct sockaddr_storage addr;
    int len = sizeof(addr);
    if (getpeername(to_sock(fd), (struct sockaddr *)&addr, &len) == 0)
        return ZB_OK;

    int err = 0;
    int elen = sizeof(err);
    if (getsockopt(to_sock(fd), SOL_SOCKET, SO_ERROR, (char *)&err, &elen) != 0)
        return map_errno();
    if (err == 0)
        return ZB_ERR_AGAIN;
    return map_wsa(err);
}

zb_ssize_t zb_sock_send(int fd, const void *buf, size_t len)
{
    int n = send(to_sock(fd), (const char *)buf, (int)(len > 0x7fffffff ? 0x7fffffff : len), 0);
    if (n == SOCKET_ERROR)
        return (zb_ssize_t)map_errno();
    return (zb_ssize_t)n;
}

zb_ssize_t zb_sock_sendv(int fd, const zb_sock_iov *iov, unsigned count)
{
    size_t sent = 0;
    unsigned i;

    if (!iov || count == 0)
        return (zb_ssize_t)ZB_ERR_INVALID;

    for (i = 0; i < count; ++i) {
        const uint8_t *p = (const uint8_t *)iov[i].base;
        size_t left = iov[i].len;
        while (left > 0) {
            int chunk = (int)(left > 0x7fffffff ? 0x7fffffff : left);
            int n = send(to_sock(fd), (const char *)p, chunk, 0);
            if (n == SOCKET_ERROR)
                return sent > 0 ? (zb_ssize_t)sent : (zb_ssize_t)map_errno();
            p += n;
            left -= (size_t)n;
            sent += (size_t)n;
        }
    }
    return (zb_ssize_t)sent;
}

zb_err_t zb_sock_set_tcp_bbr(int fd, int on)
{
    (void)fd;
    (void)on;
    return ZB_ERR_NOT_IMPL;
}

zb_ssize_t zb_sock_recv(int fd, void *buf, size_t len)
{
    int n = recv(to_sock(fd), (char *)buf, (int)(len > 0x7fffffff ? 0x7fffffff : len), 0);
    if (n == SOCKET_ERROR)
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
        return (zb_ssize_t)ZB_ERR_IO;

    int n = SOCKET_ERROR;
    int send_len = (int)(len > 0x7fffffff ? 0x7fffffff : len);
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        n = sendto(to_sock(fd), (const char *)buf, send_len, 0, p->ai_addr, (int)p->ai_addrlen);
        if (n != SOCKET_ERROR)
            break;
    }
    freeaddrinfo(res);
    if (n == SOCKET_ERROR)
        return (zb_ssize_t)map_errno();
    return (zb_ssize_t)n;
}

zb_ssize_t zb_sock_recvfrom(int fd, void *buf, size_t len, char *ip, size_t ip_len,
                                    uint16_t *port)
{
    struct sockaddr_storage addr;
    int slen = sizeof(addr);
    int n = recvfrom(to_sock(fd), (char *)buf, (int)(len > 0x7fffffff ? 0x7fffffff : len), 0,
                     (struct sockaddr *)&addr, &slen);
    if (n == SOCKET_ERROR)
        return (zb_ssize_t)map_errno();
    if (ip && ip_len > 0)
        addr_to_string((struct sockaddr *)&addr, slen, ip, ip_len, port);
    return (zb_ssize_t)n;
}

zb_err_t zb_sock_close(int fd)
{
    if (fd < 0 || to_sock(fd) == INVALID_SOCKET)
        return ZB_OK;
    return closesocket(to_sock(fd)) == 0 ? ZB_OK : map_errno();
}

static unsigned long multicast_if_addr(const char *local_if)
{
    if (!local_if || !local_if[0] || strcmp(local_if, "0.0.0.0") == 0)
        return htonl(INADDR_ANY);
    return inet_addr(local_if);
}

zb_err_t zb_sock_set_broadcast(int fd, int on)
{
    BOOL v = on ? TRUE : FALSE;
    return setsockopt(to_sock(fd), SOL_SOCKET, SO_BROADCAST, (const char *)&v, sizeof(v)) == 0 ? ZB_OK
                                                                                            : ZB_ERR_PLATFORM;
}

zb_err_t zb_sock_set_multicast_ttl(int fd, uint8_t ttl)
{
    if (setsockopt(to_sock(fd), IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof(ttl)) != 0)
        return ZB_ERR_PLATFORM;
    return ZB_OK;
}

zb_err_t zb_sock_set_multicast_if(int fd, const char *local_ip)
{
    struct in_addr addr;
    addr.s_addr = multicast_if_addr(local_ip);
    if (setsockopt(to_sock(fd), IPPROTO_IP, IP_MULTICAST_IF, (const char *)&addr, sizeof(addr)) != 0)
        return ZB_ERR_PLATFORM;
    return ZB_OK;
}

zb_err_t zb_sock_set_multicast_loop(int fd, int on)
{
    BOOL v = on ? TRUE : FALSE;
    if (setsockopt(to_sock(fd), IPPROTO_IP, IP_MULTICAST_LOOP, (const char *)&v, sizeof(v)) != 0)
        return ZB_ERR_PLATFORM;
    return ZB_OK;
}

zb_err_t zb_sock_join_multicast(int fd, const char *group_addr, const char *local_if)
{
    struct ip_mreq imr;
    memset(&imr, 0, sizeof(imr));
    imr.imr_multiaddr.s_addr = inet_addr(group_addr);
    imr.imr_interface.s_addr = multicast_if_addr(local_if);
    if (setsockopt(to_sock(fd), IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&imr, sizeof(imr)) != 0)
        return map_errno();
    return ZB_OK;
}

zb_err_t zb_sock_leave_multicast(int fd, const char *group_addr, const char *local_if)
{
    struct ip_mreq imr;
    memset(&imr, 0, sizeof(imr));
    imr.imr_multiaddr.s_addr = inet_addr(group_addr);
    imr.imr_interface.s_addr = multicast_if_addr(local_if);
    if (setsockopt(to_sock(fd), IPPROTO_IP, IP_DROP_MEMBERSHIP, (const char *)&imr, sizeof(imr)) != 0)
        return map_errno();
    return ZB_OK;
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
    if (setsockopt(to_sock(fd), IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP, (const char *)&imr,
                   sizeof(imr)) != 0)
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

zb_err_t zb_sock_leave_multicast_filter(int fd, const char *group_addr, const char *src_ip,
                                                const char *local_if)
{
#if defined(IP_DROP_SOURCE_MEMBERSHIP)
    struct ip_mreq_source imr;
    memset(&imr, 0, sizeof(imr));
    imr.imr_multiaddr.s_addr = inet_addr(group_addr);
    imr.imr_sourceaddr.s_addr = inet_addr(src_ip);
    imr.imr_interface.s_addr = multicast_if_addr(local_if);
    if (setsockopt(to_sock(fd), IPPROTO_IP, IP_DROP_SOURCE_MEMBERSHIP, (const char *)&imr,
                   sizeof(imr)) != 0)
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
