#include "zn/net/tcp_server.h"
#include "zn/net/net_limits.h"
#include "zn/util/buf.h"
#include "zn/util/timer.h"
#include "zn/poller/poller.h"
#include "zn/poller/poller_pool.h"
#include "zn/net/socket.h"
#include "zn/net/ssl.h"
#include "zb/os/sync.h"
#include "zb/os/sem.h"
#include "zb/os/atomic.h"
#include "zn/platform.h"
#include "zn/util/log.h"
#include "zn_config.h"
#if ZN_HAVE_OPENSSL
#include "internal/ssl_ctx_internal.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#define ZN_TCP_TLS 1
#else
#define ZN_TCP_TLS 0
#endif
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#define ZN_TCP_RECV_CHUNK 4096
#define ZN_TCP_DEFAULT_MANAGER_SEC 2.0f

#ifdef ZN_TEST_HOOKS
static volatile size_t g_zn_test_out_mem_append;
#endif

typedef struct zn_listen_binding {
    zn_tcp_server *server;
    zn_poller *poller;
    zn_poller_timer *manager_timer;
} zn_listen_binding;

enum {
    ZN_TCP_OUT_MEM = 0,
    ZN_TCP_OUT_BUF = 1
};

typedef struct zn_tcp_out_chunk {
    struct zn_tcp_out_chunk *next;
    int kind;
    size_t off;
    size_t len;
    union {
        uint8_t *mem;
        zn_buf *buf;
    } u;
} zn_tcp_out_chunk;

struct zn_tcp_session {
    zn_tcp_server *server;
    zn_socket *sock;
    zn_poller *poller;
    void *user;
    int closing;
    int32_t ref;
#if ZN_TCP_TLS
    SSL *ssl;
    int tls_ready;
    int tls_want_out;
#endif
    zn_tcp_out_chunk *out_head;
    zn_tcp_out_chunk *out_tail;
    size_t out_pending;
    int out_highwater_hit;
    /** 当前是否已向 poller 注册可写事件（发送积压时需要，否则数据永不发出） */
    int want_out;
    /** 建立时刻与最近活动时刻（ms，收发任一方向），供握手/空闲超时判断 */
    uint64_t created_ms;
    uint64_t last_rx_ms;
    uint64_t last_active_ms;
    int saw_rx;
    /** 对端 IPv4（网络序），用于 per-IP 连接上限计数 */
    uint32_t peer_ipv4;
};

struct zn_tcp_server {
    zn_socket *listen_sock;
    zn_poller **pollers;
    unsigned poller_count;

    zn_tcp_session_ops_t ops;
    void *session_user;
    void *(*session_create_user)(zn_tcp_server *, zn_tcp_session *);
    zn_poller *(*pick_poller)(zn_tcp_server *, zn_poller *, void *);
    void *pick_poller_user;
    zn_poller_pool *poller_pool;
    int pool_pick_poller;

    float manager_interval_sec;
    uint64_t handshake_timeout_ms;
    uint64_t idle_timeout_ms;
    unsigned max_sessions;
    unsigned max_sessions_per_ip;

    zn_listen_binding *listen_bindings;
    zn_tcp_session **sessions;
    size_t session_count;
    size_t session_cap;

    zb_mutex *mutex;
    char host[ZN_NET_ADDR_STR_LEN];
    uint16_t port;
    int backlog;
    int started;

    size_t out_highwater_bytes;
    zn_tcp_session_out_highwater_cb on_out_highwater;
    zn_ssl_ctx *ssl_ctx;
    int destroying;
};

static void session_detach(zn_tcp_session *session);
static void session_remove(zn_tcp_server *srv, zn_tcp_session *session);
static void session_out_flush(zn_tcp_session *session);

/**
 * 按需注册/注销可写事件。
 * 发送遇到 EAGAIN/部分写后必须注册可写事件，否则剩余数据会一直积压
 * （非 RTMP 服务没有 manager 定时 flush 兜底，会话会永久卡住）。
 */
static void session_set_events(zn_tcp_session *session, int want_out)
{
    unsigned ev = ZN_POLL_IN | ZN_POLL_ERR | ZN_POLL_HUP;

    if (!session || !session->sock || session->want_out == want_out) {
        return;
    }
    if (want_out) {
        ev |= ZN_POLL_OUT;
    }
    if (zn_socket_mod_events(session->sock, ev) == ZN_OK) {
        session->want_out = want_out;
    }
}

#if ZN_TCP_TLS
static void session_tls_events(zn_tcp_session *session, int want_out)
{
    session_set_events(session, want_out);
}

static int session_tls_handshake(zn_tcp_session *session)
{
    int r;
    int err;

    if (!session || !session->ssl) {
        return -1;
    }
    if (session->tls_ready) {
        return 1;
    }
    ERR_clear_error();
    r = SSL_do_handshake(session->ssl);
    if (r == 1) {
        session->tls_ready = 1;
        session_tls_events(session, 0);
        session_out_flush(session);
        return 1;
    }
    err = SSL_get_error(session->ssl, r);
    if (err == SSL_ERROR_WANT_READ) {
        session->tls_want_out = 0;
        session_tls_events(session, 0);
        return 0;
    }
    if (err == SSL_ERROR_WANT_WRITE) {
        session->tls_want_out = 1;
        session_tls_events(session, 1);
        return 0;
    }
    session_remove(session->server, session);
    return -1;
}
#endif

static void session_on_error(zn_socket *sock, void *user)
{
    zn_tcp_session *session = (zn_tcp_session *)user;
    (void)sock;
    if (!session || session->closing)
        return;
    session_remove(session->server, session);
}

