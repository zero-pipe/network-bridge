#ifndef NB_PUBSUB_INTERNAL_H
#define NB_PUBSUB_INTERNAL_H

/*
 * Core-internal access to the pub/sub spine (src/core/pubsub.c).
 *
 * The public pubsub.h keeps nb_reader_t opaque. Wait sets (waitset.c) need the
 * reader's history queue to attach to it, so they come through this header
 * rather than reinterpreting the opaque type.
 */

#include <nb/pubsub.h>

#include "sample_queue.h"

/* History behind a reader; NULL for a callback reader (no history). */
nb_sample_queue_t *nb_ps_reader_queue(nb_reader_t *reader);

#endif /* NB_PUBSUB_INTERNAL_H */
