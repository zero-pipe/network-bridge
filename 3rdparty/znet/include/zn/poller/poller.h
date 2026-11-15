#ifndef ZN_POLLER_H
#define ZN_POLLER_H

#include "../zn_export.h"
#include "../zn_errno.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zn_poller zn_poller;

enum {
    ZN_POLL_IN = 1 << 0,
    ZN_POLL_OUT = 1 << 1,
    ZN_POLL_ERR = 1 << 2,
    ZN_POLL_HUP = 1 << 3
};

/** poller 负载统计滑动窗口 (ms) */
#define ZN_POLLER_LOAD_WINDOW_MS  2000
/**
 * zn_poller_run 默认最大 sleep (ms)。
 * 流媒体 10ms 级 timer 需要 ≤10；见 docs/10-timer-scheduler-contract.md。
 */
#define ZN_POLLER_PENDING_POLL_MS 10

typedef void (*zn_poller_cb)(int fd, unsigned events, void *user);

/** 在 poller 线程内执行的任务（勿阻塞） */
typedef void (*zn_poller_task_fn)(void *user);

ZN_API zn_poller *zn_poller_create(void);
ZN_API void zn_poller_destroy(zn_poller *p);

ZN_API zn_err_t zn_poller_add(zn_poller *p, int fd, unsigned events, zn_poller_cb cb, void *user);
ZN_API zn_err_t zn_poller_mod(zn_poller *p, int fd, unsigned events);
ZN_API zn_err_t zn_poller_del(zn_poller *p, int fd);
/**
 * 跨线程安全删除 fd：poller 线程在运行时投递到该线程执行并等待完成；
 * 未运行或当前就是 poller 线程时直接删除。
 */
ZN_API zn_err_t zn_poller_del_sync(zn_poller *p, int fd);

/**
 * @param timeout_ms -1 表示无限等待，0 表示立即返回
 * @return 就绪事件数，0 表示超时，<0 错误
 */
ZN_API int zn_poller_poll(zn_poller *p, int timeout_ms);

ZN_API int zn_poller_wake_read_fd(zn_poller *p);
ZN_API int zn_poller_wake_write_fd(zn_poller *p);
ZN_API zn_err_t zn_poller_wake(zn_poller *p);

/**
 * 投递任务到 poller 线程（pipe 唤醒 + 队列）。
 * @param may_sync 非 0 且当前已是 poller 线程则立即执行
 * @note user 须在任务执行完成前保持有效（勿传栈上局部变量地址）
 */
ZN_API zn_err_t zn_poller_async(zn_poller *p, zn_poller_task_fn fn, void *user, int may_sync);
ZN_API zn_err_t zn_poller_async_first(zn_poller *p, zn_poller_task_fn fn, void *user, int may_sync);

/** 不 poll 时手动排空任务队列 */
ZN_API void zn_poller_process_pending(zn_poller *p);

/** 标记当前线程为该 poller 的事件线程（在 run 前也可手动调用） */
ZN_API void zn_poller_bind_thread(zn_poller *p);
ZN_API void zn_poller_unbind_thread(zn_poller *p);
ZN_API int zn_poller_is_current_thread(zn_poller *p);
/** poller 线程是否已绑定（run 中） */
ZN_API int zn_poller_is_running(const zn_poller *p);
/** 调试用：ZN_DEBUG_AFFINITY 开启时，非 owner 线程调用会 abort */
ZN_API void zn_poller_assert_owner(const zn_poller *p, const char *what);

/** 阻塞循环 poll，*stop_flag 非 0 时退出 */
ZN_API void zn_poller_run(zn_poller *p, volatile int *stop_flag);

/** 线程负载 0–100（仅在 poll/run 路径统计），越高越忙 */
ZN_API int zn_poller_get_load(zn_poller *p);

#ifdef __cplusplus
}
#endif

#endif /* ZN_POLLER_H */
