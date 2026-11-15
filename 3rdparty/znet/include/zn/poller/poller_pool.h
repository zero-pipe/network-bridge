#ifndef ZN_POLLER_POOL_H
#define ZN_POLLER_POOL_H

#include "../zn_export.h"
#include "../zn_errno.h"
#include "zb/os/thread.h"
#include "../util/buf.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zn_poller;
typedef struct zn_poller zn_poller;
typedef struct zn_poller_pool zn_poller_pool;

typedef struct zn_poller_pool_opts {
    /** 0 表示 zb_thread_hardware_concurrency()，至少为 1 */
    unsigned size;
    int prefer_current_thread;
    /** 工作线程调度优先级，默认 ZB_THREAD_PRIO_NORMAL */
    zb_thread_priority_t thread_priority;
} zn_poller_pool_opts_t;

ZN_API zn_poller_pool *zn_poller_pool_create(const zn_poller_pool_opts_t *opts);
ZN_API void zn_poller_pool_destroy(zn_poller_pool *pool);

/** 为每个 poller 启动 zn_poller_run 线程 */
ZN_API zn_err_t zn_poller_pool_start(zn_poller_pool *pool);
ZN_API void zn_poller_pool_stop(zn_poller_pool *pool);

ZN_API unsigned zn_poller_pool_size(const zn_poller_pool *pool);
ZN_API zn_poller *zn_poller_pool_at(const zn_poller_pool *pool, unsigned index);
ZN_API zn_poller *zn_poller_pool_get_first(const zn_poller_pool *pool);

/**
 * 按负载选择 poller（对齐 EventPollerPool::getPoller）。
 * @param prefer_current 非 0 且当前线程属于池中某 poller 时直接返回该 poller
 */
ZN_API zn_poller *zn_poller_pool_get(zn_poller_pool *pool, int prefer_current);

/** 按 conn_key 稳定 hash 到固定 poller（EventLoop Pinning） */
ZN_API zn_poller *zn_poller_pool_get_by_key(zn_poller_pool *pool, uint64_t key);

ZN_API void zn_poller_pool_set_prefer_current_thread(zn_poller_pool *pool, int on);

/** 各 poller 负载 0–100；loads 长度至少 size，返回写入数量 */
ZN_API unsigned zn_poller_pool_get_loads(const zn_poller_pool *pool, int *loads, unsigned loads_cap);

/**
 * TcpServer pick_poller 默认实现：从 pool 选最轻载 poller（ignore accept_poller）。
 * user 须为 zn_poller_pool*，prefer_current 由 pool 配置决定。
 */
ZN_API zn_poller *zn_tcp_pick_poller_from_pool(void *pool, zn_poller *accept_poller);
ZN_API zn_poller *zn_tcp_pick_poller_by_key(void *pool, uint64_t key);

/** 为每个 poller 创建并挂载独立 zn_buf_pool（per_poller / hybrid；建议 opts.thread_safe=0） */
ZN_API zn_err_t zn_poller_pool_attach_buf_pools(zn_poller_pool *pool, const zn_buf_pool_opts *opts);
ZN_API void zn_poller_pool_detach_buf_pools(zn_poller_pool *pool);

#ifdef __cplusplus
}
#endif

#endif /* ZN_POLLER_POOL_H */
