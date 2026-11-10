#ifndef NB_BASE_SAMPLE_INFO_H
#define NB_BASE_SAMPLE_INFO_H

/*
 * DDS sample metadata shared by the DDS spine and the RTPS engine.
 *
 * This header is model-neutral data only; the message envelope that carries
 * RPC traffic lives in <nb/rpc/message.h>.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Width of a DDS instance handle (a key hash), in octets. */
#define NB_INSTANCE_HANDLE_SIZE 16

/*
 * State of an inbound DDS sample. VALID is zero on purpose: a zeroed
 * nb_sample_info_t reads as ordinary data rather than as a dispose.
 */
typedef enum nb_sample_state {
    NB_SAMPLE_STATE_VALID = 0,
    NB_SAMPLE_STATE_DISPOSED = 1,
    NB_SAMPLE_STATE_NO_WRITERS = 2
} nb_sample_state_t;

typedef enum nb_view_state {
    NB_VIEW_STATE_NOT_NEW = 0,
    NB_VIEW_STATE_NEW = 1
} nb_view_state_t;

typedef enum nb_instance_state {
    NB_INSTANCE_STATE_ALIVE = 0,
    NB_INSTANCE_STATE_NOT_ALIVE_DISPOSED = 1,
    NB_INSTANCE_STATE_NOT_ALIVE_NO_WRITERS = 2
} nb_instance_state_t;

typedef struct nb_sample_info {
    nb_sample_state_t state;
    bool has_instance_handle;
    uint8_t instance_handle[NB_INSTANCE_HANDLE_SIZE];
    /* Writer source timestamp (INFO_TS) in nanoseconds; 0 when none. */
    int64_t source_timestamp_ns;
    /* Reader reception timestamp in nanoseconds; 0 when not stamped. */
    int64_t reception_timestamp_ns;
    nb_instance_state_t instance_state;
    nb_view_state_t view_state;
    /* DDS generationRank: 0 for the first sample of an instance the reader
     * sees, incremented whenever the instance is re-registered. */
    int32_t generation_rank;
} nb_sample_info_t;

/* DDS `SampleInfo::valid_data`: false only for a state-change notification. */
static inline bool nb_sample_info_valid_data(const nb_sample_info_t *sample)
{
    return !sample || sample->state == NB_SAMPLE_STATE_VALID;
}

#ifdef __cplusplus
}
#endif

#endif /* NB_BASE_SAMPLE_INFO_H */
