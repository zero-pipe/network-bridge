#ifndef NB_SUBSCRIBE_H
#define NB_SUBSCRIBE_H

/*
 * Subscriber-facing aliases. A subscriber is an ordinary service whose method
 * names a topic. Topic-based transports deliver inbound samples as EVENT
 * messages to that method.
 */

#include <nb/service.h>

typedef nb_service_t nb_subscriber_t;
typedef nb_handler_fn nb_subscriber_handler_fn;

/* Thin inline wrappers instead of macros, so these keep function-pointer and
 * debugger-friendly identities while preserving the old names. */
static inline int nb_subscriber_create(nb_node_t *node,
                                       const char *name,
                                       nb_subscriber_t **out)
{
    return nb_service_create(node, name, out);
}

static inline void nb_subscriber_destroy(nb_subscriber_t *sub)
{
    nb_service_destroy(sub);
}

static inline int nb_subscriber_subscribe(nb_subscriber_t *sub,
                                          const char *topic,
                                          nb_subscriber_handler_fn handler,
                                          void *user)
{
    return nb_service_add_method(sub, topic, handler, user);
}

/*
 * Queued subscription (DDS read/take): samples are kept in a reader-side
 * history of at most `depth` samples (0 = 32) instead of being delivered to a
 * callback. Drain it with nb_subscriber_read / nb_subscriber_take.
 */
static inline int nb_subscriber_subscribe_queue(nb_subscriber_t *sub,
                                                const char *topic,
                                                uint32_t depth)
{
    return nb_service_add_queue_method(sub, topic, depth);
}

static inline size_t nb_subscriber_read(nb_subscriber_t *sub,
                                        const char *topic,
                                        const nb_sample_filter_t *filter,
                                        nb_sample_t *out,
                                        size_t max)
{
    return nb_service_read(sub, topic, filter, out, max);
}

static inline size_t nb_subscriber_take(nb_subscriber_t *sub,
                                        const char *topic,
                                        const nb_sample_filter_t *filter,
                                        nb_sample_t *out,
                                        size_t max)
{
    return nb_service_take(sub, topic, filter, out, max);
}

static inline size_t nb_subscriber_queue_size(nb_subscriber_t *sub, const char *topic)
{
    return nb_service_queue_size(sub, topic);
}

#endif /* NB_SUBSCRIBE_H */