static void session_unref(zn_tcp_session *session);

/* 读取泵：调用方必须已持有 session 引用。on_recv 可能关闭本会话，每次回调后检查 closing。 */
static void session_read_pump(zn_socket *sock, zn_tcp_session *session)
{
    if (!session || session->closing)
        return;

#if ZN_TCP_TLS
    if (session->ssl) {
        if (!session->tls_ready && session_tls_handshake(session) != 1) {
            return;
        }
        for (;;) {
            char buf[ZN_TCP_RECV_CHUNK];
            int n = SSL_read(session->ssl, buf, (int)sizeof(buf));
            if (n > 0) {
                session->last_rx_ms = zn_monotonic_ms();
                session->last_active_ms = session->last_rx_ms;
                session->saw_rx = 1;
                if (session->server->ops.on_recv)
                    session->server->ops.on_recv(session, buf, (size_t)n, session->user);
                if (zb_atomic_load32(&session->closing))
                    return;
                continue;
            }
            if (n == 0) {
                session_remove(session->server, session);
                return;
            }
            {
                int e = SSL_get_error(session->ssl, n);
                if (e == SSL_ERROR_WANT_WRITE) {
                    session_tls_events(session, 1);
                    return;
                }
                if (e == SSL_ERROR_WANT_READ) {
                    return;
                }
            }
            session_remove(session->server, session);
            return;
        }
    }
#endif

    for (;;) {
        char buf[ZN_TCP_RECV_CHUNK];
        zn_ssize_t n = zn_socket_recv(sock, buf, sizeof(buf));
        if (n > 0) {
            session->last_rx_ms = zn_monotonic_ms();
            session->last_active_ms = session->last_rx_ms;
            session->saw_rx = 1;
            if (session->server->ops.on_recv)
                session->server->ops.on_recv(session, buf, (size_t)n, session->user);
            if (zb_atomic_load32(&session->closing))
                return;
            continue;
        }
        if (n == ZN_ERR_AGAIN)
            break;
        if (n == 0 || n < 0)
            session_remove(session->server, session);
        break;
    }
}

static void session_on_readable(zn_socket *sock, void *user)
{
    zn_tcp_session *session = (zn_tcp_session *)user;
    if (!session || session->closing)
        return;
    /* 持有引用直到读取结束，防止 on_recv 中关闭会话释放 socket 后仍被读取。 */
    zb_atomic_increment32(&session->ref);
    session_read_pump(sock, session);
    session_unref(session);
}

static void session_on_writable(zn_socket *sock, void *user)
{
    zn_tcp_session *session = (zn_tcp_session *)user;

    (void)sock;
    if (!session || session->closing) {
        return;
    }
#if ZN_TCP_TLS
    if (session->ssl && !session->tls_ready) {
        (void)session_tls_handshake(session);
        return;
    }
#endif
    session_out_flush(session);
}

static zn_err_t session_attach(zn_tcp_session *session, zn_poller *poller)
{
    zn_socket_callbacks_t cb = { session_on_readable, session_on_writable, session_on_error };
    return zn_socket_attach_poller(session->sock, poller, &cb, session);
}

static zn_tcp_session *session_create(zn_tcp_server *srv, zn_socket *client, zn_poller *poller)
{
    zn_tcp_session *session = (zn_tcp_session *)calloc(1, sizeof(*session));
    if (!session)
        return NULL;

    session->server = srv;
    session->sock = client;
    session->poller = poller;
    session->ref = 1;
    session->created_ms = zn_monotonic_ms();
    session->last_rx_ms = session->created_ms;
    session->last_active_ms = session->created_ms;
    if (srv->session_create_user)
        session->user = srv->session_create_user(srv, session);
    else
        session->user = srv->session_user;

#if ZN_TCP_TLS
    if (srv->ssl_ctx && srv->ssl_ctx->ctx) {
        session->ssl = SSL_new(srv->ssl_ctx->ctx);
        if (!session->ssl || SSL_set_fd(session->ssl, (int)zn_socket_fd(client)) != 1) {
            if (session->ssl) {
                SSL_free(session->ssl);
                session->ssl = NULL;
            }
            free(session);
            return NULL;
        }
        SSL_set_accept_state(session->ssl);
        session->tls_ready = 0;
    }
#endif

    if (session_attach(session, poller) != ZN_OK) {
        free(session);
        return NULL;
    }
#if ZN_TCP_TLS
    if (session->ssl) {
        session_tls_events(session, 1);
    }
#endif
    return session;
}

/** 记录对端 IPv4 并做连接上限检查；超限返回 0（调用方关闭会话）。 */
static int session_admit(zn_tcp_server *srv, zn_tcp_session *session)
{
    char ip[64];
    uint16_t port = 0;
    unsigned same_ip = 0;
    size_t i;
    int admit = 1;

    memset(ip, 0, sizeof(ip));
    if (zn_socket_get_peer((const zn_socket *)session->sock, ip, sizeof(ip), &port) == ZN_OK &&
        ip[0]) {
        unsigned char b[4];
        if (inet_pton(AF_INET, ip, b) == 1) {
            session->peer_ipv4 = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                                 ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        }
    }

    if (!srv->max_sessions && !srv->max_sessions_per_ip)
        return 1;

    zb_mutex_lock(srv->mutex);
    if (srv->max_sessions && srv->session_count + 1 > srv->max_sessions) {
        admit = 0;
    }
    if (admit && srv->max_sessions_per_ip && session->peer_ipv4) {
        for (i = 0; i < srv->session_count; ++i) {
            const zn_tcp_session *pe = srv->sessions[i];
            if (pe && pe->peer_ipv4 == session->peer_ipv4) {
                ++same_ip;
            }
        }
        if (same_ip + 1 > srv->max_sessions_per_ip) {
            admit = 0;
        }
    }
    zb_mutex_unlock(srv->mutex);
    return admit;
}

