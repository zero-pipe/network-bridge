#ifndef NB_SAMPLE_QUEUE_H
#define NB_SAMPLE_QUEUE_H

/*
 * Reader-side history for the DDS spine.
 *
 * One queue per DDS reader. Samples are copied on the way in, so the backend
 * callback may return immediately. read() returns borrowed views into the
 * queue storage; take() transfers the buffer to the caller (release with
 * nb_dds_sample_release). The queue has its own mutex and wakes wait sets.
 */

#include <nb/dds/sample.h>

#include <ntk.h>

#include <stddef.h>
#include <stdint.h>

/* Default KEEP_LAST depth when the caller passes 0. */
#define NB_SAMPLE_QUEUE_DEFAULT_DEPTH 32u
/* Wait sets that can listen to one history (each waitset takes one slot). */
#define NB_SAMPLE_QUEUE_MAX_LISTENERS 8u

typedef struct nb_sample_queue nb_sample_queue_t;

/* Called after a sample has been enqueued; used by wait sets to wake up. */
typedef void (*nb_sample_notify_fn)(void *user);

nb_sample_queue_t *nb_sample_queue_create(uint32_t depth);
void nb_sample_queue_destroy(nb_sample_queue_t *queue);

/* KEEP_LAST enqueue: a full queue drops its oldest sample. */
int nb_sample_queue_push(nb_sample_queue_t *queue,
                         const void *data,
                         size_t len,
                         uint8_t encoding,
                         const nb_dds_sample_info_t *info);

size_t nb_sample_queue_read(nb_sample_queue_t *queue,
                            const nb_dds_sample_filter_t *filter,
                            nb_dds_sample_t *out,
                            size_t max);
size_t nb_sample_queue_take(nb_sample_queue_t *queue,
                            const nb_dds_sample_filter_t *filter,
                            nb_dds_sample_t *out,
                            size_t max);
size_t nb_sample_queue_size(nb_sample_queue_t *queue);
/* Samples dropped because the history was full; a monotonic counter. */
uint64_t nb_sample_queue_dropped(nb_sample_queue_t *queue);
/* Samples matching filter without marking them read (wait set readiness). */
size_t nb_sample_queue_match_count(nb_sample_queue_t *queue,
                                   const nb_dds_sample_filter_t *filter);
void nb_sample_queue_set_depth(nb_sample_queue_t *queue, uint32_t depth);
/* Register a wake-up callback; called after every successful enqueue.
 * NB_ERR_AGAIN when the queue already has NB_SAMPLE_QUEUE_MAX_LISTENERS. */
int nb_sample_queue_add_listener(nb_sample_queue_t *queue, nb_sample_notify_fn fn, void *user);
void nb_sample_queue_remove_listener(nb_sample_queue_t *queue,
                                     nb_sample_notify_fn fn,
                                     void *user);

#endif /* NB_SAMPLE_QUEUE_H */
