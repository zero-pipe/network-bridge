#ifndef ZN_NET_UDP_SERVER_H
#define ZN_NET_UDP_SERVER_H

#include "../zn_export.h"
#include "../zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zn_poller;
struct zn_poller_pool;
typedef struct zn_poller zn_poller;
typedef struct zn_poller_pool zn_poller_pool;
typedef struct zn_udp_server zn_udp_server;

typedef struct zn_udp_server_ops {
  void (*on_packet)(zn_udp_server *srv, const char *peer_ip, uint16_t peer_port,
                    const void *data, size_t len, void *user);
} zn_udp_server_ops_t;

typedef struct zn_udp_server_opts {
    const char *host;
    uint16_t port;
    /** SO_REUSEADDR；多 poller 时强制为 1 */
    int reuse;
    /**
     * SO_REUSEPORT：每个 poller 独立 bind 同端口，内核将入站包分到各 fd（抢占收包）。
     * poller_count > 1 时自动开启；单 poller 时可显式置 1。
     */
    int reuse_port;

    zn_poller *poller;
    /** 与 poller 二选一 */
    zn_poller_pool *poller_pool;

    const zn_udp_server_ops_t *ops;
    void *user;
} zn_udp_server_opts_t;

ZN_API zn_udp_server *zn_udp_server_create(const zn_udp_server_opts_t *opts);
ZN_API void zn_udp_server_destroy(zn_udp_server *srv);

ZN_API zn_err_t zn_udp_server_start(zn_udp_server *srv);
ZN_API void zn_udp_server_stop(zn_udp_server *srv);

ZN_API uint16_t zn_udp_server_port(const zn_udp_server *srv);
ZN_API unsigned zn_udp_server_poller_count(const zn_udp_server *srv);
ZN_API zn_poller *zn_udp_server_poller(const zn_udp_server *srv, unsigned index);

ZN_API zn_err_t zn_udp_server_sendto(zn_udp_server *srv, const char *ip, uint16_t port,
                                           const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZN_NET_UDP_SERVER_H */