static zn_err_t session_list_add(zn_tcp_server *srv, zn_tcp_session *session)
{
    zb_mutex_lock(srv->mutex);
    if (srv->session_count >= srv->session_cap) {
        size_t cap = srv->session_cap ? srv->session_cap * 2 : 8;
        zn_tcp_session **p = (zn_tcp_session **)realloc(srv->sessions, cap * sizeof(*p));
        if (!p) {
            zb_mutex_unlock(srv->mutex);
            return ZN_ERR_NOMEM;
        }
        srv->sessions = p;
        srv->session_cap = cap;
    }
    srv->sessions[srv->session_count++] = session;
    zb_mutex_unlock(srv->mutex);
    return ZN_OK;
}

static void session_out_free_chunk(zn_tcp_out_chunk *chunk)
{
    if (!chunk)
        return;
    if (chunk->kind == ZN_TCP_OUT_MEM)
        free(chunk->u.mem);
    else if (chunk->kind == ZN_TCP_OUT_BUF)
        zn_buf_unref(chunk->u.buf);
    free(chunk);
}

static void session_out_pop_chunk(zn_tcp_session *session)
{
    zn_tcp_out_chunk *chunk = session->out_head;
    if (!chunk)
        return;
    session->out_head = chunk->next;
    if (!session->out_head)
        session->out_tail = NULL;
    if (session->out_pending >= chunk->len)
        session->out_pending -= chunk->len;
    else
        session->out_pending = 0;
    session_out_free_chunk(chunk);
}

static const void *session_chunk_ptr(const zn_tcp_out_chunk *chunk)
{
    if (!chunk)
        return NULL;
    if (chunk->kind == ZN_TCP_OUT_BUF)
        return (const uint8_t *)zn_buf_data(chunk->u.buf) + chunk->off;
    return chunk->u.mem + chunk->off;
}

static void session_out_check_highwater(zn_tcp_session *session)
{
    zn_tcp_server *srv;

    if (!session)
        return;
    srv = session->server;
    if (!srv || !srv->on_out_highwater || srv->out_highwater_bytes == 0)
        return;
    if (session->out_pending < srv->out_highwater_bytes) {
        session->out_highwater_hit = 0;
        return;
    }
    if (!session->out_highwater_hit) {
        session->out_highwater_hit = 1;
        srv->on_out_highwater(session, session->out_pending, session->user);
    }
}

static int session_out_append_buf(zn_tcp_session *session, zn_buf *buf);

static int session_out_enqueue(zn_tcp_session *session, zn_tcp_out_chunk *chunk)
{
    if (!session || !chunk)
        return -1;
    chunk->next = NULL;
    if (session->out_tail)
        session->out_tail->next = chunk;
    else
        session->out_head = chunk;
    session->out_tail = chunk;
    session->out_pending += chunk->len;
    session_out_check_highwater(session);
    return 0;
}

static int session_out_append_mem(zn_tcp_session *session, const void *data, size_t len)
{
    zn_buf *buf;
    int r;

    if (!session || !data || len == 0)
        return 0;
    /* 会话钉在 poller 上：出站拷贝走本地无锁池（无本地池则回退共享池/malloc） */
    buf = zn_buf_alloc_local(session->poller, len);
    if (!buf)
        return -1;
    memcpy((void *)zn_buf_data(buf), data, len);
    zn_buf_set_len(buf, len);
#ifdef ZN_TEST_HOOKS
    g_zn_test_out_mem_append += len;
#endif
    r = session_out_append_buf(session, buf);
    zn_buf_unref(buf);
    return r;
}

static int session_out_append_buf(zn_tcp_session *session, zn_buf *buf)
{
    zn_tcp_out_chunk *chunk;
    size_t len;

    if (!session || !buf)
        return -1;
    len = zn_buf_len(buf);
    if (len == 0)
        return 0;
    chunk = (zn_tcp_out_chunk *)calloc(1, sizeof(*chunk));
    if (!chunk)
        return -1;
    chunk->kind = ZN_TCP_OUT_BUF;
    chunk->off = 0;
    chunk->len = len;
    chunk->u.buf = zn_buf_ref(buf);
    return session_out_enqueue(session, chunk);
}

static void session_out_advance_sent(zn_tcp_session *session, size_t sent)
{
    size_t skip = sent;

    while (skip > 0 && session->out_head) {
        zn_tcp_out_chunk *chunk = session->out_head;

        if (chunk->len == 0) {
            session_out_pop_chunk(session);
            continue;
        }
        if (skip >= chunk->len) {
            skip -= chunk->len;
            session_out_pop_chunk(session);
            continue;
        }
        chunk->off += skip;
        chunk->len -= skip;
        if (session->out_pending >= skip)
            session->out_pending -= skip;
        else
            session->out_pending = 0;
        skip = 0;
    }
}

static zn_ssize_t session_out_try_sendv(zn_tcp_session *session, unsigned *out_count)
{
    zn_socket_iov iov[ZN_SOCKET_IOV_MAX];
    unsigned count = 0;
    size_t total = 0;
    zn_tcp_out_chunk *c;

    if (!session || !session->sock)
        return ZN_ERR_INVALID;

    for (c = session->out_head; c && count < ZN_SOCKET_IOV_MAX; c = c->next) {
        const void *ptr;

        if (c->len == 0)
            continue;
        ptr = session_chunk_ptr(c);
        if (!ptr)
            continue;
        iov[count].base = ptr;
        iov[count].len = c->len;
        total += c->len;
        count++;
    }
    if (out_count)
        *out_count = count;
    if (count == 0)
        return 0;
    if (count == 1)
        return zn_socket_send(session->sock, iov[0].base, iov[0].len);
    return zn_socket_sendv(session->sock, iov, count);
}

