#ifndef NB_ALLOC_H
#define NB_ALLOC_H

/*
 * Global allocator and statistics (alloc).
 *
 * This module does not depend on the rest of the framework or ntk; it is usable on its own. The public
 * symbols are nb_malloc/nb_free, named for their semantics rather than the
 * file name. Install an allocator before creating threads; hot paths only read
 * the current configuration at runtime.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NB_MEM_ALIGN_DEFAULT 16u

typedef struct nb_allocator {
    void *(*alloc)(size_t size, void *opaque);
    void (*free)(void *ptr, void *opaque);
    void *opaque;
} nb_allocator_t;

typedef struct nb_mem_stats {
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t failed_alloc_count;
    uint64_t outstanding_bytes;
    uint64_t peak_bytes;
} nb_mem_stats_t;

/* NULL restores malloc/free. Returns NB_OK or NB_ERR_INVALID. */
int nb_mem_set_allocator(const nb_allocator_t *allocator);

/* Allocate via the installed allocator; return NULL on failure. */
void *nb_malloc(size_t size);
/* Zero-initialized allocation. */
void *nb_malloc0(size_t size);
/* Allocate with an explicit alignment; 0 uses NB_MEM_ALIGN_DEFAULT. */
void *nb_malloc_aligned(size_t size, size_t alignment);
/* Resize an allocation; contents up to the smaller size are preserved. */
void *nb_realloc(void *ptr, size_t size);
/* Release an allocation; NULL is a no-op. */
void nb_free(void *ptr);

/* Cumulative allocator statistics since start or the last reset. */
void nb_mem_get_stats(nb_mem_stats_t *out);
void nb_mem_reset_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_ALLOC_H */
