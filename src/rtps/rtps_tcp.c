#include "rtps_tcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ntk.h>

#define NB_RTPS_TCP_BACKLOG 8
#define NB_RTPS_TCP_RECV_CHUNK 4096u
#define NB_RTPS_TCP_HEADER_SIZE 20u

typedef struct nb_rtps_tcp_conn {
    int used;
    int established;
    int initiated;
    int failed;
    ntk_socket *sock;
    int fd;
    char key[NB_RTPS_TCP_KEY_MAX];
    char host[NB_RTPS_TCP_HOST_MAX];
    uint16_t port;

    /* Stream reassembly. Only the participant thread touches this half. */
    uint8_t *rx;
    size_t rx_len;
    size_t rx_cap;
    int has_header;
    nb_rtps_header_t header;
    int has_source_ts;
    nb_rtps_time_t source_ts;

    /* Outbound queue, guarded by the instance mutex. */
    uint8_t *tx;
    size_t tx_len;
    size_t tx_cap;
} nb_rtps_tcp_conn_t;

struct nb_rtps_tcp {
    ntk_socket *listen_sock;
    int listen_fd;
    uint16_t port;
    ntk_poller *poller;
    ntk_mutex *lock;
    nb_rtps_tcp_message_fn on_message;
    nb_rtps_tcp_link_fn on_link;
    nb_rtps_tcp_link_fn on_unlink;
    void *user;
    nb_rtps_tcp_conn_t conns[NB_RTPS_TCP_MAX_CONNECTIONS];
    nb_rtps_tcp_stats_t stats;
};

static int key_to_index(const char *key)
{
    char *end = NULL;
    long value;

    if (!key || strncmp(key, NB_RTPS_TCP_KEY_PREFIX, 4) != 0) {
        return -1;
    }
    value = strtol(key + 4, &end, 10);
    if (!end || *end != '\0' || value < 0 ||
        value >= (long)NB_RTPS_TCP_MAX_CONNECTIONS) {
        return -1;
    }
    return (int)value;
}

int nb_rtps_tcp_key_index(const char *key)
{
    return key_to_index(key);
}

static int conn_reserve(nb_rtps_tcp_conn_t *conn, size_t *cap, uint8_t **buf, size_t need)
{
    size_t target;
    uint8_t *grown;

    if (need <= *cap) {
        return 0;
    }
    target = *cap ? *cap : 1024u;
    while (target < need) {
        target *= 2u;
    }
    grown = (uint8_t *)realloc(*buf, target);
    if (!grown) {
        return -1;
    }
    *buf = grown;
    *cap = target;
    (void)conn;
    return 0;
}

static void conn_free_buffers(nb_rtps_tcp_conn_t *conn)
{
    free(conn->rx);
    free(conn->tx);
    conn->rx = NULL;
    conn->tx = NULL;
    conn->rx_len = 0;
    conn->rx_cap = 0;
    conn->tx_len = 0;
    conn->tx_cap = 0;
}

static void poller_del(nb_rtps_tcp_t *tcp, int fd)
{
    if (tcp->poller && fd >= 0) {
        (void)ntk_poller_del(tcp->poller, fd);
    }
}

static void poller_add(nb_rtps_tcp_t *tcp, int fd)
{
    if (tcp->poller && fd >= 0) {
        (void)ntk_poller_add(tcp->poller, fd, NTK_POLL_IN, NULL, NULL);
    }
}

/* Free a slot. Called with the instance mutex held, and only after the
 * unlink callback has run: the callback needs to see the peer while the
 * address is still there. */
static int conn_release(nb_rtps_tcp_t *tcp, int index)
{
    nb_rtps_tcp_conn_t *conn = &tcp->conns[index];
    int was_used = conn->used;
    int fd = conn->fd;
    ntk_socket *sock = conn->sock;

    if (!was_used) {
        return 0;
    }
    conn->used = 0;
    conn->established = 0;
    conn->failed = 0;
    conn->sock = NULL;
    conn->fd = -1;
    conn->host[0] = '\0';
    conn->port = 0;
    conn->has_header = 0;
    conn->has_source_ts = 0;
    conn->rx_len = 0;
    conn->tx_len = 0;
    poller_del(tcp, fd);
    if (sock) {
        ntk_socket_destroy(sock);
    }
    return 1;
}

