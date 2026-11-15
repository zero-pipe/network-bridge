#include "zn/net/tls_client.h"
#include "zn/net/ssl.h"
#include "zn/net/socket.h"
#include "zn/poller/poller.h"
#include "internal/ssl_ctx_internal.h"
#if defined(_WIN32)
#  include <winsock2.h>
#endif
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TLS_RECV_BUF 16384
#define TLS_SEND_BUF 65536

typedef enum {
    TLS_ST_TCP = 0,
    TLS_ST_HANDSHAKE,
    TLS_ST_READY,
} tls_state_t;

struct zn_tls_client {
    zn_tls_client_opts_t opts;
    zn_socket *sock;
    SSL *ssl_handle;
    tls_state_t state;
    int tcp_connected;
    int closing;
    int ready_sent;
    char host_for_connect[256];
    char sni_name[256];
    uint8_t pending[TLS_SEND_BUF];
    size_t pending_len;
};

static void tls_fire_error(zn_tls_client *c);
static void tls_mod_poll(zn_tls_client *c, int want_write);
static zn_err_t tls_do_handshake(zn_tls_client *c);
static void tls_drain_plain(zn_tls_client *c);
static void tls_flush_pending(zn_tls_client *c);

static void tls_mod_poll(zn_tls_client *c, int want_write)
{
    if (!c || !c->sock)
        return;
    unsigned ev = ZN_POLL_IN | ZN_POLL_ERR | ZN_POLL_HUP;
    if (want_write || !c->tcp_connected)
        ev |= ZN_POLL_OUT;
    zn_socket_mod_events(c->sock, ev);
}

static void tls_fire_error(zn_tls_client *c)
{
    if (!c || c->closing)
        return;
    c->closing = 1;
    zn_socket_detach_poller(c->sock);
    if (c->ssl_handle) {
        SSL_free(c->ssl_handle);
        c->ssl_handle = NULL;
    }
    c->state = TLS_ST_TCP;
    c->tcp_connected = 0;
    c->ready_sent = 0;
    c->pending_len = 0;
    if (c->opts.ops && c->opts.ops->on_error)
        c->opts.ops->on_error(c, c->opts.user);
}

static zn_err_t tls_ssl_error(zn_tls_client *c, int ret, const char *where)
{
    int err = SSL_get_error(c->ssl_handle, ret);
    if (err == SSL_ERROR_WANT_READ) {
        tls_mod_poll(c, 0);
        return ZN_ERR_AGAIN;
    }
    if (err == SSL_ERROR_WANT_WRITE) {
        tls_mod_poll(c, 1);
        return ZN_ERR_AGAIN;
    }
    char buf[256];
    unsigned long e = ERR_get_error();
    if (e)
        ERR_error_string_n(e, buf, sizeof(buf));
    else
        snprintf(buf, sizeof(buf), "%s ssl_err=%d", where ? where : "ssl", err);
    zn_set_last_error(buf);
    tls_fire_error(c);
    return ZN_ERR_IO;
}

static zn_err_t tls_do_handshake(zn_tls_client *c)
{
    if (!c || !c->ssl_handle)
        return ZN_ERR_INVALID;
    int r = SSL_connect(c->ssl_handle);
    if (r == 1) {
        c->state = TLS_ST_READY;
        tls_mod_poll(c, 0);
        if (!c->ready_sent) {
            c->ready_sent = 1;
            if (c->opts.ops && c->opts.ops->on_connect)
                c->opts.ops->on_connect(c, c->opts.user);
        }
        tls_flush_pending(c);
        tls_drain_plain(c);
        return ZN_OK;
    }
    return tls_ssl_error(c, r, "SSL_connect");
}

