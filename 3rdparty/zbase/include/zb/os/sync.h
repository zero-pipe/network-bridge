/**
 * @file sync.h
 * @brief 跨平台互斥量。
 */
#ifndef ZB_OS_SYNC_H
#define ZB_OS_SYNC_H

#include "zb/zb_export.h"
#include "zb/zb_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zb_mutex zb_mutex;

/*
 * 语义与 POSIX 对齐：
 *   - ZB_MUTEX_NORMAL：不可重入；trylock 由持有线程再次调用返回 ZB_ERR_AGAIN。
 *   - ZB_MUTEX_RECURSIVE：可重入；同一线程可多次 lock/trylock，需相应次数 unlock。
 * 注意：Windows 后端基于 CRITICAL_SECTION（天然可重入），NORMAL 互斥量的
 * trylock 语义由 owner/depth 跟踪补齐；但 NORMAL 互斥量"重入 lock"在 Windows
 * 不会报错（POSIX 会死锁），调用方不得依赖该行为。
 */
enum {
    ZB_MUTEX_NORMAL = 0,
    ZB_MUTEX_RECURSIVE = 1
};

ZB_API zb_mutex *zb_mutex_create(int flags);
ZB_API void zb_mutex_destroy(zb_mutex *m);
ZB_API zb_err_t zb_mutex_lock(zb_mutex *m);
ZB_API zb_err_t zb_mutex_unlock(zb_mutex *m);
ZB_API zb_err_t zb_mutex_trylock(zb_mutex *m);

#ifdef __cplusplus
}
#endif

#endif /* ZB_OS_SYNC_H */
