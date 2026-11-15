/**
 * @file sem.h
 * @brief 跨平台信号量。
 */
#ifndef ZB_OS_SEM_H
#define ZB_OS_SEM_H

#include "zb/zb_export.h"
#include "zb/zb_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zb_sem zb_sem;

ZB_API zb_sem *zb_sem_create(unsigned int initial);
ZB_API void zb_sem_destroy(zb_sem *s);
ZB_API zb_err_t zb_sem_wait(zb_sem *s);
ZB_API zb_err_t zb_sem_timedwait(zb_sem *s, unsigned int timeout_ms);
ZB_API zb_err_t zb_sem_post(zb_sem *s, unsigned int count);

#ifdef __cplusplus
}
#endif

#endif /* ZB_OS_SEM_H */
