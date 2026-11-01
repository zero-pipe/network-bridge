#ifndef NB_SAMPLE_H
#define NB_SAMPLE_H

/*
 * DDS reader history: read/take, state masks and instance filtering.
 *
 * A subscription either forwards samples to a callback
 * (nb_subscriber_subscribe) or keeps a bounded reader-side history that the
 * application drains with read/take (nb_subscriber_subscribe_queue). Both are
 * ordinary service methods; the queue simply replaces the callback for EVENT
 * messages.
 *
 * History follows DDS KEEP_LAST: a full queue drops its oldest sample. The
 * queue is per subscription, not per instance; instance filtering is done by
 * nb_sample_filter_t at read/take time.
 */

#include <nb/message.h>
#include <nb/message_view.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DDS StateMask, flattened into one bit set. A sample matches when its
 * sample state, view state and instance state each select one of these bits.
 * Passing 0 as the mask means NB_STATE_ANY.
 */
typedef uint32_t nb_state_mask_t;

#define NB_STATE_SAMPLE_READ 0x00000001u      /* already returned by read/take */
#define NB_STATE_SAMPLE_NOT_READ 0x00000002u  /* not returned yet */
#define NB_STATE_VIEW_NEW 0x00000004u         /* first sample of an instance */
#define NB_STATE_VIEW_NOT_NEW 0x00000008u
#define NB_STATE_INSTANCE_ALIVE 0x00000010u
#define NB_STATE_INSTANCE_DISPOSED 0x00000020u
#define NB_STATE_INSTANCE_NO_WRITERS 0x00000040u

#define NB_STATE_ANY 0x0000007fu
/* DDS ANY_DATA_READER_STATE: not read yet, new instance, alive. */
#define NB_STATE_ANY_DATA_READER                                                             \
    (NB_STATE_SAMPLE_NOT_READ | NB_STATE_VIEW_NEW | NB_STATE_INSTANCE_ALIVE)

typedef struct nb_sample_filter {
    nb_state_mask_t state_mask; /* 0 is treated as NB_STATE_ANY */
    /* Instance handle (NB_INSTANCE_HANDLE_SIZE bytes) or NULL for any
     * instance. Samples without a PID_KEY_HASH never match a non-NULL
     * handle. */
    const uint8_t *instance;
} nb_sample_filter_t;

#define NB_SAMPLE_FILTER_ANY                                                                 \
    {                                                                                        \
        NB_STATE_ANY, NULL                                                                   \
    }

typedef struct nb_sample {
    nb_message_view_t data; /* owned after take, borrowed after read */
    nb_sample_info_t info;
} nb_sample_t;

/* Release the payload of a taken sample; safe and cheap for read samples. */
static inline void nb_sample_release(nb_sample_t *sample)
{
    if (sample) {
        nb_message_view_release(&sample->data);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* NB_SAMPLE_H */
