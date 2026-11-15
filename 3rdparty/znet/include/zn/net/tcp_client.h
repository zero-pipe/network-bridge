#ifndef ZN_NET_TCP_CLIENT_H
#define ZN_NET_TCP_CLIENT_H

#include "../zn_export.h"
#include "../zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zn_poller;
struct zn_socket;
typedef struct zn_poller zn_poller;
typedef struct zn_socket zn_socket;
typedef struct zn_tcp_client zn_tcp_client;

typedef struct zn_tcp_client_ops {
    void (*on_connect)(zn_tcp_client *client, void *user);
    void (*on_recv)(zn_tcp_client *client, const void *data, size_t len, void *user);
    void (*on_error)(zn_tcp_client *client, void *user);
} zn_tcp_client_ops_t;

typedef struct zn_tcp_client_opts {
    zn_poller *poller;
    const zn_tcp_client_ops_t *ops;
    void *user;
} zn_tcp_client_opts_t;

ZN_API zn_tcp_client *zn_tcp_client_create(const zn_tcp_client_opts_t *opts);
ZN_API void zn_tcp_client_destroy(zn_tcp_client *client);

/** 非阻塞连接并注册到 poller；成功连通后触发 on_connect */
ZN_API zn_err_t zn_tcp_client_connect(zn_tcp_client *client, const char *host, uint16_t port);

ZN_API int zn_tcp_client_is_connected(const zn_tcp_client *client);
ZN_API zn_socket *zn_tcp_client_socket(zn_tcp_client *client);
ZN_API zn_err_t zn_tcp_client_send(zn_tcp_client *client, const void *data, size_t len);
ZN_API void zn_tcp_client_close(zn_tcp_client *client);

#ifdef __cplusplus
}
#endif

#endif /* ZN_NET_TCP_CLIENT_H */
