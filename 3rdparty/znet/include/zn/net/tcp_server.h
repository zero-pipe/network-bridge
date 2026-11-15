#ifndef ZN_NET_TCP_SERVER_H
#define ZN_NET_TCP_SERVER_H

#include "../zn_export.h"
#include "../zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zn_poller;
struct zn_poller_pool;
struct zn_socket;
typedef struct zn_poller zn_poller;
typedef struct zn_poller_pool zn_poller_pool;
typedef struct zn_socket zn_socket;
struct zn_ssl_ctx;
typedef struct zn_ssl_ctx zn_ssl_ctx;
typedef struct zn_tcp_server zn_tcp_server;
typedef struct zn_tcp_session zn_tcp_session;

typedef void (*zn_tcp_session_out_highwater_cb)(zn_tcp_session *session, size_t pending, void *user);

typedef struct zn_tcp_session_ops {
    void (*on_recv)(zn_tcp_session *session, const void *data, size_t len, void *user);
    void (*on_error)(zn_tcp_session *session, void *user);
    /** 周期性回调（在所属 poller 线程）；可选 */
    void (*on_manager)(zn_tcp_session *session, void *user);
} zn_tcp_session_ops_t;

typedef struct zn_tcp_server_opts {
    const char *host;
    uint16_t port;
    int backlog;

    /**
     * I/O 线程由 pool 提供（须已 create；start 前调用 zn_poller_pool_start）。
     * TcpServer 不创建 poller 线程。
     */
    zn_poller_pool *poller_pool;

    const zn_tcp_session_ops_t *session_ops;
    void *session_user;
    void *(*session_create_user)(zn_tcp_server *srv, zn_tcp_session *session);

    zn_poller *(*pick_poller)(zn_tcp_server *srv, zn_poller *accept_poller, void *user);
    void *pick_poller_user;

    /** 会话巡检间隔（秒）；0 且配置了 on_manager 时默认 2s，<0 表示关闭 */
    float manager_interval_sec;

    /**
     * 首包超时（毫秒）：会话建立后多久内必须收到首个字节，否则关闭。
     * 防止半开连接/慢速攻击永久占用 fd。0=关闭。
     * 依赖 manager 巡检；未配置 on_manager 时也会为超时启用巡检。
     */
    uint64_t handshake_timeout_ms;
    /**
     * 空闲超时（毫秒）：距上次收到字节超过该时长即关闭。0=关闭。
     * 依赖 manager 巡检（同上）。
     */
    uint64_t idle_timeout_ms;

    /** 单服务最大会话数；0=不限。超出时新连接被立即关闭。 */
    unsigned max_sessions;
    /** 单 IP 最大会话数；0=不限。超出时新连接被立即关闭。 */
    unsigned max_sessions_per_ip;

    /** 出站队列字节高水位；0 表示不回调 */
    size_t out_highwater_bytes;
    zn_tcp_session_out_highwater_cb on_out_highwater;

    /** 非 NULL 时该监听为 TLS 服务端（TLS_server_method 上下文） */
    zn_ssl_ctx *ssl_ctx;
} zn_tcp_server_opts_t;

ZN_API zn_tcp_server *zn_tcp_server_create(const zn_tcp_server_opts_t *opts);
ZN_API void zn_tcp_server_destroy(zn_tcp_server *srv);

/** 绑定 listen 并注册到 pool 内各 poller；若配置了 on_manager 则挂载巡检 timer */
ZN_API zn_err_t zn_tcp_server_start(zn_tcp_server *srv);

ZN_API void zn_tcp_server_stop(zn_tcp_server *srv);

ZN_API int zn_tcp_server_is_running(const zn_tcp_server *srv);

ZN_API uint16_t zn_tcp_server_port(const zn_tcp_server *srv);
ZN_API unsigned zn_tcp_server_poller_count(const zn_tcp_server *srv);
ZN_API zn_poller *zn_tcp_server_poller(const zn_tcp_server *srv, unsigned index);

ZN_API zn_socket *zn_tcp_session_socket(zn_tcp_session *session);
ZN_API void *zn_tcp_session_user(zn_tcp_session *session);
/** 更新会话 user；异步任务在 on_error 后据此判断会话是否仍有效（置 NULL） */
ZN_API void zn_tcp_session_set_user(zn_tcp_session *session, void *user);
ZN_API zn_poller *zn_tcp_session_poller(zn_tcp_session *session);
struct zn_buf;
typedef struct zn_buf zn_buf;

/** 拷贝发送；阻塞/半写时入队缓冲走 session poller 的 alloc_local */
ZN_API zn_err_t zn_tcp_session_send(zn_tcp_session *session, const void *data, size_t len);
/** 跨线程安全发送：拷贝后投递到 session poller；回调线程内直接发送。 */
ZN_API zn_err_t zn_tcp_session_send_async(zn_tcp_session *session, const void *data, size_t len);
ZN_API zn_err_t zn_tcp_session_sendv(zn_tcp_session *session,
                                           const void *parts[], const size_t lens[], unsigned count);
/** 零拷贝发送；内部 zn_buf_ref，写完成后 zn_buf_unref（调用方宜用 alloc_local） */
ZN_API zn_err_t zn_tcp_session_send_buf(zn_tcp_session *session, zn_buf *buf);
ZN_API void zn_tcp_session_flush(zn_tcp_session *session);
ZN_API size_t zn_tcp_session_out_pending(const zn_tcp_session *session);
/** 丢弃尚未发出的应用层发送缓冲（VOD seek 时避免旧媒体排在 Notify 前） */
ZN_API void zn_tcp_session_out_discard(zn_tcp_session *session);
ZN_API void zn_tcp_session_close(zn_tcp_session *session);

#ifdef ZN_TEST_HOOKS
ZN_API void zn_tcp_test_reset_out_mem_append(void);
ZN_API size_t zn_tcp_test_out_mem_append_bytes(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ZN_NET_TCP_SERVER_H */
