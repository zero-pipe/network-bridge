#ifndef _platform_locker_h_
#define _platform_locker_h_

/*
 * zice 兼容锁接口：内部由 zbase 的 zb_mutex 实现。
 * 新代码请直接使用 zb/os/sync.h。
 */
#include "zb/os/sync.h"
#include <stddef.h>

typedef struct zb_locker {
    zb_mutex *m;
} zb_locker_t;

static inline int zb_locker_create(zb_locker_t *locker)
{
    if (!locker)
        return -1;
    locker->m = zb_mutex_create(ZB_MUTEX_RECURSIVE);
    return locker->m ? 0 : -1;
}

static inline int zb_locker_destroy(zb_locker_t *locker)
{
    if (!locker)
        return -1;
    if (locker->m)
        zb_mutex_destroy(locker->m);
    locker->m = NULL;
    return 0;
}

static inline int zb_locker_lock(zb_locker_t *locker)
{
    return (locker && zb_mutex_lock(locker->m) == ZB_OK) ? 0 : -1;
}

static inline int zb_locker_unlock(zb_locker_t *locker)
{
    return (locker && zb_mutex_unlock(locker->m) == ZB_OK) ? 0 : -1;
}

static inline int zb_locker_trylock(zb_locker_t *locker)
{
    if (!locker)
        return -1;
    return zb_mutex_trylock(locker->m) == ZB_OK ? 0 : -1;
}

#endif /* !_platform_locker_h_ */
