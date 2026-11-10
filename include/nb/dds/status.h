#ifndef NB_DDS_STATUS_H
#define NB_DDS_STATUS_H

/*
 * DDS status events (matching, QoS incompatible, deadline, liveliness, lost or
 * rejected samples).
 *
 * A backend that can detect status delivers these to the callback registered
 * with nb_dds_participant_set_status_callback. The callback runs on the
 * backend thread and must not call back into the same participant.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NB_DDS_STATUS_ENDPOINT_MAX 96
#define NB_DDS_STATUS_REASON_MAX 32
#define NB_DDS_STATUS_GUID_SIZE 16

typedef enum nb_dds_status_kind {
    NB_DDS_STATUS_NONE = 0,
    NB_DDS_STATUS_OFFERED_DEADLINE_MISSED = 1,
    NB_DDS_STATUS_REQUESTED_DEADLINE_MISSED = 2,
    NB_DDS_STATUS_LIVELINESS_CHANGED = 3,
    NB_DDS_STATUS_PUBLICATION_MATCHED = 4,
    NB_DDS_STATUS_SUBSCRIPTION_MATCHED = 5,
    NB_DDS_STATUS_QOS_INCOMPATIBLE = 6,
    NB_DDS_STATUS_SAMPLE_LOST = 7,
    NB_DDS_STATUS_SAMPLE_REJECTED = 8
} nb_dds_status_kind_t;

typedef struct nb_dds_status_event {
    nb_dds_status_kind_t kind;
    /* Backend name of the local endpoint; empty when it has none. */
    char endpoint[NB_DDS_STATUS_ENDPOINT_MAX];
    uint32_t count;
    uint32_t total_count;
    /* Liveliness: whether the matched writer is alive now and the totals. */
    int alive;
    uint32_t alive_count;
    uint32_t not_alive_count;
    /* The remote endpoint the event refers to, when it names one. */
    bool has_peer;
    uint8_t peer_guid[NB_DDS_STATUS_GUID_SIZE];
    /* QoS mismatch / rejection: the policy that failed, e.g. "deadline". */
    char reason[NB_DDS_STATUS_REASON_MAX];
} nb_dds_status_event_t;

typedef void (*nb_dds_status_callback_t)(void *user, const nb_dds_status_event_t *event);

#ifdef __cplusplus
}
#endif

#endif /* NB_DDS_STATUS_H */