static void session_out_flush(zn_tcp_session *session)
{
    if (!session || !session->sock)
        return;

#if ZN_TCP_TLS
    if (session->ssl) {
        if (!session->tls_ready) {
            return; /* 握手完成后由 handshake 路径 flush */
        }
        while (session->out_head) {
            zn_tcp_out_chunk *chunk = session->out_head;
            const void *ptr = session_chunk_ptr(chunk);
            size_t chunk_len = chunk->len;
            int n;

            if (chunk_len == 0 || !ptr) {
                session_out_pop_chunk(session);
                continue;
            }
            if (chunk_len > 65536) {
                chunk_len = 65536;
            }
            ERR_clear_error();
            n = SSL_write(session->ssl, ptr, (int)chunk_len);
            if (n > 0) {
                session->last_active_ms = zn_monotonic_ms();
                session_out_advance_sent(session, (size_t)n);
                continue;
            }
            {
                int e = SSL_get_error(session->ssl, n);
                if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) {
                    session_tls_events(session, 1);
                    return;
                }
            }
            session_remove(session->server, session);
            return;
        }
        session_tls_events(session, 0);
        return;
    }
#endif

    while (session->out_head) {
        unsigned count = 0;
        size_t total = 0;
        zn_tcp_out_chunk *c;
        zn_ssize_t n;

        if (session->out_head->len == 0) {
            session_out_pop_chunk(session);
            continue;
        }

        for (c = session->out_head; c && count < ZN_SOCKET_IOV_MAX; c = c->next) {
            if (c->len == 0)
                break;
            total += c->len;
            count++;
        }
        if (count == 0)
            break;

        n = session_out_try_sendv(session, &count);
        if (n >= 0 && n > 0) {
            session->last_active_ms = zn_monotonic_ms();
        }
        if (n == (zn_ssize_t)total) {
            session_out_advance_sent(session, (size_t)n);
            continue;
        }
        if (n > 0) {
            session_out_advance_sent(session, (size_t)n);
            continue;
        }
        if (n == ZN_ERR_AGAIN) {
            /* 内核缓冲满：等待可写事件再继续，否则积压数据永不发出 */
            session_set_events(session, 1);
            return;
        }
        return;
    }
    /* 积压已清空，恢复只读关注（避免空转的可写事件） */
    session_set_events(session, 0);
}

static void session_free(zn_tcp_session *session)
{
    if (!session)
        return;
    if (session->sock && zn_socket_detach_poller(session->sock) != ZN_OK) {
        zn_error("tcp_session free: detach poller timeout, leak session");
        return;
    }
    while (session->out_head)
        session_out_pop_chunk(session);
#if ZN_TCP_TLS
    if (session->ssl) {
        SSL_free(session->ssl);
        session->ssl = NULL;
    }
#endif
    zn_socket_destroy(session->sock);
    free(session);
}

static void session_unref(zn_tcp_session *session)
{
    if (session && zb_atomic_decrement32(&session->ref) == 0)
        session_free(session);
}

static void session_remove(zn_tcp_server *srv, zn_tcp_session *target)
{
    int removed = 0;

    if (!srv || !target)
        return;

    zb_mutex_lock(srv->mutex);
    if (zb_atomic_load32(&target->closing)) {
        zb_mutex_unlock(srv->mutex);
        return;
    }
    zb_atomic_add32(&target->closing, 1);
    for (size_t i = 0; i < srv->session_count; ++i) {
        if (srv->sessions[i] == target) {
            srv->sessions[i] = srv->sessions[srv->session_count - 1];
            srv->session_count--;
            removed = 1;
            break;
        }
    }
    zb_mutex_unlock(srv->mutex);

    if (!removed)
        return;

    if (srv->ops.on_error)
        srv->ops.on_error(target, target->user);

    target->server = NULL;
    session_unref(target);
}

static void session_detach(zn_tcp_session *session)
{
    if (!session || !session->sock)
        return;
    zn_socket_detach_poller(session->sock);
}

static zn_poller *choose_poller(zn_tcp_server *srv, zn_poller *accept_poller)
{
    if (srv->pick_poller) {
        zn_poller *p = srv->pick_poller(srv, accept_poller, srv->pick_poller_user);
        if (p)
            return p;
    }
    if (srv->pool_pick_poller && srv->poller_pool) {
        zn_poller *p = zn_tcp_pick_poller_from_pool(srv->poller_pool, accept_poller);
        if (p)
            return p;
    }
    return accept_poller;
}

static void listen_on_readable(int fd, unsigned events, void *user)
{
    zn_listen_binding *bind = (zn_listen_binding *)user;
    zn_tcp_server *srv = bind->server;
    zn_poller *accept_poller = bind->poller;
    (void)events;

    if (!srv || !srv->listen_sock || zn_socket_fd(srv->listen_sock) != fd)
        return;

    for (;;) {
        zn_socket *client = NULL;
        zn_err_t err = zn_socket_accept(srv->listen_sock, &client);
        if (err == ZN_ERR_AGAIN)
            break;
        if (err != ZN_OK || !client)
            break;

        zn_poller *target = choose_poller(srv, accept_poller);
        zn_tcp_session *session = session_create(srv, client, target);
        if (!session) {
            zn_socket_destroy(client);
            continue;
        }
        if (!session_admit(srv, session)) {
            zn_warn("tcp: connection limit reached, rejecting");
            session_unref(session);
            continue;
        }
        if (session_list_add(srv, session) != ZN_OK) {
            session_unref(session);
            continue;
        }
    }
}

