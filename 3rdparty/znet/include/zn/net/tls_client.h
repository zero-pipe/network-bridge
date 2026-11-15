#ifndef ZN_NET_TLS_CLIENT_H
#define ZN_NET_TLS_CLIENT_H

#include "../zn_export.h"
#include "../zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zn_poller;
struct zn_ssl_ctx;
struct zn_socket;
typedef struct zn_poller zn_poller;
typedef struct zn_ssl_ctx zn_ssl_ctx;
typedef struct zn_socket zn_socket;
typedef struct zn_tls_client zn_tls_client;

typedef struct zn_tls_client_ops {
    void (*on_connect)(zn_tls_client *client, void *user);
    void (*on_recv)(zn_tls_client *client, const void *data, size_t len, void *user);
    void (*on_error)(zn_tls_client *client, void *user);
} zn_tls_client_ops_t;

typedef struct zn_tls_client_opts {
    zn_poller *poller;
    zn_ssl_ctx *ssl_ctx;
    const zn_tls_client_ops_t *ops;
    void *user;
    /**
     * TLS SNI / 校验用主机名；NULL 则使用 connect 的 host。
     */
    const char *sni_host;
} zn_tls_client_opts_t;

ZN_API zn_tls_client *zn_tls_client_create(const zn_tls_client_opts_t *opts);
ZN_API void zn_tls_client_destroy(zn_tls_client *client);

/** TCP 连通并完成 TLS 握手后触发 on_connect */
ZN_API zn_err_t zn_tls_client_connect(zn_tls_client *client, const char *host, uint16_t port);

ZN_API int zn_tls_client_is_connected(const zn_tls_client *client);
ZN_API zn_socket *zn_tls_client_socket(zn_tls_client *client);
ZN_API zn_err_t zn_tls_client_send(zn_tls_client *client, const void *data, size_t len);
ZN_API void zn_tls_client_close(zn_tls_client *client);

#ifdef __cplusplus
}
#endif

#endif /* ZN_NET_TLS_CLIENT_H */
