#ifndef NB_STATUS_H
#define NB_STATUS_H

/*
 * DDS status events, as reported by the provider that can detect them.
 *
 * A provider delivers these to the callback registered in
 * nb_transport_provider_options_t.on_status, next to the ordinary receive
 * callback. The callback runs on the provider's own thread and must not call
 * back into the provider.
 *
 * The RTPS provider reports deadline misses (offered and requested),
 * liveliness changes, endpoint matches and incompatible QoS. `count` is the
 * running counter for deadline misses, the current number of matches for a
 * match event (so a decrease means the peer went away), and the number of
 * affected writers for liveliness. `total_count` is the cumulative match count
 * for match events and zero elsewhere.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Room for the provider's name of the endpoint (usually the method). */
#define NB_STATUS_ENDPOINT_MAX 96
/* Room for the policy name of a QoS mismatch, e.g. "deadline". */
#define NB_STATUS_REASON_MAX 32
/* Width of an RTPS GUID prefix plus entity id, when the provider has one. */
#define NB_STATUS_GUID_SIZE 16

typedef enum nb_status_kind {
    NB_STATUS_NONE = 0,
    /* A publication did not write within its deadline. `count` is the miss
     * count, `endpoint` the publication. */
    NB_STATUS_OFFERED_DEADLINE_MISSED = 1,
    /* A subscription received nothing from a matched writer within its
     * deadline. */
    NB_STATUS_REQUESTED_DEADLINE_MISSED = 2,
    /* A matched writer stopped or resumed asserting its liveliness lease.
     * `alive` says which way it moved. */
    NB_STATUS_LIVELINESS_CHANGED = 3,
    /* A remote subscription matched a local publication, or stopped matching
     * it. `count` is the publication's current match count. */
    NB_STATUS_PUBLICATION_MATCHED = 4,
    /* A remote publication matched a local subscription, or stopped matching
     * it. `count` is the subscription's current match count. */
    NB_STATUS_SUBSCRIPTION_MATCHED = 5,
    /* A remote endpoint has the same topic and type but cannot match the local
     * endpoint; `reason` names the policy that failed. */
    NB_STATUS_QOS_INCOMPATIBLE = 6,
    /* The reader knows a sample will never arrive (a GAP covered it, or the
     * writer withdrew it). `count` is the cumulative number lost. */
    NB_STATUS_SAMPLE_LOST = 7,
    /* The reader received a sample it refused to store: TimeBasedFilter, an
     * ordering rule, or a resource limit. `count` is the cumulative number
     * rejected, `reason` names the policy. */
    NB_STATUS_SAMPLE_REJECTED = 8
} nb_status_kind_t;

typedef struct nb_status_event {
    nb_status_kind_t kind;
    /* Provider name of the local endpoint the event belongs to; empty when the
     * provider has no name for it. */
    char endpoint[NB_STATUS_ENDPOINT_MAX];
    uint32_t count;
    uint32_t total_count;
    /* Liveliness: whether the matched writer is alive now, and the totals over
     * the subscription's matched writers (DDS LivelinessChangedStatus). */
    int alive;
    uint32_t alive_count;
    uint32_t not_alive_count;
    /* The remote endpoint the event refers to, when it names one. */
    bool has_peer;
    uint8_t peer_guid[NB_STATUS_GUID_SIZE];
    /* QoS mismatch only: the policy that failed, e.g. "deadline". */
    char reason[NB_STATUS_REASON_MAX];
} nb_status_event_t;

typedef void (*nb_status_callback_t)(void *user, const nb_status_event_t *event);

#ifdef __cplusplus
}
#endif

#endif /* NB_STATUS_H */