static int conn_alloc(nb_rtps_tcp_t *tcp,
                      ntk_socket *sock,
                      int initiated,
                      const char *host,
                      uint16_t port)
{
    int i;

    for (i = 0; i < (int)NB_RTPS_TCP_MAX_CONNECTIONS; i++) {
        nb_rtps_tcp_conn_t *conn = &tcp->conns[i];

        if (conn->used) {
            continue;
        }
        memset(conn, 0, sizeof(*conn));
        conn->used = 1;
        conn->initiated = initiated;
        conn->sock = sock;
        conn->fd = ntk_socket_fd(sock);
        conn->port = port;
        (void)snprintf(conn->key, sizeof(conn->key), "%s%d", NB_RTPS_TCP_KEY_PREFIX, i);
        if (host) {
            (void)snprintf(conn->host, sizeof(conn->host), "%s", host);
        }
        poller_add(tcp, conn->fd);
        return i;
    }
    return -1;
}

static int flush_locked(nb_rtps_tcp_t *tcp, nb_rtps_tcp_conn_t *conn)
{
    while (conn->tx_len > 0) {
        ntk_ssize_t n = ntk_socket_send(conn->sock, conn->tx, conn->tx_len);

        if (n > 0) {
            size_t sent = (size_t)n;

            if (sent < conn->tx_len) {
                memmove(conn->tx, conn->tx + sent, conn->tx_len - sent);
            }
            conn->tx_len -= sent;
            tcp->stats.bytes_sent += (uint64_t)sent;
            continue;
        }
        if (n == NTK_ERR_AGAIN) {
            return 0;
        }
        conn->failed = 1;
        return -1;
    }
    return 0;
}

static void conn_drop_rx(nb_rtps_tcp_conn_t *conn, size_t count)
{
    if (count >= conn->rx_len) {
        conn->rx_len = 0;
        return;
    }
    memmove(conn->rx, conn->rx + count, conn->rx_len - count);
    conn->rx_len -= count;
}

/*
 * Pull submessages out of the reassembly buffer. Submessages are self
 * delimiting, and every RTPS message starts with the 20-octet header, so the
 * stream is: header, submessage*, header, submessage*, ...
 */
static void conn_parse(nb_rtps_tcp_t *tcp, nb_rtps_tcp_conn_t *conn, int index)
{
    for (;;) {
        if (conn->rx_len >= 4 && memcmp(conn->rx, "RTPS", 4) == 0) {
            nb_rtps_header_t header;
            size_t offset = 0;

            if (conn->rx_len < NB_RTPS_TCP_HEADER_SIZE) {
                return;
            }
            if (nb_rtps_message_begin(conn->rx, conn->rx_len, &header, &offset) != NB_OK) {
                /* Not something we can parse. Drop what we have and wait for
                 * the peer to send a header we recognise. */
                conn->rx_len = 0;
                conn->has_header = 0;
                tcp->stats.rejected++;
                return;
            }
            conn->header = header;
            conn->has_header = 1;
            conn->has_source_ts = 0;
            tcp->stats.messages_received++;
            /* The header is the whole buffer we asked about; whatever followed
             * it is still in the reassembly buffer. */
            conn_drop_rx(conn, offset);
            continue;
        }
        if (!conn->has_header) {
            /* Leading bytes that are not a header: resynchronise. */
            conn->rx_len = 0;
            return;
        }
        if (conn->rx_len < 4) {
            return;
        }
        {
            nb_rtps_submessage_view_t view;
            uint8_t id = conn->rx[0];
            uint8_t flags = conn->rx[1];
            uint16_t octets = (uint16_t)((uint16_t)conn->rx[2] |
                                         ((uint16_t)conn->rx[3] << 8));
            size_t body_len;

            if (octets == 0) {
                /* A zero length means "to the end of the message". INFO_TS has
                 * a known size; anything else can only be delimited by the
                 * next header, which we cannot see yet. Treat the buffered
                 * bytes as the body and let the next header branch resync. */
                body_len = (id == NB_RTPS_SUBMSG_INFO_TS) ? 8u
                                                          : (conn->rx_len - 4u);
            } else {
                body_len = octets;
            }
            if (conn->rx_len < 4u + body_len) {
                return;
            }
            view.id = id;
            view.flags = flags;
            view.body = conn->rx + 4;
            view.body_len = body_len;
            if (tcp->on_message) {
                tcp->on_message(tcp->user,
                                index,
                                &conn->header,
                                &view,
                                &conn->has_source_ts,
                                &conn->source_ts);
            }
            if (!conn->used) {
                return;
            }
            conn_drop_rx(conn, 4u + body_len);
        }
    }
}

