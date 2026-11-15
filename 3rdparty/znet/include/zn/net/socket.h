#ifndef ZN_NET_SOCKET_H
#define ZN_NET_SOCKET_H

#include "../zn_export.h"
#include "../zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER)
#  include <BaseTsd.h>
typedef SSIZE_T zn_ssize_t;
#elif defined(__GNUC__) || defined(__linux__)
#  include <sys/types.h>
typedef ssize_t zn_ssize_t;
#else
typedef ptrdiff_t zn_ssize_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct zn_poller;
typedef struct zn_poller zn_poller;
typedef struct zn_socket zn_socket;

typedef enum zn_sock_type {
    ZN_SOCK_NONE = 0,
    ZN_SOCK_TCP_CLIENT,
    ZN_SOCK_TCP_LISTEN,
    ZN_SOCK_TCP_CONNECTED,
    ZN_SOCK_UDP
} zn_sock_type_t;

/**
 * Poller 就绪回调（在 poll 线程内调用，勿阻塞）。
 * on_readable：可读（TCP 已连接 / UDP / listen 可 accept）
 * on_writable：可写（含异步 connect 完成前需监听 OUT）
 * on_error：错误或对端关闭（EPOLLERR/HUP）
 */
typedef struct zn_socket_callbacks {
    void (*on_readable)(zn_socket *sock, void *user);
    void (*on_writable)(zn_socket *sock, void *user);
    void (*on_error)(zn_socket *sock, void *user);
} zn_socket_callbacks_t;

ZN_API zn_socket *zn_socket_create(void);
ZN_API void zn_socket_destroy(zn_socket *sock);

/** 接管已有 fd（设为非阻塞 + CLOEXEC），成功后由 zn_socket 负责 close */
ZN_API zn_err_t zn_socket_adopt(zn_socket *sock, int fd, zn_sock_type_t type);

ZN_API int zn_socket_fd(const zn_socket *sock);
ZN_API zn_sock_type_t zn_socket_type(const zn_socket *sock);

/** host 可为 NULL / "" / "0.0.0.0"；port=0 表示由系统分配 */
ZN_API zn_err_t zn_socket_listen(zn_socket *sock, const char *host, uint16_t port, int backlog);

/** @param reuse SO_REUSEADDR */
ZN_API zn_err_t zn_socket_bind_udp(zn_socket *sock, const char *host, uint16_t port, int reuse);

/** reuse_addr / reuse_port 对应 SO_REUSEADDR / SO_REUSEPORT */
ZN_API zn_err_t zn_socket_bind_udp_ex(zn_socket *sock, const char *host, uint16_t port,
                                            int reuse_addr, int reuse_port);

/**
 * TCP 连接。已连接返回 ZN_OK；非阻塞进行中也返回 ZN_OK 且 *in_progress=1。
 * 失败返回负错误码。
 */
ZN_API zn_err_t zn_socket_connect(zn_socket *sock, const char *host, uint16_t port, int *in_progress);

/** 异步 connect 后，在 on_writable 或 poll OUT 后调用，检查是否真正连通 */
ZN_API zn_err_t zn_socket_check_connect(zn_socket *sock);

/** 从监听 socket accept，*out_client 为新分配的 socket */
ZN_API zn_err_t zn_socket_accept(zn_socket *listen_sock, zn_socket **out_client);

typedef struct zn_socket_iov {
    const void *base;
    size_t len;
} zn_socket_iov;

#define ZN_SOCKET_IOV_MAX 16

ZN_API zn_ssize_t zn_socket_send(zn_socket *sock, const void *data, size_t len);
ZN_API zn_ssize_t zn_socket_sendv(zn_socket *sock, const zn_socket_iov *iov, unsigned count);

/** Linux 可选 BBR；Win32 返回 ZN_ERR_NOT_IMPL */
ZN_API zn_err_t zn_socket_set_bbr(zn_socket *sock, int on);
ZN_API zn_ssize_t zn_socket_recv(zn_socket *sock, void *buf, size_t len);

ZN_API zn_ssize_t zn_socket_sendto(zn_socket *sock, const void *data, size_t len,
                                         const char *ip, uint16_t port);
ZN_API zn_ssize_t zn_socket_recvfrom(zn_socket *sock, void *buf, size_t len,
                                           char *ip, size_t ip_len, uint16_t *port);

ZN_API zn_err_t zn_socket_get_local(const zn_socket *sock, char *ip, size_t ip_len, uint16_t *port);
ZN_API zn_err_t zn_socket_get_peer(const zn_socket *sock, char *ip, size_t ip_len, uint16_t *port);

/** 注册到 poller；listen 默认 IN，connect 进行中会附加 OUT */
ZN_API zn_err_t zn_socket_attach_poller(zn_socket *sock, zn_poller *poller,
                                              const zn_socket_callbacks_t *cb, void *user);
ZN_API zn_err_t zn_socket_detach_poller(zn_socket *sock);

/** 修改关注事件（已 attach 后） */
ZN_API zn_err_t zn_socket_mod_events(zn_socket *sock, unsigned events);

/** UDP：开启广播发送（SO_BROADCAST） */
ZN_API zn_err_t zn_socket_set_broadcast(zn_socket *sock, int on);

ZN_API zn_err_t zn_socket_set_multicast_ttl(zn_socket *sock, uint8_t ttl);
ZN_API zn_err_t zn_socket_set_multicast_if(zn_socket *sock, const char *local_ip);
ZN_API zn_err_t zn_socket_set_multicast_loop(zn_socket *sock, int on);
ZN_API zn_err_t zn_socket_join_multicast(zn_socket *sock, const char *group_addr,
                                               const char *local_if);
ZN_API zn_err_t zn_socket_leave_multicast(zn_socket *sock, const char *group_addr,
                                                const char *local_if);
ZN_API zn_err_t zn_socket_join_multicast_filter(zn_socket *sock, const char *group_addr,
                                                      const char *src_ip, const char *local_if);
ZN_API zn_err_t zn_socket_leave_multicast_filter(zn_socket *sock, const char *group_addr,
                                                       const char *src_ip, const char *local_if);

#ifdef __cplusplus
}
#endif

#endif /* ZN_NET_SOCKET_H */
