/**
 * @file thread.h
 * @brief 跨平台线程。
 */
#ifndef ZB_OS_THREAD_H
#define ZB_OS_THREAD_H

#include "zb/zb_export.h"
#include "zb/zb_errno.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zb_thread zb_thread;
typedef void (*zb_thread_fn)(void *user);

typedef enum zb_thread_priority {
    ZB_THREAD_PRIO_LOWEST = 0,
    ZB_THREAD_PRIO_LOW = 1,
    ZB_THREAD_PRIO_NORMAL = 2,
    ZB_THREAD_PRIO_HIGH = 3,
    ZB_THREAD_PRIO_HIGHEST = 4
} zb_thread_priority_t;

ZB_API zb_thread *zb_thread_create(zb_thread_fn fn, void *user);
ZB_API zb_thread *zb_thread_create_ex(zb_thread_fn fn, void *user, zb_thread_priority_t priority);
ZB_API zb_err_t zb_thread_join(zb_thread *t);
ZB_API void zb_thread_destroy(zb_thread *t);

ZB_API void zb_thread_set_current_priority(zb_thread_priority_t priority);
ZB_API unsigned int zb_thread_hardware_concurrency(void);

#ifdef __cplusplus
}
#endif

#endif /* ZB_OS_THREAD_H */