static void conn_read(nb_rtps_tcp_t *tcp, nb_rtps_tcp_conn_t *conn, int index)
{
    uint8_t chunk[NB_RTPS_TCP_RECV_CHUNK];

    for (;;) {
        ntk_ssize_t n = ntk_socket_recv(conn->sock, chunk, sizeof(chunk));

        if (n == 0) {
            conn->failed = 1;
            return;
        }
        if (n < 0) {
            if (n != NTK_ERR_AGAIN) {
                conn->failed = 1;
            }
            return;
        }
        if (conn->rx_len + (size_t)n > NB_RTPS_TCP_RX_MAX) {
            /* The peer is sending a message we cannot buffer. Drop the
             * connection instead of growing without bound. */
            tcp->stats.rejected++;
            conn->failed = 1;
            return;
        }
        if (conn_reserve(conn, &conn->rx_cap, &conn->rx, conn->rx_len + (size_t)n) != 0) {
            conn->failed = 1;
            return;
        }
        memcpy(conn->rx + conn->rx_len, chunk, (size_t)n);
        conn->rx_len += (size_t)n;
        tcp->stats.bytes_received += (uint64_t)n;
        conn_parse(tcp, conn, index);
        if (!conn->used) {
            return;
        }
    }
}

/* Accept everything pending. Returns 1 if at least one connection was made. */
static int accept_pending(nb_rtps_tcp_t *tcp)
{
    int accepted = 0;

    if (!tcp->listen_sock) {
        return 0;
    }
    for (;;) {
        ntk_socket *client = NULL;
        ntk_err_t rc = ntk_socket_accept(tcp->listen_sock, &client);
        int index;

        if (rc == NTK_ERR_AGAIN) {
            break;
        }
        if (rc != NTK_OK || !client) {
            break;
        }
        ntk_mutex_lock(tcp->lock);
        index = conn_alloc(tcp, client, 0, NULL, 0);
        if (index >= 0) {
            tcp->conns[index].established = 1;
        }
        ntk_mutex_unlock(tcp->lock);
        if (index < 0) {
            ntk_socket_destroy(client);
            tcp->stats.rejected++;
            continue;
        }
        accepted = 1;
        if (tcp->on_link) {
            tcp->on_link(tcp->user, index);
        }
    }
    return accepted;
}

nb_rtps_tcp_t *nb_rtps_tcp_create(const char *bind_host,
                                  uint16_t port,
                                  ntk_poller *poller,
                                  nb_rtps_tcp_message_fn on_message,
                                  nb_rtps_tcp_link_fn on_link,
                                  nb_rtps_tcp_link_fn on_unlink,
                                  void *user)
{
    nb_rtps_tcp_t *tcp;
    char local[64];
    uint16_t bound_port = 0;
    int i;

    tcp = (nb_rtps_tcp_t *)calloc(1, sizeof(*tcp));
    if (!tcp) {
        return NULL;
    }
    tcp->listen_fd = -1;
    tcp->poller = poller;
    tcp->on_message = on_message;
    tcp->on_link = on_link;
    tcp->on_unlink = on_unlink;
    tcp->user = user;
    tcp->lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    if (!tcp->lock) {
        free(tcp);
        return NULL;
    }
    for (i = 0; i < (int)NB_RTPS_TCP_MAX_CONNECTIONS; i++) {
        tcp->conns[i].fd = -1;
    }
    tcp->listen_sock = ntk_socket_create();
    if (!tcp->listen_sock ||
        ntk_socket_listen(tcp->listen_sock,
                          (bind_host && bind_host[0]) ? bind_host : NULL,
                          port,
                          NB_RTPS_TCP_BACKLOG) != NTK_OK ||
        ntk_socket_get_local(tcp->listen_sock, local, sizeof(local), &bound_port) !=
            NTK_OK) {
        nb_rtps_tcp_destroy(tcp);
        return NULL;
    }
    tcp->port = bound_port;
    tcp->listen_fd = ntk_socket_fd(tcp->listen_sock);
    poller_add(tcp, tcp->listen_fd);
    return tcp;
}

