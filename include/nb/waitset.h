#ifndef NB_WAITSET_H
#define NB_WAITSET_H

/*
 * DDS wait set, scoped to queued subscriptions.
 *
 * A wait set holds a set of (service, method) reader histories. Waiting
 * blocks until at least one attached history has a sample matching the
 * filter, or until the timeout expires. The wait set only observes the
 * reader history: check the specific readers with nb_service_read /
 * nb_service_take after a successful wait.
 *
 * Wake-up is event driven (each enqueue signals the attached wait sets), so a
 * blocked waiter returns as soon as a sample is queued. Samples that were
 * already queued when the wait began are found by the initial scan.
 *
 * Threading: one thread may wait on a wait set while other threads enqueue.
 * Attaching/detaching and destroying are not safe concurrently with a wait in
 * progress on the same object.
 *
 * A pub/sub reader with a history can also be attached with
 * nb_waitset_attach_reader (<nb/pubsub.h>); that spelling does not need a
 * service/method pair.
 */

#include <nb/node.h>
#include <nb/sample.h>
#include <nb/service.h>

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

/*
 * Attach a queued subscription. The method must already be a reader history
 * (nb_service_add_queue_method / nb_subscriber_subscribe_queue); returns
 * NB_ERR_NOTFOUND otherwise.
 */
int nb_waitset_attach(nb_waitset_t *waitset, nb_service_t *service, const char *method);
int nb_waitset_detach(nb_waitset_t *waitset, nb_service_t *service, const char *method);

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
