#ifndef ZN_UTIL_MPSC_H
#define ZN_UTIL_MPSC_H

#include "../zn_export.h"
#include "../zn_errno.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zn_mpsc_queue zn_mpsc_queue;

/** cap_pow2 须为 2 的幂且 ≥ 2 */
ZN_API zn_mpsc_queue *zn_mpsc_create(size_t cap_pow2);
ZN_API void zn_mpsc_destroy(zn_mpsc_queue *q);

/** 多生产者 push；队列满返回 ZN_ERR_AGAIN */
ZN_API zn_err_t zn_mpsc_push(zn_mpsc_queue *q, void *item);
/** 单消费者 pop；空返回 NULL */
ZN_API void *zn_mpsc_pop(zn_mpsc_queue *q);

#ifdef __cplusplus
}
#endif

#endif /* ZN_UTIL_MPSC_H */