/** 握手/空闲超时巡检：超时即关闭（on_error 会被调用，业务据此清理）。 */
static void run_timeout_checks(zn_tcp_server *srv, zn_poller *poller)
{
    uint64_t now = zn_monotonic_ms();
    zn_tcp_session *expired[32];
    size_t n = 0;
    size_t i;

    zb_mutex_lock(srv->mutex);
    for (i = 0; i < srv->session_count && n < sizeof(expired) / sizeof(expired[0]); ++i) {
        zn_tcp_session *s = srv->sessions[i];
        int hit = 0;

        if (!s || s->closing || s->poller != poller)
            continue;
        if (srv->handshake_timeout_ms && !s->saw_rx &&
            now > s->created_ms &&
            now - s->created_ms >= srv->handshake_timeout_ms) {
            zn_warn("tcp: handshake timeout (%llums) closing session",
                     (unsigned long long)srv->handshake_timeout_ms);
            hit = 1;
        } else if (srv->idle_timeout_ms && now > s->last_active_ms &&
                   now - s->last_active_ms >= srv->idle_timeout_ms) {
            zn_warn("tcp: idle timeout (%llums) closing session",
                     (unsigned long long)srv->idle_timeout_ms);
            hit = 1;
        }
        if (hit) {
            zb_atomic_add32(&s->ref, 1); /* 巡检期间持有 */
            expired[n++] = s;
        }
    }
    zb_mutex_unlock(srv->mutex);

    /* 在锁外关闭：session_remove 自己取锁，且会回调业务。 */
    for (i = 0; i < n; ++i) {
        session_remove(srv, expired[i]);
        session_unref(expired[i]);
    }
}

static void run_manager_on_poller(zn_tcp_server *srv, zn_poller *poller)
{
    run_timeout_checks(srv, poller);

    zb_mutex_lock(srv->mutex);
    for (size_t i = 0; i < srv->session_count; ++i) {
        zn_tcp_session *s = srv->sessions[i];
        if (!s || s->closing || s->poller != poller)
            continue;
        session_out_flush(s);
        zb_mutex_unlock(srv->mutex);
        if (srv->ops.on_manager)
            srv->ops.on_manager(s, s->user);
        zb_mutex_lock(srv->mutex);
    }
    zb_mutex_unlock(srv->mutex);
}

static uint64_t manager_timer_cb(void *user)
{
    zn_listen_binding *bind = (zn_listen_binding *)user;
    zn_tcp_server *srv = bind->server;
    run_manager_on_poller(srv, bind->poller);
    if (srv->manager_interval_sec <= 0)
        return 0;
    return (uint64_t)(srv->manager_interval_sec * ZN_MS_PER_SEC);
}

static zn_err_t listen_attach_manager(zn_tcp_server *srv, zn_listen_binding *bind)
{
    if (srv->manager_interval_sec <= 0)
        return ZN_OK;
    /* 无 on_manager 但配置了超时：仍需要巡检定时器兜底。 */
    if (!srv->ops.on_manager && !srv->handshake_timeout_ms && !srv->idle_timeout_ms)
        return ZN_OK;
    bind->manager_timer = zn_poller_do_delay(bind->poller,
        (uint64_t)(srv->manager_interval_sec * ZN_MS_PER_SEC), manager_timer_cb, bind);
    if (!bind->manager_timer)
        return ZN_ERR_NOMEM;
    return ZN_OK;
}

zn_tcp_server *zn_tcp_server_create(const zn_tcp_server_opts_t *opts)
{
    if (!opts || !opts->session_ops || !opts->poller_pool)
        return NULL;

    unsigned n = zn_poller_pool_size(opts->poller_pool);
    if (n == 0)
        return NULL;

    zn_tcp_server *srv = (zn_tcp_server *)calloc(1, sizeof(*srv));
    if (!srv)
        return NULL;

    srv->mutex = zb_mutex_create(ZB_MUTEX_NORMAL);
    if (!srv->mutex) {
        free(srv);
        return NULL;
    }

    srv->poller_count = n;
    srv->pollers = (zn_poller **)calloc(n, sizeof(zn_poller *));
    if (!srv->pollers) {
        zn_tcp_server_destroy(srv);
        return NULL;
    }

    for (unsigned i = 0; i < n; ++i) {
        srv->pollers[i] = zn_poller_pool_at(opts->poller_pool, i);
        if (!srv->pollers[i]) {
            zn_tcp_server_destroy(srv);
            return NULL;
        }
    }

    srv->ops = *opts->session_ops;
    srv->session_user = opts->session_user;
    srv->session_create_user = opts->session_create_user;
    srv->handshake_timeout_ms = opts->handshake_timeout_ms;
    srv->idle_timeout_ms = opts->idle_timeout_ms;
    srv->max_sessions = opts->max_sessions;
    srv->max_sessions_per_ip = opts->max_sessions_per_ip;
    srv->poller_pool = opts->poller_pool;
    srv->pick_poller = opts->pick_poller;
    srv->pick_poller_user = opts->pick_poller_user;
    srv->ssl_ctx = opts->ssl_ctx;
    if (srv->poller_pool && !srv->pick_poller)
        srv->pool_pick_poller = 1;

    if (opts->manager_interval_sec < 0)
        srv->manager_interval_sec = -1.0f;
    else if (opts->manager_interval_sec > 0)
        srv->manager_interval_sec = opts->manager_interval_sec;
    else if (opts->session_ops->on_manager || opts->handshake_timeout_ms ||
             opts->idle_timeout_ms)
        srv->manager_interval_sec = ZN_TCP_DEFAULT_MANAGER_SEC;
    else
        srv->manager_interval_sec = -1.0f;

    srv->out_highwater_bytes = opts->out_highwater_bytes;
    srv->on_out_highwater = opts->on_out_highwater;

    if (opts->host)
        strncpy(srv->host, opts->host, sizeof(srv->host) - 1);
    else
        srv->host[0] = '\0';
    srv->port = opts->port;
    srv->backlog = opts->backlog > 0 ? opts->backlog : ZN_TCP_DEFAULT_BACKLOG;

    srv->listen_sock = zn_socket_create();
    if (!srv->listen_sock) {
        zn_tcp_server_destroy(srv);
        return NULL;
    }

    srv->listen_bindings = (zn_listen_binding *)calloc(n, sizeof(*srv->listen_bindings));
    if (!srv->listen_bindings) {
        zn_tcp_server_destroy(srv);
        return NULL;
    }
    return srv;
}

