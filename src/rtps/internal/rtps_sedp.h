#ifndef NB_RTPS_SEDP_H
#define NB_RTPS_SEDP_H

/*
 * SEDP endpoint discovery payload.
 *
 * This module builds and parses the ParameterList carried by a publication or
 * subscription DATA sample. Reliable writer state, heartbeat/acknack handling,
 * and endpoint bookkeeping live in the participant runtime.
 */

#include <stddef.h>
#include <stdint.h>

#include <nb/base/error.h>

#include "rtps_types.h"

#define NB_RTPS_SEDP_TOPIC_MAX 128
#define NB_RTPS_SEDP_TYPE_MAX 128
#define NB_RTPS_SEDP_PARTITION_MAX 64
#define NB_RTPS_SEDP_USER_DATA_MAX 256
#define NB_RTPS_SEDP_LOCATORS_MAX 4
#define NB_RTPS_SEDP_PAYLOAD_MAX 1200

/* QoS values used by PID_RELIABILITY, PID_DURABILITY, and PID_HISTORY. */
#define NB_RTPS_RELIABILITY_BEST_EFFORT 1u
#define NB_RTPS_RELIABILITY_RELIABLE 2u

#define NB_RTPS_DURABILITY_VOLATILE 0u
#define NB_RTPS_DURABILITY_TRANSIENT_LOCAL 1u
#define NB_RTPS_DURABILITY_TRANSIENT 2u
#define NB_RTPS_DURABILITY_PERSISTENT 3u

#define NB_RTPS_HISTORY_KEEP_LAST 0u
#define NB_RTPS_HISTORY_KEEP_ALL 1u

/* PID_OWNERSHIP, PID_LIVELINESS and PID_DESTINATION_ORDER carry the DDS enum
 * values, not our own. */
#define NB_RTPS_OWNERSHIP_SHARED 0u
#define NB_RTPS_OWNERSHIP_EXCLUSIVE 1u

#define NB_RTPS_LIVELINESS_AUTOMATIC 0u
#define NB_RTPS_LIVELINESS_MANUAL_BY_PARTICIPANT 1u
#define NB_RTPS_LIVELINESS_MANUAL_BY_TOPIC 2u

#define NB_RTPS_DESTINATION_ORDER_BY_RECEPTION 0u
#define NB_RTPS_DESTINATION_ORDER_BY_SOURCE 1u

/* PID_PRESENTATION carries the DDS access scope (INSTANCE < TOPIC < GROUP) and
 * the coherent/ordered access flags. */
#define NB_RTPS_PRESENTATION_INSTANCE 0u
#define NB_RTPS_PRESENTATION_TOPIC 1u
#define NB_RTPS_PRESENTATION_GROUP 2u

typedef struct nb_rtps_endpoint_info {
    nb_rtps_guid_t endpoint_guid;
    char topic_name[NB_RTPS_SEDP_TOPIC_MAX];
    char type_name[NB_RTPS_SEDP_TYPE_MAX];
    uint32_t reliability;
    uint32_t durability;
    uint32_t history_kind;
    uint32_t history_depth;
    char partition[NB_RTPS_SEDP_PARTITION_MAX];
    /* Announced QoS. Durations are milliseconds; 0 means "absent", which is
     * the DDS default (infinite deadline/lifespan/lease, zero latency budget). */
    uint32_t ownership;
    uint32_t ownership_strength;
    uint32_t destination_order;
    uint32_t deadline_ms;
    uint32_t latency_budget_ms;
    uint32_t lifespan_ms;
    uint32_t liveliness_kind;
    uint32_t liveliness_lease_ms;
    /* Presentation; the DDS default is INSTANCE with both flags clear and is
     * omitted from the wire. */
    uint32_t presentation_scope;
    int coherent_access;
    int ordered_access;
    /* TimeBasedFilter minimum separation; 0 means no filter. */
    uint32_t min_separation_ms;
    /* ResourceLimits; 0 means unlimited. */
    uint32_t max_samples;
    uint32_t max_instances;
    uint32_t max_samples_per_instance;
    /* Local-only WriterDataLifecycle flag; DDS does not announce it, so it
     * never travels in SEDP. */
    int autodispose;
    nb_rtps_locator_t unicast[NB_RTPS_SEDP_LOCATORS_MAX];
    size_t unicast_count;
    nb_rtps_locator_t multicast[NB_RTPS_SEDP_LOCATORS_MAX];
    size_t multicast_count;
    uint8_t user_data[NB_RTPS_SEDP_USER_DATA_MAX];
    size_t user_data_len;
    /* Subscription only: ask a Cyclone writer to put PID_KEY_HASH in the inline
     * QoS of every keyed sample, which is how a reader learns the instance
     * handle without deserializing the key. Cyclone-specific (PID 0x801c) and
     * ignored by other implementations. Local-only, never decoded. */
    int requests_keyhash;
} nb_rtps_endpoint_info_t;

/* Build the serialized ParameterList for a SEDP DATA payload. */
int nb_rtps_sedp_build(const nb_rtps_endpoint_info_t *info,
                       void *buf,
                       size_t cap,
                       size_t *out_len);

/* Parse a SEDP payload. Endpoint GUID, topic name, and type name are required;
 * unknown parameters (including vendor-specific ones) are ignored. */
int nb_rtps_sedp_parse(const void *payload, size_t len, nb_rtps_endpoint_info_t *out);

/* Parse the endpoint GUID from a key-only dispose sample. */
/* Build the key-only payload of a dispose sample: a ParameterList that carries
 * just PID_ENDPOINT_GUID (RTPS 2.5 8.5.4). A peer needs it because a DATA
 * submessage flagged KEY refers to the instance by its key, not by the full
 * endpoint description. */
int nb_rtps_sedp_build_key(const nb_rtps_guid_t *endpoint_guid,
                           void *buf,
                           size_t cap,
                           size_t *out_len);

int nb_rtps_sedp_parse_key(const void *payload, size_t len, nb_rtps_guid_t *out);

#endif /* NB_RTPS_SEDP_H */
