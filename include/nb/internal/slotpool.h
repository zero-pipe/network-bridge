#ifndef NB_SLOTPOOL_H
#define NB_SLOTPOOL_H

/*
 * nb_slotpool: fixed-size pool indexed by 32-bit slot IDs, with a generation
 * counter to resist ABA reuse.
 *
 * Used by chunk and streaming paths: queues carry IDs rather than pointers, so
 * a recycled buffer can never leave a dangling pointer behind. Each slot owns
 * a fixed-size nb_buf (see buf.h).
 */

#include <stddef.h>
#include <stdint.h>

#include <nb/buf.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nb_slotpool nb_slotpool_t;
typedef uint32_t nb_slot_id_t;

#define NB_SLOT_ID_NONE 0u

typedef struct nb_slotpool_config {
    size_t payload_size; /* bytes per slot */
    size_t slot_count;   /* hard slot limit (1..65535) */
} nb_slotpool_config_t;

nb_slotpool_t *nb_slotpool_create(const nb_slotpool_config_t *config);
/* Release the pool and all slots. */
void nb_slotpool_destroy(nb_slotpool_t *pool);

/* Take a free slot; returns NB_SLOT_ID_NONE when exhausted. */
nb_slot_id_t nb_slotpool_alloc(nb_slotpool_t *pool);

/* Resolve an ID to a borrowed buffer; returns NULL for stale, freed, or
 * out-of-range IDs. */
nb_buf_t *nb_slotpool_get(nb_slotpool_t *pool, nb_slot_id_t id);

/* Return a slot; stale or duplicate frees are ignored. */
void nb_slotpool_free(nb_slotpool_t *pool, nb_slot_id_t id);

/* Bulk release (chunk / batch-return paths). */
void nb_slotpool_free_batch(nb_slotpool_t *pool, const nb_slot_id_t *ids, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* NB_SLOTPOOL_H */