typedef struct zn_tcp_destroy_barrier {
    zn_tcp_server *srv;
    zn_poller *poller;
    zb_sem *done;
} zn_tcp_destroy_barrier;

/* 只在 poller 线程（或 poller 未运行）调用：此时不会有该 poller 的回调并发。 */
static void destroy_sessions_for_poller(zn_tcp_server *srv, zn_poller *poller)
{
    zb_mutex_lock(srv->mutex);
    for (size_t i = 0; i < srv->session_count;) {
        zn_tcp_session *session = srv->sessions[i];
        if (session->poller != poller) {
            ++i;
            continue;
        }
        srv->sessions[i] = srv->sessions[srv->session_count - 1];
        srv->session_count--;
        zb_atomic_add32(&session->closing, 1);
        zb_mutex_unlock(srv->mutex);
        session_unref(session);
        zb_mutex_lock(srv->mutex);
    }
    zb_mutex_unlock(srv->mutex);
}

static void destroy_sessions_async(void *user)
{
    zn_tcp_destroy_barrier *b = (zn_tcp_destroy_barrier *)user;
    destroy_sessions_for_poller(b->srv, b->poller);
    zb_sem_post(b->done, 1);
}

void zn_tcp_server_destroy(zn_tcp_server *srv)
{
    zn_tcp_destroy_barrier *barriers = NULL;
    int safe = 1;

    if (!srv)
        return;
    zb_mutex_lock(srv->mutex);
    srv->destroying = 1;
    zb_mutex_unlock(srv->mutex);
    zn_tcp_server_stop(srv);

    if (srv->poller_count)
        barriers = (zn_tcp_destroy_barrier *)calloc(srv->poller_count, sizeof(*barriers));

    for (unsigned i = 0; i < srv->poller_count; ++i) {
        zn_poller *p = srv->pollers[i];
        if (!p)
            continue;
        if (!zn_poller_is_running(p) || zn_poller_is_current_thread(p)) {
            destroy_sessions_for_poller(srv, p);
            continue;
        }
        if (!barriers) {
            safe = 0;
            break;
        }
        barriers[i].srv = srv;
        barriers[i].poller = p;
        barriers[i].done = zb_sem_create(0);
        if (!barriers[i].done ||
            zn_poller_async(p, destroy_sessions_async, &barriers[i], 0) != ZN_OK) {
            safe = 0;
            break;
        }
    }

    for (unsigned i = 0; i < srv->poller_count; ++i) {
        if (!barriers || !barriers[i].done)
            continue;
        if (zb_sem_timedwait(barriers[i].done, 5000) != ZB_OK) {
            zn_error("tcp_server destroy: poller %p drain timeout", (void *)barriers[i].poller);
            safe = 0;
        }
        zb_sem_destroy(barriers[i].done);
    }
    free(barriers);

    if (safe) {
        zb_mutex_lock(srv->mutex);
        if (srv->session_count != 0) {
            zb_mutex_unlock(srv->mutex);
            zn_error("tcp_server destroy: %zu sessions left, leak server",
                     srv->session_count);
            return;
        }
        zb_mutex_unlock(srv->mutex);
    } else {
        /* 无法确认 poller 已停止访问：泄漏 server，避免悬空回调。 */
        zn_error("tcp_server destroy: poller drain failed, leak server");
        return;
    }

    free(srv->sessions);
    free(srv->listen_bindings);
    free(srv->pollers);
    zn_socket_destroy(srv->listen_sock);
    zb_mutex_destroy(srv->mutex);
    free(srv);
}

zn_err_t zn_tcp_server_start(zn_tcp_server *srv)
{
    if (!srv || srv->started)
        return ZN_ERR_STATE;

    zn_err_t err = zn_socket_listen(srv->listen_sock, srv->host[0] ? srv->host : NULL, srv->port, srv->backlog);
    if (err != ZN_OK)
        return err;

    char ip[ZN_NET_ADDR_STR_LEN];
    uint16_t bound_port = 0;
    if (zn_socket_get_local(srv->listen_sock, ip, sizeof(ip), &bound_port) == ZN_OK && bound_port)
        srv->port = bound_port;

    int listen_fd = zn_socket_fd(srv->listen_sock);
    for (unsigned i = 0; i < srv->poller_count; ++i) {
        srv->listen_bindings[i].server = srv;
        srv->listen_bindings[i].poller = srv->pollers[i];
        err = zn_poller_add(srv->pollers[i], listen_fd, ZN_POLL_IN | ZN_POLL_ERR | ZN_POLL_HUP,
                              listen_on_readable, &srv->listen_bindings[i]);
        if (err != ZN_OK) {
            zn_tcp_server_stop(srv);
            return err;
        }
        err = listen_attach_manager(srv, &srv->listen_bindings[i]);
        if (err != ZN_OK) {
            zn_tcp_server_stop(srv);
            return err;
        }
    }

    srv->started = 1;
    return ZN_OK;
}

