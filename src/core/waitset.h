#ifndef NB_WAITSET_INTERNAL_H
#define NB_WAITSET_INTERNAL_H

#include <nb/waitset.h>

#include <ntk.h>

#include "sample_queue.h"

typedef struct nb_waitset_link {
    struct nb_waitset_link *next;
    /* Exactly one key is set: (service, method) for a legacy service-method
     * history, or reader for a pub/sub reader. */
    nb_service_t *service;
    char method[NB_METHOD_NAME_MAX];
    struct nb_reader *reader;
    nb_sample_queue_t *queue;
} nb_waitset_link_t;

struct nb_waitset {
    nb_node_t *node;
    ntk_mutex *lock; /* guards links */
    ntk_sem *sem;    /* one permit per enqueue on an attached history */
    nb_waitset_link_t *links;
};

#endif /* NB_WAITSET_INTERNAL_H */
