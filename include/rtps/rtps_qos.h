#ifndef RTPS_QOS_H
#define RTPS_QOS_H

/*
 * QoS for topics, writers and readers.
 *
 * Every policy is optional: the `present` bitmask says which fields the caller
 * set. The effective QoS of an endpoint is resolved as
 *
 *     endpoint explicit value  >  topic value  >  DDS default
 *
 * A policy that is set neither on the endpoint nor on the topic is left at the
 * engine default and omitted from SEDP, exactly as the RTPS layer does today.
 * Enum values use the DDS/RTPS wire numbering so they can be mapped directly.
 */

#include <stdint.h>

#include "rtps_types.h"
#include "rtps_version.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RTPS_QOS_PRESENT_RELIABILITY (1u << 0)
#define RTPS_QOS_PRESENT_DURABILITY (1u << 1)
#define RTPS_QOS_PRESENT_HISTORY (1u << 2)
#define RTPS_QOS_PRESENT_DEADLINE (1u << 3)
#define RTPS_QOS_PRESENT_LIFESPAN (1u << 4)
#define RTPS_QOS_PRESENT_LIVELINESS (1u << 5)
#define RTPS_QOS_PRESENT_OWNERSHIP (1u << 6)
#define RTPS_QOS_PRESENT_DESTINATION_ORDER (1u << 7)
#define RTPS_QOS_PRESENT_PRESENTATION (1u << 8)
#define RTPS_QOS_PRESENT_PARTITION (1u << 9)
#define RTPS_QOS_PRESENT_LATENCY_BUDGET (1u << 10)
#define RTPS_QOS_PRESENT_TIME_BASED_FILTER (1u << 11)
#define RTPS_QOS_PRESENT_RESOURCE_LIMITS (1u << 12)
#define RTPS_QOS_PRESENT_AUTODISPOSE (1u << 13)

typedef enum rtps_reliability {
    RTPS_RELIABILITY_BEST_EFFORT = 1,
    RTPS_RELIABILITY_RELIABLE = 2
} rtps_reliability_t;

typedef enum rtps_durability {
    RTPS_DURABILITY_VOLATILE = 0,
    RTPS_DURABILITY_TRANSIENT_LOCAL = 1,
    RTPS_DURABILITY_TRANSIENT = 2,
    RTPS_DURABILITY_PERSISTENT = 3
} rtps_durability_t;

typedef enum rtps_history {
    RTPS_HISTORY_KEEP_LAST = 0,
    RTPS_HISTORY_KEEP_ALL = 1
} rtps_history_t;

typedef enum rtps_liveliness {
    RTPS_LIVELINESS_AUTOMATIC = 0,
    RTPS_LIVELINESS_MANUAL_BY_PARTICIPANT = 1,
    RTPS_LIVELINESS_MANUAL_BY_TOPIC = 2
} rtps_liveliness_t;

typedef enum rtps_ownership {
    RTPS_OWNERSHIP_SHARED = 0,
    RTPS_OWNERSHIP_EXCLUSIVE = 1
} rtps_ownership_t;

typedef enum rtps_destination_order {
    RTPS_DESTINATION_ORDER_BY_RECEPTION = 0,
    RTPS_DESTINATION_ORDER_BY_SOURCE = 1
} rtps_destination_order_t;

typedef enum rtps_presentation_scope {
    RTPS_PRESENTATION_INSTANCE = 0,
    RTPS_PRESENTATION_TOPIC = 1,
    RTPS_PRESENTATION_GROUP = 2
} rtps_presentation_scope_t;

typedef struct rtps_qos {
    uint32_t present; /* RTPS_QOS_PRESENT_* bits */
    uint32_t reliability; /* rtps_reliability_t */
    uint32_t durability; /* rtps_durability_t */
    uint32_t history; /* rtps_history_t */
    uint32_t depth; /* KEEP_LAST depth; 0 = engine default */
    uint32_t deadline_ms; /* 0 = infinite */
    uint32_t lifespan_ms; /* 0 = infinite */
    uint32_t liveliness_kind; /* rtps_liveliness_t */
    uint32_t liveliness_lease_ms; /* 0 = infinite */
    uint32_t ownership; /* rtps_ownership_t */
    int32_t ownership_strength;
    uint32_t destination_order; /* rtps_destination_order_t */
    uint32_t presentation_scope; /* rtps_presentation_scope_t */
    int coherent_access;
    int ordered_access;
    char partition[RTPS_PARTITION_MAX]; /* comma-separated names */
    uint32_t latency_budget_ms;
    uint32_t time_based_filter_ms; /* reader-side minimum separation */
    /* ResourceLimits; 0 means unlimited in the API, encoded as -1 on the wire. */
    uint32_t max_samples;
    uint32_t max_instances;
    uint32_t max_samples_per_instance;
    int autodispose_unregistered_instances;
} rtps_qos_t;

RTPS_API void rtps_qos_init(rtps_qos_t *qos);
RTPS_API int rtps_qos_is_default(const rtps_qos_t *qos);

RTPS_API void rtps_qos_set_reliability(rtps_qos_t *qos, rtps_reliability_t value);
RTPS_API void rtps_qos_set_durability(rtps_qos_t *qos, rtps_durability_t value);
RTPS_API void rtps_qos_set_history(rtps_qos_t *qos, rtps_history_t kind, uint32_t depth);
RTPS_API void rtps_qos_set_deadline_ms(rtps_qos_t *qos, uint32_t ms);
RTPS_API void rtps_qos_set_lifespan_ms(rtps_qos_t *qos, uint32_t ms);
RTPS_API void rtps_qos_set_liveliness(rtps_qos_t *qos,
                                      rtps_liveliness_t kind,
                                      uint32_t lease_ms);
RTPS_API void rtps_qos_set_ownership(rtps_qos_t *qos,
                                     rtps_ownership_t kind,
                                     int32_t strength);
RTPS_API void rtps_qos_set_destination_order(rtps_qos_t *qos,
                                             rtps_destination_order_t value);
RTPS_API void rtps_qos_set_presentation(rtps_qos_t *qos,
                                        rtps_presentation_scope_t scope,
                                        int coherent,
                                        int ordered);
RTPS_API void rtps_qos_set_partition(rtps_qos_t *qos, const char *names);
RTPS_API void rtps_qos_set_latency_budget_ms(rtps_qos_t *qos, uint32_t ms);
RTPS_API void rtps_qos_set_time_based_filter_ms(rtps_qos_t *qos, uint32_t ms);
RTPS_API void rtps_qos_set_resource_limits(rtps_qos_t *qos,
                                           uint32_t max_samples,
                                           uint32_t max_instances,
                                           uint32_t max_samples_per_instance);
RTPS_API void rtps_qos_set_autodispose(rtps_qos_t *qos, int enabled);

#ifdef __cplusplus
}
#endif

#endif /* RTPS_QOS_H */
