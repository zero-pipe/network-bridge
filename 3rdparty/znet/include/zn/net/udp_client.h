#ifndef ZN_NET_UDP_CLIENT_H
#define ZN_NET_UDP_CLIENT_H

#include "../zn_export.h"
#include "../zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zn_poller;
typedef struct zn_poller zn_poller;
typedef struct zn_udp_client zn_udp_client;

typedef struct zn_udp_client_ops {
    void (*on_packet)(zn_udp_client *client, const void *data, size_t len, const char *from_ip,
                      uint16_t from_port, void *user);
    void (*on_error)(zn_udp_client *client, void *user);
} zn_udp_client_ops_t;

typedef struct zn_udp_client_opts {
    zn_poller *poller;
    const zn_udp_client_ops_t *ops;
    void *user;
} zn_udp_client_opts_t;

ZN_API zn_udp_client *zn_udp_client_create(const zn_udp_client_opts_t *opts);
ZN_API void zn_udp_client_destroy(zn_udp_client *client);

/**
 * 绑定本地 UDP，并记录默认对端（用于 zn_udp_client_send）。
 * local_port=0 由系统分配；peer 可为组播地址。
 */
ZN_API zn_err_t zn_udp_client_start(zn_udp_client *client, const char *peer_host, uint16_t peer_port,
                                          const char *local_host, uint16_t local_port, int reuse);

/** 仅绑定本地端口并挂到 poller；推流服务端收 RTP 前使用，对端由 set_peer 或 sendto 指定 */
ZN_API zn_err_t zn_udp_client_bind(zn_udp_client *client, const char *local_host, uint16_t local_port,
                                         int reuse);

/** 组播发送参数（须在 start/bind 前调用）：ttl<=0 用默认 16；local_if 可空 */
ZN_API zn_err_t zn_udp_client_set_multicast(zn_udp_client *client, int ttl, const char *local_if);

/** NAT 打洞后更新默认 send 目标 */
ZN_API void zn_udp_client_set_peer(zn_udp_client *client, const char *peer_host, uint16_t peer_port);

ZN_API zn_err_t zn_udp_client_send(zn_udp_client *client, const void *data, size_t len);
ZN_API zn_err_t zn_udp_client_sendto(zn_udp_client *client, const void *data, size_t len,
                                           const char *ip, uint16_t port);

ZN_API uint16_t zn_udp_client_local_port(const zn_udp_client *client);

#ifdef __cplusplus
}
#endif

#endif /* ZN_NET_UDP_CLIENT_H */