void zn_tcp_server_stop(zn_tcp_server *srv)
{
    if (!srv || !srv->started)
        return;

    int listen_fd = zn_socket_fd(srv->listen_sock);
    for (unsigned i = 0; i < srv->poller_count; ++i) {
        if (srv->listen_bindings[i].manager_timer) {
            zn_poller_timer_cancel(srv->listen_bindings[i].manager_timer);
            srv->listen_bindings[i].manager_timer = NULL;
        }
        zn_poller_del_sync(srv->pollers[i], listen_fd);
    }

    srv->started = 0;
}

int zn_tcp_server_is_running(const zn_tcp_server *srv)
{
    return srv && srv->started;
}

uint16_t zn_tcp_server_port(const zn_tcp_server *srv)
{
    return srv ? srv->port : 0;
}

unsigned zn_tcp_server_poller_count(const zn_tcp_server *srv)
{
    return srv ? srv->poller_count : 0;
}

zn_poller *zn_tcp_server_poller(const zn_tcp_server *srv, unsigned index)
{
    if (!srv || index >= srv->poller_count)
        return NULL;
    return srv->pollers[index];
}

zn_socket *zn_tcp_session_socket(zn_tcp_session *session)
{
    return session ? session->sock : NULL;
}

void *zn_tcp_session_user(zn_tcp_session *session)
{
    return session ? session->user : NULL;
}

void zn_tcp_session_set_user(zn_tcp_session *session, void *user)
{
    if (session) {
        session->user = user;
    }
}

zn_poller *zn_tcp_session_poller(zn_tcp_session *session)
{
    return session ? session->poller : NULL;
}

typedef struct tcp_send_async_task {
    zn_tcp_session *session;
    void *data;
    size_t len;
} tcp_send_async_task;

static void tcp_send_async_run(void *user)
{
    tcp_send_async_task *t = (tcp_send_async_task *)user;
    if (!zb_atomic_load32(&t->session->closing))
        zn_tcp_session_send(t->session, t->data, t->len);
    free(t->data);
    session_unref(t->session);
    free(t);
}

zn_err_t zn_tcp_session_send_async(zn_tcp_session *session, const void *data, size_t len)
{
    zn_tcp_server *srv;
    tcp_send_async_task *task;
    void *copy;

    if (!session || !data || len == 0)
        return ZN_ERR_INVALID;
    if (!session->poller || !zn_poller_is_running(session->poller) ||
        zn_poller_is_current_thread(session->poller))
        return zn_tcp_session_send(session, data, len);

    srv = session->server;
    if (!srv)
        return ZN_ERR_STATE;

    zb_mutex_lock(srv->mutex);
    if (srv->destroying || zb_atomic_load32(&session->closing)) {
        zb_mutex_unlock(srv->mutex);
        return ZN_ERR_STATE;
    }
    zb_atomic_increment32(&session->ref);
    zb_mutex_unlock(srv->mutex);

    task = (tcp_send_async_task *)calloc(1, sizeof(*task));
    copy = malloc(len);
    if (!task || !copy) {
        free(task);
        free(copy);
        session_unref(session);
        return ZN_ERR_NOMEM;
    }
    memcpy(copy, data, len);
    task->session = session;
    task->data = copy;
    task->len = len;

    if (!zn_poller_is_running(session->poller) ||
        zn_poller_async(session->poller, tcp_send_async_run, task, 0) != ZN_OK) {
        free(copy);
        free(task);
        session_unref(session);
        return ZN_ERR_STATE;
    }
    return ZN_OK;
}

zn_err_t zn_tcp_session_send(zn_tcp_session *session, const void *data, size_t len)
{
    if (!session || session->closing || !data || len == 0)
        return ZN_ERR_INVALID;
    zn_poller_assert_owner(session->poller, "tcp_session_send");

#if ZN_TCP_TLS
    if (session->ssl) {
        if (session_out_append_mem(session, data, len) != 0)
            return ZN_ERR_NOMEM;
        session_out_flush(session);
        return ZN_OK;
    }
#endif

    if (session->out_head) {
        if (session_out_append_mem(session, data, len) != 0)
            return ZN_ERR_NOMEM;
        session_out_flush(session);
        return ZN_OK;
    }

    {
        zn_ssize_t n = zn_socket_send(session->sock, data, len);
        if (n == (zn_ssize_t)len) {
            session->last_active_ms = zn_monotonic_ms();
            return ZN_OK;
        }
        if (n > 0) {
            if (session_out_append_mem(session, (const uint8_t *)data + n, len - (size_t)n) != 0)
                return ZN_ERR_NOMEM;
            session_out_flush(session);
            return ZN_OK;
        }
        if (n == ZN_ERR_AGAIN) {
            if (session_out_append_mem(session, data, len) != 0)
                return ZN_ERR_NOMEM;
            session_out_flush(session);
            return ZN_OK;
        }
        return (zn_err_t)n;
    }
}

