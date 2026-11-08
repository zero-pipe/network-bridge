#ifndef NB_SERVICE_HISTORY_H
#define NB_SERVICE_HISTORY_H

/*
 * Legacy reader history attached to a service method (pub/sub spine).
 *
 * A queued method keeps inbound samples in a reader-side KEEP_LAST history
 * instead of calling a handler; the application drains it with
 * nb_service_read / nb_service_take. This is the pre-reader spelling of what
 * nb_reader_t now provides: a service method is not a topic, and the history
 * belongs to the pub/sub spine, not to RPC. rpc.h therefore does not declare
 * these calls; they are reachable from <nb/pubsub.h> and <nb/nb.h>.
 *
 * Kept for source compatibility during 0.x. New code creates a topic and a
 * history reader instead:
 *
 *     nb_topic_create(...); nb_reader_create(topic, NULL, NULL, NULL, depth, &r);
 *     nb_reader_take(r, NULL, samples, max);
 */

#include <nb/sample.h>
#include <nb/service.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Register a method that keeps inbound samples in a reader-side history instead
 * of invoking a handler. depth is the KEEP_LAST depth; 0 selects
 * NB_SAMPLE_QUEUE_DEFAULT_DEPTH. A full history drops its oldest sample.
 *
 * nb_service_set_queue() attaches a history to an already registered method
 * (its handler no longer sees EVENT messages). Method registration is
 * asynchronous, so nb_service_add_queue_method() is the race-free way to create
 * a queued subscription.
 */
int nb_service_add_queue_method(nb_service_t *service, const char *method, uint32_t depth);
int nb_service_set_queue(nb_service_t *service, const char *method, uint32_t depth);

/*
 * read/take up to max samples matching filter (NULL = any state, any instance).
 * Both mark returned samples READ. read() returns borrowed views that stay valid
 * until the sample is taken or the history is destroyed; take() removes the
 * samples and returns owned copies (release with nb_sample_release). Return the
 * number of samples written to out.
 */
size_t nb_service_read(nb_service_t *service,
                       const char *method,
                       const nb_sample_filter_t *filter,
                       nb_sample_t *out,
                       size_t max);
size_t nb_service_take(nb_service_t *service,
                       const char *method,
                       const nb_sample_filter_t *filter,
                       nb_sample_t *out,
                       size_t max);
/* Samples currently in the history / samples dropped because it was full. */
size_t nb_service_queue_size(nb_service_t *service, const char *method);
uint64_t nb_service_queue_dropped(nb_service_t *service, const char *method);

#ifdef __cplusplus
}
#endif

#endif /* NB_SERVICE_HISTORY_H */
