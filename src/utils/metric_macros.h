#ifndef NB_METRIC_MACROS_H
#define NB_METRIC_MACROS_H

/*
 * rpc-side atomic/metrics entry points.
 *
 * Generic atomics (u32/u64, acquire/release, CAS, spinlock, refcount) live in
 * nb_base's atomic.h; nb_base and rpc share the same implementation to avoid
 * duplicate definitions. This header keeps only the rpc-specific metrics
 * switches and statistics macros.
 */
#include "atomic.h"

/*
 * Metrics switch: can be turned off at runtime (node metrics.disable) or
 * compiled out entirely at build time via RPC_METRICS_ENABLED=0. When off, the
 * hot path only pays one global flag check.
 */
#ifndef RPC_METRICS_ENABLED
#define RPC_METRICS_ENABLED 1
#endif

extern volatile int nb_metrics_enabled;
extern volatile int nb_latency_enabled;
extern volatile uint32_t nb_metrics_sample_every;

#if RPC_METRICS_ENABLED
/* nb_metrics_t is all uint64_t counters; these macros keep call sites terse. */
#define NB_METRIC_INC(field)                             \
    do {                                                  \
        if (nb_metrics_enabled) {                      \
            nb_atomic_add_u64((uint64_t *)&(field), 1u); \
        }                                                 \
    } while (0)
#define NB_METRIC_ADD(field, v)                                     \
    do {                                                             \
        if (nb_metrics_enabled) {                                 \
            nb_atomic_add_u64((uint64_t *)&(field), (uint64_t)(v)); \
        }                                                            \
    } while (0)
#define NB_METRIC_SUB(field, v)                                     \
    do {                                                             \
        if (nb_metrics_enabled) {                                 \
            nb_atomic_sub_u64((uint64_t *)&(field), (uint64_t)(v)); \
        }                                                            \
    } while (0)
#else
#define NB_METRIC_INC(field) ((void)0)
#define NB_METRIC_ADD(field, v) ((void)0)
#define NB_METRIC_SUB(field, v) ((void)0)
#endif

#endif /* NB_METRIC_MACROS_H */
