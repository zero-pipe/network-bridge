#ifndef NB_POOLSET_H
#define NB_POOLSET_H

/*
 * nb_poolset: size-classed nb_buf pools.
 *
 * Used on the RX hot path: each IO takes a buffer with capacity >= size from
 * the smallest class that fits, falling back to a larger class when the
 * preferred one is exhausted instead of degrading to malloc.
 *
 * The fixed-size nb_buf_pool lives in buf.h alongside nb_buf (the two are
 * tightly coupled).
 */

#include <stddef.h>
#include <stdint.h>

#include <nb/base/buf.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nb_poolset nb_poolset_t;

/* slot_counts[i] caps the slot_sizes[i] class; slot_sizes must be ascending. */
nb_poolset_t *nb_poolset_create(const size_t *slot_sizes,
                                const size_t *slot_counts,
                                size_t class_count);

/*
 * As nb_poolset_create, but when `local` is non-zero each class pool is created
 * with NB_BUF_POOL_FLAG_LOCAL: lock-free, owned by the creating thread. Used for
 * per-thread hot-path pools; the poolset must be destroyed on the owning thread.
 */
nb_poolset_t *nb_poolset_create_ex(const size_t *slot_sizes,
                                   const size_t *slot_counts,
                                   size_t class_count,
                                   int local);

/* Take a buffer with capacity >= size; returns NULL when no class can serve it. */
nb_buf_t *nb_poolset_acquire(nb_poolset_t *poolset, size_t size);

/* Aggregate hits/misses/outstanding/created across classes for diagnostics. */
void nb_poolset_get_stats(const nb_poolset_t *poolset, nb_buf_pool_stats_t *out);

/* Release the poolset and all class pools. */
void nb_poolset_destroy(nb_poolset_t *poolset);

#ifdef __cplusplus
}
#endif

#endif /* NB_POOLSET_H */
