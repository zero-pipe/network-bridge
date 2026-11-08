#ifndef NB_WAITSET_H
#define NB_WAITSET_H

/*
 * DDS wait set, scoped to pub/sub reader histories.
 *
 * A wait set holds a set of reader histories attached with
 * nb_waitset_attach_reader (<nb/pubsub.h>). Waiting blocks until at least one
 * attached history has a sample matching the filter, or until the timeout
 * expires. The wait set only observes the history: check the specific readers
 * with nb_reader_read / nb_reader_take after a successful wait.
 *
 * Wake-up is event driven (each enqueue signals the attached wait sets), so a
 * blocked waiter returns as soon as a sample is queued. Samples that were
 * already queued when the wait began are found by the initial scan.
 *
 * Threading: one thread may wait on a wait set while other threads enqueue.
 * Attaching/detaching and destroying are not safe concurrently with a wait in
 * progress on the same object.
 */

#include <nb/node.h>
#include <nb/sample.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nb_waitset nb_waitset_t;

/* Block indefinitely (wait set timeout). */
#define NB_WAITSET_INFINITE 0xFFFFFFFFu

int nb_waitset_create(nb_node_t *node, nb_waitset_t **out);
void nb_waitset_destroy(nb_waitset_t *waitset);

/* Number of attached histories that currently have a matching sample. */
size_t nb_waitset_ready(nb_waitset_t *waitset, const nb_sample_filter_t *filter);

/*
 * Returns 1 when at least one attached history has a matching sample, 0 on
 * timeout, or a negative framework error.
 */
int nb_waitset_wait(nb_waitset_t *waitset,
                    const nb_sample_filter_t *filter,
                    uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* NB_WAITSET_H */