zn_err_t zn_tcp_session_sendv(zn_tcp_session *session,
                                const void *parts[], const size_t lens[], unsigned count)
{
    zn_socket_iov iov[ZN_SOCKET_IOV_MAX];
    unsigned i;
    size_t total = 0;

    if (!session || session->closing || !parts || !lens || count == 0)
        return ZN_ERR_INVALID;
    zn_poller_assert_owner(session->poller, "tcp_session_sendv");
    if (count > ZN_SOCKET_IOV_MAX)
        count = ZN_SOCKET_IOV_MAX;

#if ZN_TCP_TLS
    if (session->ssl) {
        unsigned k;
        for (k = 0; k < count; ++k) {
            if (session_out_append_mem(session, parts[k], lens[k]) != 0)
                return ZN_ERR_NOMEM;
        }
        session_out_flush(session);
        return ZN_OK;
    }
#endif

    for (i = 0; i < count; ++i) {
        if (!parts[i] || lens[i] == 0)
            return ZN_ERR_INVALID;
        iov[i].base = parts[i];
        iov[i].len = lens[i];
        total += lens[i];
    }

    if (session->out_head) {
        for (i = 0; i < count; ++i) {
            if (session_out_append_mem(session, parts[i], lens[i]) != 0)
                return ZN_ERR_NOMEM;
        }
        session_out_flush(session);
        return ZN_OK;
    }

    {
        zn_ssize_t n = zn_socket_sendv(session->sock, iov, count);
        if (n == (zn_ssize_t)total) {
            session->last_active_ms = zn_monotonic_ms();
            return ZN_OK;
        }
        if (n > 0) {
            size_t skip = (size_t)n;
            for (i = 0; i < count; ++i) {
                if (skip >= lens[i]) {
                    skip -= lens[i];
                    continue;
                }
                if (session_out_append_mem(session, (const uint8_t *)parts[i] + skip, lens[i] - skip) != 0)
                    return ZN_ERR_NOMEM;
                skip = 0;
                for (++i; i < count; ++i) {
                    if (session_out_append_mem(session, parts[i], lens[i]) != 0)
                        return ZN_ERR_NOMEM;
                }
                break;
            }
            session_out_flush(session);
            return ZN_OK;
        }
        if (n == ZN_ERR_AGAIN) {
            for (i = 0; i < count; ++i) {
                if (session_out_append_mem(session, parts[i], lens[i]) != 0)
                    return ZN_ERR_NOMEM;
            }
            session_out_flush(session);
            return ZN_OK;
        }
        return (zn_err_t)n;
    }
}

zn_err_t zn_tcp_session_send_buf(zn_tcp_session *session, zn_buf *buf)
{
    size_t len;
    const void *data;

    if (!session || session->closing || !buf)
        return ZN_ERR_INVALID;
    zn_poller_assert_owner(session->poller, "tcp_session_send_buf");
    len = zn_buf_len(buf);
    if (len == 0)
        return ZN_OK;
    data = zn_buf_data(buf);
    if (!data)
        return ZN_ERR_INVALID;

#if ZN_TCP_TLS
    if (session->ssl) {
        if (session_out_append_buf(session, buf) != 0)
            return ZN_ERR_NOMEM;
        session_out_flush(session);
        return ZN_OK;
    }
#endif

    if (session->out_head) {
        if (session_out_append_buf(session, buf) != 0)
            return ZN_ERR_NOMEM;
        session_out_flush(session);
        return ZN_OK;
    }

    {
        zn_ssize_t n = zn_socket_send(session->sock, data, len);
        if (n == (zn_ssize_t)len) {
            session->last_active_ms = zn_monotonic_ms();
            return ZN_OK;
        }
        if (n > 0) {
            zn_tcp_out_chunk *chunk;
            chunk = (zn_tcp_out_chunk *)calloc(1, sizeof(*chunk));
            if (!chunk)
                return ZN_ERR_NOMEM;
            chunk->kind = ZN_TCP_OUT_BUF;
            chunk->off = (size_t)n;
            chunk->len = len - (size_t)n;
            chunk->u.buf = zn_buf_ref(buf);
            if (session_out_enqueue(session, chunk) != 0) {
                zn_buf_unref(chunk->u.buf);
                free(chunk);
                return ZN_ERR_NOMEM;
            }
            session_out_flush(session);
            return ZN_OK;
        }
        if (n == ZN_ERR_AGAIN) {
            if (session_out_append_buf(session, buf) != 0)
                return ZN_ERR_NOMEM;
            session_out_flush(session);
            return ZN_OK;
        }
        return (zn_err_t)n;
    }
}

void zn_tcp_session_flush(zn_tcp_session *session)
{
    if (!session)
        return;
    zn_poller_assert_owner(session->poller, "tcp_session_flush");
    session_out_flush(session);
}

size_t zn_tcp_session_out_pending(const zn_tcp_session *session)
{
    return session ? session->out_pending : 0;
}

void zn_tcp_session_out_discard(zn_tcp_session *session)
{
    if (!session)
        return;
    while (session->out_head)
        session_out_pop_chunk(session);
    session->out_highwater_hit = 0;
}

void zn_tcp_session_close(zn_tcp_session *session)
{
    if (!session || !session->server)
        return;
    zn_poller_assert_owner(session->poller, "tcp_session_close");
    session_remove(session->server, session);
}

#ifdef ZN_TEST_HOOKS
void zn_tcp_test_reset_out_mem_append(void)
{
    g_zn_test_out_mem_append = 0;
}

size_t zn_tcp_test_out_mem_append_bytes(void)
{
    return g_zn_test_out_mem_append;
}
#endif