int nb_rtps_tcp_set_poller(nb_rtps_tcp_t *tcp, ntk_poller *poller)
{
    int i;

    if (!tcp) {
        return -1;
    }
    tcp->poller = poller;
    poller_add(tcp, tcp->listen_fd);
    for (i = 0; i < (int)NB_RTPS_TCP_MAX_CONNECTIONS; i++) {
        if (tcp->conns[i].used) {
            poller_add(tcp, tcp->conns[i].fd);
        }
    }
    return 0;
}

void nb_rtps_tcp_destroy(nb_rtps_tcp_t *tcp)
{
    int i;

    if (!tcp) {
        return;
    }
    for (i = 0; i < (int)NB_RTPS_TCP_MAX_CONNECTIONS; i++) {
        nb_rtps_tcp_conn_t *conn = &tcp->conns[i];

        if (conn->sock) {
            poller_del(tcp, conn->fd);
            ntk_socket_destroy(conn->sock);
            conn->sock = NULL;
        }
        conn_free_buffers(conn);
    }
    if (tcp->listen_sock) {
        poller_del(tcp, tcp->listen_fd);
        ntk_socket_destroy(tcp->listen_sock);
    }
    if (tcp->lock) {
        ntk_mutex_destroy(tcp->lock);
    }
    free(tcp);
}

uint16_t nb_rtps_tcp_port(const nb_rtps_tcp_t *tcp)
{
    return tcp ? tcp->port : 0;
}

static int address_equal(const char *a, uint16_t a_port, const char *b, uint16_t b_port)
{
    return a && b && a[0] && b[0] && a_port == b_port && strcmp(a, b) == 0;
}

int nb_rtps_tcp_has_link(const nb_rtps_tcp_t *tcp, const char *host, uint16_t port)
{
    int i;

    if (!tcp) {
        return 0;
    }
    for (i = 0; i < (int)NB_RTPS_TCP_MAX_CONNECTIONS; i++) {
        const nb_rtps_tcp_conn_t *conn = &tcp->conns[i];

        if (conn->used && address_equal(conn->host, conn->port, host, port)) {
            return 1;
        }
    }
    return 0;
}

int nb_rtps_tcp_find_link(const nb_rtps_tcp_t *tcp, const char *host, uint16_t port)
{
    int i;

    if (!tcp) {
        return -1;
    }
    for (i = 0; i < (int)NB_RTPS_TCP_MAX_CONNECTIONS; i++) {
        const nb_rtps_tcp_conn_t *conn = &tcp->conns[i];

        if (conn->used && conn->established &&
            address_equal(conn->host, conn->port, host, port)) {
            return i;
        }
    }
    return -1;
}

int nb_rtps_tcp_connect(nb_rtps_tcp_t *tcp, const char *host, uint16_t port)
{
    ntk_socket *sock;
    int in_progress = 0;
    int index;

    if (!tcp || !host || !host[0] || port == 0) {
        return -1;
    }
    if (nb_rtps_tcp_has_link(tcp, host, port)) {
        return -1;
    }
    sock = ntk_socket_create();
    if (!sock) {
        return -1;
    }
    if (ntk_socket_connect(sock, host, port, &in_progress) != NTK_OK) {
        ntk_socket_destroy(sock);
        return -1;
    }
    ntk_mutex_lock(tcp->lock);
    index = conn_alloc(tcp, sock, 1, host, port);
    if (index >= 0 && !in_progress) {
        tcp->conns[index].established = 1;
    }
    ntk_mutex_unlock(tcp->lock);
    if (index < 0) {
        ntk_socket_destroy(sock);
        return -1;
    }
    if (!in_progress && tcp->on_link) {
        tcp->on_link(tcp->user, index);
    }
    return index;
}

const char *nb_rtps_tcp_key(const nb_rtps_tcp_t *tcp, int index)
{
    if (!tcp || index < 0 || index >= (int)NB_RTPS_TCP_MAX_CONNECTIONS ||
        !tcp->conns[index].used) {
        return NULL;
    }
    return tcp->conns[index].key;
}

int nb_rtps_tcp_established(const nb_rtps_tcp_t *tcp, int index)
{
    if (!tcp || index < 0 || index >= (int)NB_RTPS_TCP_MAX_CONNECTIONS) {
        return 0;
    }
    return tcp->conns[index].used && tcp->conns[index].established;
}

