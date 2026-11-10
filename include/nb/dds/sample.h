#ifndef NB_DDS_SAMPLE_H
#define NB_DDS_SAMPLE_H

/*
 * DDS samples: state masks, instance filtering, and the value returned by a
 * reader.
 *
 * A reader either forwards samples to a callback or keeps a bounded KEEP_LAST
 * history that the application drains with nb_dds_reader_read /
 * nb_dds_reader_take. The queue is per reader, not per instance; instance
 * filtering is applied by nb_dds_sample_filter_t at read/take time.
 */

#include <nb/base/buf.h>
#include <nb/base/sample_info.h>
#include <nb/base/types.h>

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef nb_sample_info_t nb_dds_sample_info_t;

/*
 * DDS StateMask, flattened into one bit set. A sample matches when its sample
 * state, view state and instance state each select one of these bits. Passing
 * 0 as the mask means NB_DDS_STATE_ANY.
 */
typedef uint32_t nb_dds_state_mask_t;

#define NB_DDS_STATE_SAMPLE_READ 0x00000001u
#define NB_DDS_STATE_SAMPLE_NOT_READ 0x00000002u
#define NB_DDS_STATE_VIEW_NEW 0x00000004u
#define NB_DDS_STATE_VIEW_NOT_NEW 0x00000008u
#define NB_DDS_STATE_INSTANCE_ALIVE 0x00000010u
#define NB_DDS_STATE_INSTANCE_DISPOSED 0x00000020u
#define NB_DDS_STATE_INSTANCE_NO_WRITERS 0x00000040u

#define NB_DDS_STATE_ANY 0x0000007fu
#define NB_DDS_STATE_ANY_DATA_READER                                                         \
    (NB_DDS_STATE_SAMPLE_NOT_READ | NB_DDS_STATE_VIEW_NEW | NB_DDS_STATE_INSTANCE_ALIVE)

typedef struct nb_dds_sample_filter {
    nb_dds_state_mask_t state_mask; /* 0 is treated as NB_DDS_STATE_ANY */
    /* Instance handle (NB_INSTANCE_HANDLE_SIZE bytes) or NULL for any
     * instance. Samples without a key hash never match a non-NULL handle. */
    const uint8_t *instance;
} nb_dds_sample_filter_t;

#define NB_DDS_SAMPLE_FILTER_ANY                                                             \
    {                                                                                        \
        NB_DDS_STATE_ANY, NULL                                                               \
    }

/*
 * One sample. data is borrowed after read() and owned after take(); release
 * taken samples with nb_dds_sample_release().
 */
typedef struct nb_dds_sample {
    nb_payload_t data;
    nb_dds_sample_info_t info;
    /* Internal ownership of a taken sample's data (NULL for read views). */
    nb_buf_t *owner;
} nb_dds_sample_t;

void nb_dds_sample_release(nb_dds_sample_t *samples, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* NB_DDS_SAMPLE_H */
