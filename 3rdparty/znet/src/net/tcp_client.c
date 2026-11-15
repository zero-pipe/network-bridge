#include "zn/net/tcp_client.h"
#include "zn/net/socket.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ZN_TCP_CLIENT_RECV 4096

struct zn_tcp_client {
    zn_socket *sock;
    zn_poller *poller;
    zn_tcp_client_ops_t ops;
    void *user;
    int connected;
    int closing;
};

static void client_on_error(zn_socket *sock, void *user);
static void client_on_readable(zn_socket *sock, void *user);
static void client_on_writable(zn_socket *sock, void *user);

static void fire_error(zn_tcp_client *client)
{
    if (!client || client->closing)
        return;
    client->closing = 1;
    if (client->ops.on_error)
        client->ops.on_error(client, client->user);
    zn_socket_detach_poller(client->sock);
}

static void client_on_writable(zn_socket *sock, void *user)
{
    zn_tcp_client *client = (zn_tcp_client *)user;
    if (!client || client->closing || client->connected)
        return;

    zn_err_t err = zn_socket_check_connect(sock);
    if (err == ZN_OK) {
        client->connected = 1;
        if (client->ops.on_connect)
            client->ops.on_connect(client, client->user);
        return;
    }
    if (err != ZN_ERR_AGAIN)
        fire_error(client);
}

static void client_on_readable(zn_socket *sock, void *user)
{
    zn_tcp_client *client = (zn_tcp_client *)user;
    if (!client || client->closing)
        return;

    if (!client->connected) {
        client_on_writable(sock, user);
        if (!client->connected || client->closing)
            return;
    }

    char buf[ZN_TCP_CLIENT_RECV];
    zn_ssize_t n = zn_socket_recv(sock, buf, sizeof(buf));
    if (n > 0) {
        if (client->ops.on_recv)
            client->ops.on_recv(client, buf, (size_t)n, client->user);
        return;
    }
    if (n == 0 || (n < 0 && n != ZN_ERR_AGAIN))
        fire_error(client);
}

static void client_on_error(zn_socket *sock, void *user)
{
    (void)sock;
    fire_error((zn_tcp_client *)user);
}

zn_tcp_client *zn_tcp_client_create(const zn_tcp_client_opts_t *opts)
{
    if (!opts || !opts->poller || !opts->ops)
        return NULL;

    zn_tcp_client *client = (zn_tcp_client *)calloc(1, sizeof(*client));
    if (!client)
        return NULL;

    client->sock = zn_socket_create();
    if (!client->sock) {
        free(client);
        return NULL;
    }

    client->poller = opts->poller;
    client->ops = *opts->ops;
    client->user = opts->user;
    return client;
}

void zn_tcp_client_destroy(zn_tcp_client *client)
{
    if (!client)
        return;
    zn_tcp_client_close(client);
    zn_socket_destroy(client->sock);
    free(client);
}

zn_err_t zn_tcp_client_connect(zn_tcp_client *client, const char *host, uint16_t port)
{
    if (!client || client->closing)
        return ZN_ERR_INVALID;

    zn_socket_detach_poller(client->sock);
    client->connected = 0;

    int pending = 0;
    zn_err_t err = zn_socket_connect(client->sock, host, port, &pending);
    if (err != ZN_OK)
        return err;

    zn_socket_callbacks_t cb = { client_on_readable, client_on_writable, client_on_error };
    err = zn_socket_attach_poller(client->sock, client->poller, &cb, client);
    if (err != ZN_OK)
        return err;

    if (!pending) {
        client->connected = 1;
        if (client->ops.on_connect)
            client->ops.on_connect(client, client->user);
    }
    return ZN_OK;
}

int zn_tcp_client_is_connected(const zn_tcp_client *client)
{
    return client && client->connected && !client->closing;
}

zn_socket *zn_tcp_client_socket(zn_tcp_client *client)
{
    return client ? client->sock : NULL;
}

zn_err_t zn_tcp_client_send(zn_tcp_client *client, const void *data, size_t len)
{
    if (!client || !client->connected || client->closing || !data || len == 0)
        return ZN_ERR_STATE;
    const uint8_t *p = (const uint8_t *)data;
    size_t off = 0;
    int again_spin = 0;
    while (off < len) {
        zn_ssize_t n = zn_socket_send(client->sock, p + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            again_spin = 0;
            continue;
        }
        if (n == ZN_ERR_AGAIN) {
            if (++again_spin > 64)
                return ZN_ERR_AGAIN;
            continue;
        }
        if (n < 0)
            return (zn_err_t)n;
        return ZN_ERR_IO;
    }
    return ZN_OK;
}

void zn_tcp_client_close(zn_tcp_client *client)
{
    if (!client)
        return;
    /* fire_error 会置 closing=1；此处必须始终重建 socket，不能因 closing 直接 return */
    client->closing = 1;
    zn_socket_detach_poller(client->sock);
    zn_socket_destroy(client->sock);
    client->sock = zn_socket_create();
    client->connected = 0;
    client->closing = 0;
}