static void tls_drain_plain(zn_tls_client *c)
{
    if (!c || c->state != TLS_ST_READY || !c->ssl_handle || c->closing)
        return;

    uint8_t buf[TLS_RECV_BUF];
    for (;;) {
        int n = SSL_read(c->ssl_handle, buf, sizeof(buf));
        if (n > 0) {
            if (c->opts.ops && c->opts.ops->on_recv)
                c->opts.ops->on_recv(c, buf, (size_t)n, c->opts.user);
            continue;
        }
        if (n == 0) {
            tls_fire_error(c);
            return;
        }
        int err = SSL_get_error(c->ssl_handle, n);
        if (err == SSL_ERROR_WANT_READ) {
            tls_mod_poll(c, 0);
            return;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            tls_mod_poll(c, 1);
            return;
        }
        tls_ssl_error(c, n, "SSL_read");
        return;
    }
}

static void tls_flush_pending(zn_tls_client *c)
{
    if (!c || !c->pending_len || c->state != TLS_ST_READY || !c->ssl_handle)
        return;
    size_t off = 0;
    while (off < c->pending_len) {
        int n = SSL_write(c->ssl_handle, c->pending + off, (int)(c->pending_len - off));
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (tls_ssl_error(c, n, "SSL_write") == ZN_ERR_AGAIN) {
            if (off > 0) {
                memmove(c->pending, c->pending + off, c->pending_len - off);
                c->pending_len -= off;
            }
            return;
        }
        return;
    }
    c->pending_len = 0;
}

static void tls_start_ssl(zn_tls_client *c)
{
    if (!c || !c->opts.ssl_ctx || !c->opts.ssl_ctx->ctx)
        return;

    c->ssl_handle = SSL_new(c->opts.ssl_ctx->ctx);
    if (!c->ssl_handle) {
        zn_set_last_error("SSL_new failed");
        tls_fire_error(c);
        return;
    }

    if (SSL_set_fd(c->ssl_handle, (int)zn_socket_fd(c->sock)) != 1) {
        zn_set_last_error("SSL_set_fd failed");
        tls_fire_error(c);
        return;
    }

    const char *sni = c->sni_name[0] ? c->sni_name : c->host_for_connect;
    if (sni[0])
        (void)SSL_ctrl(c->ssl_handle, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, (void *)sni);

    c->state = TLS_ST_HANDSHAKE;
    c->ready_sent = 0;
    tls_mod_poll(c, 1);
    (void)tls_do_handshake(c);
}

static void sock_on_writable(zn_socket *sock, void *user)
{
    zn_tls_client *c = (zn_tls_client *)user;
    if (!c || c->closing)
        return;

    if (!c->tcp_connected) {
        zn_err_t err = zn_socket_check_connect(sock);
        if (err == ZN_OK) {
            c->tcp_connected = 1;
            tls_start_ssl(c);
            return;
        }
        if (err != ZN_ERR_AGAIN)
            tls_fire_error(c);
        return;
    }

    if (c->state == TLS_ST_HANDSHAKE)
        (void)tls_do_handshake(c);
    else if (c->state == TLS_ST_READY) {
        tls_flush_pending(c);
        tls_drain_plain(c);
    }
}

static void sock_on_readable(zn_socket *sock, void *user)
{
    zn_tls_client *c = (zn_tls_client *)user;
    if (!c || c->closing)
        return;

    if (!c->tcp_connected) {
        sock_on_writable(sock, user);
        if (!c->tcp_connected || c->closing)
            return;
    }

    if (c->state == TLS_ST_HANDSHAKE)
        (void)tls_do_handshake(c);
    else if (c->state == TLS_ST_READY)
        tls_drain_plain(c);
}

static void sock_on_error(zn_socket *sock, void *user)
{
    (void)sock;
    tls_fire_error((zn_tls_client *)user);
}

zn_tls_client *zn_tls_client_create(const zn_tls_client_opts_t *opts)
{
    if (!opts || !opts->poller || !opts->ops || !opts->ssl_ctx || !opts->ssl_ctx->ctx)
        return NULL;
    if (zn_ssl_global_init() != ZN_OK)
        return NULL;

    zn_tls_client *c = (zn_tls_client *)calloc(1, sizeof(*c));
    if (!c)
        return NULL;

    c->opts = *opts;
    if (opts->sni_host && opts->sni_host[0])
        strncpy(c->sni_name, opts->sni_host, sizeof(c->sni_name) - 1);

    c->sock = zn_socket_create();
    if (!c->sock) {
        free(c);
        return NULL;
    }
    return c;
}