void nb_rtps_tcp_link_info(const nb_rtps_tcp_t *tcp,
                           int index,
                           char *host,
                           size_t cap,
                           uint16_t *port)
{
    if (!tcp || index < 0 || index >= (int)NB_RTPS_TCP_MAX_CONNECTIONS || !host ||
        cap == 0) {
        if (host && cap > 0) {
            host[0] = '\0';
        }
        if (port) {
            *port = 0;
        }
        return;
    }
    (void)snprintf(host, cap, "%s", tcp->conns[index].host);
    if (port) {
        *port = tcp->conns[index].port;
    }
}

int nb_rtps_tcp_send(nb_rtps_tcp_t *tcp, int index, const void *data, size_t len)
{
    nb_rtps_tcp_conn_t *conn;
    int rc;

    if (!tcp || !data || len == 0 || index < 0 ||
        index >= (int)NB_RTPS_TCP_MAX_CONNECTIONS) {
        return -1;
    }
    ntk_mutex_lock(tcp->lock);
    conn = &tcp->conns[index];
    if (!conn->used || conn->failed) {
        ntk_mutex_unlock(tcp->lock);
        return -1;
    }
    if (conn->tx_len + len > NB_RTPS_TCP_TX_MAX ||
        conn_reserve(conn, &conn->tx_cap, &conn->tx, conn->tx_len + len) != 0) {
        tcp->stats.rejected++;
        ntk_mutex_unlock(tcp->lock);
        return -1;
    }
    memcpy(conn->tx + conn->tx_len, data, len);
    conn->tx_len += len;
    /* One call carries one serialised RTPS message: the participant layers
     * hand us exactly what it would have put in a datagram. */
    tcp->stats.messages_sent++;
    if (conn->established) {
        rc = flush_locked(tcp, conn);
    } else {
        rc = 0;
    }
    ntk_mutex_unlock(tcp->lock);
    return rc;
}

void nb_rtps_tcp_service(nb_rtps_tcp_t *tcp)
{
    int i;

    if (!tcp) {
        return;
    }
    (void)accept_pending(tcp);
    for (i = 0; i < (int)NB_RTPS_TCP_MAX_CONNECTIONS; i++) {
        nb_rtps_tcp_conn_t *conn = &tcp->conns[i];

        if (!conn->used) {
            continue;
        }
        if (!conn->established && conn->initiated && !conn->failed) {
            ntk_err_t rc = ntk_socket_check_connect(conn->sock);

            if (rc == NTK_ERR_AGAIN) {
                continue;
            }
            if (rc != NTK_OK) {
                conn->failed = 1;
            } else {
                conn->established = 1;
                if (tcp->on_link) {
                    tcp->on_link(tcp->user, i);
                }
                if (!conn->used) {
                    continue;
                }
            }
        }
        if (conn->established && !conn->failed) {
            conn_read(tcp, conn, i);
        }
        if (conn->used && !conn->failed) {
            ntk_mutex_lock(tcp->lock);
            if (conn->established && conn->tx_len > 0) {
                (void)flush_locked(tcp, conn);
            }
            ntk_mutex_unlock(tcp->lock);
        }
        if (conn->used && conn->failed) {
            if ((conn->established || !conn->initiated) && tcp->on_unlink) {
                /* Mark first so the callback sees a consistent table. */
                int fd = conn->fd;
                (void)fd;
                tcp->on_unlink(tcp->user, i);
            }
            ntk_mutex_lock(tcp->lock);
            (void)conn_release(tcp, i);
            ntk_mutex_unlock(tcp->lock);
        }
    }
}

size_t nb_rtps_tcp_links(const nb_rtps_tcp_t *tcp)
{
    size_t count = 0;
    int i;

    if (!tcp) {
        return 0;
    }
    for (i = 0; i < (int)NB_RTPS_TCP_MAX_CONNECTIONS; i++) {
        if (tcp->conns[i].used && tcp->conns[i].established) {
            count++;
        }
    }
    return count;
}

void nb_rtps_tcp_stats(const nb_rtps_tcp_t *tcp, nb_rtps_tcp_stats_t *out)
{
    if (!tcp || !out) {
        return;
    }
    *out = tcp->stats;
    out->links = (uint32_t)nb_rtps_tcp_links(tcp);
}
