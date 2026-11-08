#ifndef NB_SAMPLE_QUEUE_H
#define NB_SAMPLE_QUEUE_H

/*
 * Reader-side history for DDS read/take.
 *
 * One queue per subscription (service method). Samples are copied on the way
 * in, so the transport frame can be released as soon as the enqueue returns.
 * The queue is guarded by its own mutex; read() returns borrowed views into
 * the queue storage and is meant for a single application reader per
 * subscription, while take() hands back an owned copy.
 */

#include <nb/message.h>
#include <nb/sample.h>

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
int nb_sample_queue_push(nb_sample_queue_t *queue, const nb_message_t *msg);

size_t nb_sample_queue_read(nb_sample_queue_t *queue,
                            const nb_sample_filter_t *filter,
                            nb_sample_t *out,
                            size_t max);
size_t nb_sample_queue_take(nb_sample_queue_t *queue,
                            const nb_sample_filter_t *filter,
                            nb_sample_t *out,
                            size_t max);
size_t nb_sample_queue_size(nb_sample_queue_t *queue);
/* Samples dropped because the history was full; a monotonic counter. */
uint64_t nb_sample_queue_dropped(nb_sample_queue_t *queue);
/* Samples matching filter without marking them read (wait set readiness). */
size_t nb_sample_queue_match_count(nb_sample_queue_t *queue, const nb_sample_filter_t *filter);
void nb_sample_queue_set_depth(nb_sample_queue_t *queue, uint32_t depth);
/* Register a wake-up callback; called after every successful enqueue.
 * NB_ERR_AGAIN when the queue already has NB_SAMPLE_QUEUE_MAX_LISTENERS. */
int nb_sample_queue_add_listener(nb_sample_queue_t *queue, nb_sample_notify_fn fn, void *user);
void nb_sample_queue_remove_listener(nb_sample_queue_t *queue,
                                     nb_sample_notify_fn fn,
                                     void *user);

#endif /* NB_SAMPLE_QUEUE_H */