void zn_tls_client_destroy(zn_tls_client *client)
{
    if (!client)
        return;
    zn_tls_client_close(client);
    if (client->ssl_handle) {
        SSL_free(client->ssl_handle);
        client->ssl_handle = NULL;
    }
    if (client->sock) {
        zn_socket_destroy(client->sock);
        client->sock = NULL;
    }
    free(client);
}

zn_err_t zn_tls_client_connect(zn_tls_client *client, const char *host, uint16_t port)
{
    if (!client || client->closing || !host)
        return ZN_ERR_INVALID;

    zn_socket_detach_poller(client->sock);
    if (client->ssl_handle) {
        SSL_free(client->ssl_handle);
        client->ssl_handle = NULL;
    }
    client->state = TLS_ST_TCP;
    client->tcp_connected = 0;
    client->ready_sent = 0;
    client->pending_len = 0;

    strncpy(client->host_for_connect, host, sizeof(client->host_for_connect) - 1);
    if (!client->sni_name[0])
        strncpy(client->sni_name, host, sizeof(client->sni_name) - 1);

    int pending = 0;
    zn_err_t err = zn_socket_connect(client->sock, host, port, &pending);
    if (err != ZN_OK)
        return err;

    zn_socket_callbacks_t cb = { sock_on_readable, sock_on_writable, sock_on_error };
    err = zn_socket_attach_poller(client->sock, client->opts.poller, &cb, client);
    if (err != ZN_OK)
        return err;

    if (!pending) {
        client->tcp_connected = 1;
        tls_start_ssl(client);
    }
    return ZN_OK;
}

int zn_tls_client_is_connected(const zn_tls_client *client)
{
    return client && client->state == TLS_ST_READY && !client->closing && client->ready_sent;
}

zn_socket *zn_tls_client_socket(zn_tls_client *client)
{
    return client ? client->sock : NULL;
}

zn_err_t zn_tls_client_send(zn_tls_client *client, const void *data, size_t len)
{
    if (!client || client->closing || !data || len == 0)
        return ZN_ERR_INVALID;

    if (client->state != TLS_ST_READY || !client->ssl_handle) {
        if (client->pending_len + len > TLS_SEND_BUF)
            return ZN_ERR_BUFFER_TOO_SMALL;
        memcpy(client->pending + client->pending_len, data, len);
        client->pending_len += len;
        return ZN_ERR_AGAIN;
    }

    const uint8_t *p = (const uint8_t *)data;
    size_t off = 0;
    while (off < len) {
        int n = SSL_write(client->ssl_handle, p + off, (int)(len - off));
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        zn_err_t err = tls_ssl_error(client, n, "SSL_write");
        if (err == ZN_ERR_AGAIN && off < len) {
            size_t rest = len - off;
            if (client->pending_len + rest > TLS_SEND_BUF)
                return ZN_ERR_BUFFER_TOO_SMALL;
            memcpy(client->pending + client->pending_len, p + off, rest);
            client->pending_len += rest;
        }
        return err;
    }
    return ZN_OK;
}

void zn_tls_client_close(zn_tls_client *client)
{
    if (!client)
        return;
    /* 允许在 tls_fire_error() 之后再次 close：此时 closing 已置位，
     * 但仍需重置 socket/状态，否则后续 connect 会一直返回 ZN_ERR_INVALID。 */
    client->closing = 1;
    zn_socket_detach_poller(client->sock);
    if (client->ssl_handle) {
        SSL_shutdown(client->ssl_handle);
        SSL_free(client->ssl_handle);
        client->ssl_handle = NULL;
    }
    zn_socket_destroy(client->sock);
    client->sock = zn_socket_create();
    client->state = TLS_ST_TCP;
    client->tcp_connected = 0;
    client->ready_sent = 0;
    client->pending_len = 0;
    client->closing = 0;
}
