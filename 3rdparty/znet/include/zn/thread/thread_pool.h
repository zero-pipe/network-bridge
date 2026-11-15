#ifndef ZN_THREAD_THREAD_POOL_H
#define ZN_THREAD_THREAD_POOL_H

#include "../zn_export.h"
#include "../zn_errno.h"
#include "zb/os/thread.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zn_thread_pool zn_thread_pool;
typedef void (*zn_thread_pool_fn)(void *user);

typedef struct zn_thread_pool_opts {
    /** 0 表示 zb_thread_hardware_concurrency()，至少为 1 */
    unsigned thread_count;
    zb_thread_priority_t priority;
    /** 非 0 时 create 后立即 start */
    int auto_start;
} zn_thread_pool_opts_t;

ZN_API zn_thread_pool *zn_thread_pool_create(const zn_thread_pool_opts_t *opts);
ZN_API void zn_thread_pool_destroy(zn_thread_pool *pool);

ZN_API zn_err_t zn_thread_pool_start(zn_thread_pool *pool);
ZN_API void zn_thread_pool_fini(zn_thread_pool *pool);

/**
 * 投递任务；may_sync 非 0 且当前线程属于池内 worker 时同步执行。
 * @note user 须在 fn 执行完成前保持有效
 */
ZN_API zn_err_t zn_thread_pool_async(zn_thread_pool *pool, zn_thread_pool_fn fn, void *user, int may_sync);
ZN_API zn_err_t zn_thread_pool_async_first(zn_thread_pool *pool, zn_thread_pool_fn fn, void *user,
    int may_sync);

ZN_API unsigned zn_thread_pool_worker_count(const zn_thread_pool *pool);

#ifdef __cplusplus
}
#endif

#endif /* ZN_THREAD_THREAD_POOL_H */
