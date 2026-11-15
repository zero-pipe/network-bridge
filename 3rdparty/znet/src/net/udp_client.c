#include "zn/net/udp_client.h"
#include "zn/net/socket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZN_UDP_CLIENT_RECV 65536

struct zn_udp_client {
    zn_socket *sock;
    zn_poller *poller;
    zn_udp_client_ops_t ops;
    void *user;

    char peer_host[64];
    uint16_t peer_port;
    int has_peer;
    int started;
    uint8_t *recv_buf;
    size_t recv_cap;
};

static int udp_client_ensure_recv_buf(zn_udp_client *client)
{
    if (!client)
        return 0;
    if (client->recv_buf && client->recv_cap >= ZN_UDP_CLIENT_RECV)
        return 1;
    uint8_t *p = (uint8_t *)realloc(client->recv_buf, ZN_UDP_CLIENT_RECV);
    if (!p)
        return 0;
    client->recv_buf = p;
    client->recv_cap = ZN_UDP_CLIENT_RECV;
    return 1;
}

static void udp_client_on_readable(zn_socket *sock, void *user)
{
    zn_udp_client *client = (zn_udp_client *)user;
    if (!client || !client->ops.on_packet)
        return;
    if (!udp_client_ensure_recv_buf(client))
        return;

    char from_ip[64];
    uint16_t from_port = 0;

    for (;;) {
        zn_ssize_t n = zn_socket_recvfrom(sock, client->recv_buf, client->recv_cap, from_ip, sizeof(from_ip),
            &from_port);
        if (n > 0) {
            client->ops.on_packet(client, client->recv_buf, (size_t)n, from_ip, from_port, client->user);
            continue;
        }
        if (n == ZN_ERR_AGAIN)
            break;
        if (client->ops.on_error)
            client->ops.on_error(client, client->user);
        break;
    }
}

static void udp_client_on_error(zn_socket *sock, void *user)
{
    (void)sock;
    zn_udp_client *client = (zn_udp_client *)user;
    if (client && client->ops.on_error)
        client->ops.on_error(client, client->user);
}

zn_udp_client *zn_udp_client_create(const zn_udp_client_opts_t *opts)
{
    if (!opts || !opts->poller || !opts->ops)
        return NULL;

    zn_udp_client *client = (zn_udp_client *)calloc(1, sizeof(*client));
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

void zn_udp_client_destroy(zn_udp_client *client)
{
    if (!client)
        return;
    if (client->started)
        zn_socket_detach_poller(client->sock);
    zn_socket_destroy(client->sock);
    free(client->recv_buf);
    free(client);
}

zn_err_t zn_udp_client_set_multicast(zn_udp_client *client, int ttl, const char *local_if)
{
    zn_err_t err;

    if (!client || !client->sock) {
        return ZN_ERR_INVALID;
    }
    err = zn_socket_set_multicast_ttl(client->sock, (uint8_t)(ttl > 0 ? ttl : 16));
    if (err != ZN_OK) {
        return err;
    }
    if (local_if && local_if[0]) {
        err = zn_socket_set_multicast_if(client->sock, local_if);
        if (err != ZN_OK) {
            return err;
        }
    }
    (void)zn_socket_set_multicast_loop(client->sock, 1);
    return ZN_OK;
}

static zn_err_t udp_client_bind_and_attach(zn_udp_client *client, const char *local_host, uint16_t local_port,
    int reuse)
{
    if (!client)
        return ZN_ERR_INVALID;

    if (client->started)
        zn_socket_detach_poller(client->sock);

    zn_err_t err = zn_socket_bind_udp(client->sock, local_host, local_port, reuse);
    if (err != ZN_OK)
        return err;

    zn_socket_callbacks_t cb = { udp_client_on_readable, NULL, udp_client_on_error };
    err = zn_socket_attach_poller(client->sock, client->poller, &cb, client);
    if (err != ZN_OK)
        return err;

    client->started = 1;
    return ZN_OK;
}

zn_err_t zn_udp_client_bind(zn_udp_client *client, const char *local_host, uint16_t local_port, int reuse)
{
    if (!client)
        return ZN_ERR_INVALID;
    client->has_peer = 0;
    client->peer_host[0] = '\0';
    client->peer_port = 0;
    return udp_client_bind_and_attach(client, local_host, local_port, reuse);
}

void zn_udp_client_set_peer(zn_udp_client *client, const char *peer_host, uint16_t peer_port)
{
    if (!client || !peer_host || !peer_host[0])
        return;
    snprintf(client->peer_host, sizeof(client->peer_host), "%s", peer_host);
    client->peer_port = peer_port;
    client->has_peer = 1;
}

zn_err_t zn_udp_client_start(zn_udp_client *client, const char *peer_host, uint16_t peer_port,
                                  const char *local_host, uint16_t local_port, int reuse)
{
    if (!client || !peer_host)
        return ZN_ERR_INVALID;

    zn_err_t err = udp_client_bind_and_attach(client, local_host, local_port, reuse);
    if (err != ZN_OK)
        return err;

    zn_udp_client_set_peer(client, peer_host, peer_port);
    return ZN_OK;
}

zn_err_t zn_udp_client_send(zn_udp_client *client, const void *data, size_t len)
{
    if (!client || !client->has_peer)
        return ZN_ERR_STATE;
    return zn_udp_client_sendto(client, data, len, client->peer_host, client->peer_port);
}

zn_err_t zn_udp_client_sendto(zn_udp_client *client, const void *data, size_t len, const char *ip,
                                  uint16_t port)
{
    if (!client || !client->started || !ip)
        return ZN_ERR_STATE;
    zn_ssize_t n = zn_socket_sendto(client->sock, data, len, ip, port);
    if (n < 0)
        return (zn_err_t)n;
    return ZN_OK;
}

uint16_t zn_udp_client_local_port(const zn_udp_client *client)
{
    uint16_t port = 0;
    if (client && client->started)
        zn_socket_get_local(client->sock, NULL, 0, &port);
    return port;
}
