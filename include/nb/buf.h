#ifndef NB_BUF_H
#define NB_BUF_H

/*
 * Reference-counted contiguous message buffers with explicit ownership.
 *
 * unique mode: the default path, no atomics. Transfer ownership with
 * nb_buf_move() and return memory with nb_buf_release().
 *
 * shared mode: promote with nb_buf_share() only when a buffer really is
 * broadcast to several owners, then hand it out with nb_buf_ref() /
 * nb_buf_ref_n(). Every holder eventually calls nb_buf_release().
 *
 * This module does not depend on the rest of the framework or ntk; it is usable on its own.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <nb/alloc.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nb_buf nb_buf_t;
typedef struct nb_buf_pool nb_buf_pool_t;
typedef void (*nb_buf_free_fn)(void *opaque, uint8_t *data);

#define NB_BUF_FLAG_SHARED   0x0001u
#define NB_BUF_FLAG_EXTERNAL 0x0002u
#define NB_BUF_FLAG_READONLY 0x0004u
#define NB_BUF_FLAG_POOLED   0x0008u

/* Pool flag: create slots lazily; slot_count > 0 acts as a hard cap. */
#define NB_BUF_POOL_FLAG_LAZY 0x0001u

typedef struct nb_buf_view {
    nb_buf_t *owner; /* borrowed; does not add a reference */
    const uint8_t *data;
    size_t size;
} nb_buf_view_t;

typedef struct nb_buf_pool_config {
    size_t slot_size;  /* bytes per buffer slot */
    size_t slot_count; /* 0 = lazy allocation; > 0 = preallocate and cap */
    size_t max_cached; /* free slots kept for reuse; 0 = slot_count or the default of 64 */
    uint32_t flags;    /* NB_BUF_POOL_FLAG_* */
} nb_buf_pool_config_t;

typedef struct nb_buf_pool_stats {
    uint64_t hits;        /* acquires served from cached slots */
    uint64_t misses;      /* acquires that had to create a slot */
    uint64_t failed;      /* acquires that could not obtain a slot */
    uint64_t outstanding; /* slots currently checked out */
    uint64_t cached;      /* free slots currently kept */
    uint64_t created;     /* total slots ever created */
} nb_buf_pool_stats_t;

nb_buf_t *nb_buf_alloc(size_t size);
nb_buf_t *nb_buf_alloc_aligned(size_t size, size_t alignment);
nb_buf_t *nb_buf_create_external(void *data,
                                 size_t size,
                                 nb_buf_free_fn free_fn,
                                 void *opaque,
                                 uint32_t flags);

/* Transfer unique ownership; sets *buf to NULL on success. */
nb_buf_t *nb_buf_move(nb_buf_t **buf);
/* Reclaim a unique buffer, or drop one reference of a shared buffer. */
void nb_buf_release(nb_buf_t *buf);

/* Promote a unique buffer to shared; a second call returns NB_ERR_ALREADY. */
int nb_buf_share(nb_buf_t *buf);
/* Shared buffers only; returns NULL for unique buffers. */
nb_buf_t *nb_buf_ref(nb_buf_t *buf);
/* Add n references at once and return buf; shared buffers only. */
nb_buf_t *nb_buf_ref_n(nb_buf_t *buf, size_t n);

uint8_t *nb_buf_data(nb_buf_t *buf);
const uint8_t *nb_buf_const_data(const nb_buf_t *buf);
size_t nb_buf_size(const nb_buf_t *buf);
size_t nb_buf_capacity(const nb_buf_t *buf);
uint32_t nb_buf_flags(const nb_buf_t *buf);
bool nb_buf_is_shared(const nb_buf_t *buf);
/* Set the logical size (<= capacity); returns NB_ERR_INVALID when it exceeds the
 * capacity. */
int nb_buf_set_size(nb_buf_t *buf, size_t size);

/* Borrow a slice; owner must outlive the view. */
int nb_buf_slice(nb_buf_t *owner, size_t offset, size_t size, nb_buf_view_t *out);

nb_buf_pool_t *nb_buf_pool_create(const nb_buf_pool_config_t *config);
nb_buf_t *nb_buf_acquire(nb_buf_pool_t *pool);
/* Release the pool and all cached slots. */
void nb_buf_pool_destroy(nb_buf_pool_t *pool);
void nb_buf_pool_get_stats(const nb_buf_pool_t *pool, nb_buf_pool_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NB_BUF_H */
