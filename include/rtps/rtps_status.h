#ifndef RTPS_STATUS_H
#define RTPS_STATUS_H

/*
 * Status events reported by the participant on its own thread. They are the
 * public half of the DDS status contract: the local endpoint, a count, and for
 * match/liveliness events the peer it refers to.
 */

#include "rtps_types.h"
#include "rtps_version.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rtps_status_kind {
    RTPS_STATUS_OFFERED_DEADLINE_MISSED = 1,
    RTPS_STATUS_REQUESTED_DEADLINE_MISSED = 2,
    RTPS_STATUS_LIVELINESS_CHANGED = 3,
    RTPS_STATUS_PUBLICATION_MATCHED = 4,
    RTPS_STATUS_SUBSCRIPTION_MATCHED = 5,
    RTPS_STATUS_QOS_INCOMPATIBLE = 6,
    RTPS_STATUS_SAMPLE_LOST = 7,
    RTPS_STATUS_SAMPLE_REJECTED = 8
} rtps_status_kind_t;

typedef struct rtps_status_event {
    rtps_status_kind_t kind;
    /* The local endpoint the event belongs to. */
    rtps_guid_t endpoint;
    /* Current count; cumulative count for match events. */
    uint32_t count;
    uint32_t total_count;
    /* Peer the event refers to, when has_peer is set. */
    rtps_guid_t peer;
    int has_peer;
    /* Liveliness: whether the peer is alive now and the subscription totals. */
    int alive;
    uint32_t alive_count;
    uint32_t not_alive_count;
    /* QoS incompatible: the failing policy, for example "reliability". */
    char reason[32];
} rtps_status_event_t;

/*
 * Runs on the participant thread, outside its internal lock. The callback must
 * not call back into the participant.
 */
typedef void (*rtps_status_callback_t)(void *user, const rtps_status_event_t *event);

#ifdef __cplusplus
}
#endif

#endif /* RTPS_STATUS_H */
