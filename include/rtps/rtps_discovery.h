#ifndef RTPS_DISCOVERY_H
#define RTPS_DISCOVERY_H

/*
 * Snapshot views over the discovery tables (the DDS built-in topics). Entries
 * are copied; later discovery changes do not modify a returned snapshot.
 */

#include <stddef.h>
#include <stdint.h>

#include "rtps_types.h"
#include "rtps_version.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RTPS_DISCOVERY_NAME_MAX 64u
#define RTPS_DISCOVERY_TOPIC_MAX 160u
#define RTPS_DISCOVERY_TYPE_MAX 160u
#define RTPS_DISCOVERY_PARTITION_MAX 128u

typedef enum rtps_builtin_topic {
    RTPS_BUILTIN_TOPIC_PARTICIPANT = 0, /* DCPSParticipant */
    RTPS_BUILTIN_TOPIC_PUBLICATION = 1, /* DCPSPublication */
    RTPS_BUILTIN_TOPIC_SUBSCRIPTION = 2,/* DCPSSubscription */
    RTPS_BUILTIN_TOPIC_TOPIC = 3        /* DCPSTopic, derived from endpoints */
} rtps_builtin_topic_t;

typedef struct rtps_discovery_sample {
    rtps_builtin_topic_t topic;
    /* Instance handle: the participant or endpoint GUID, or a topic hash. */
    uint8_t handle[RTPS_INSTANCE_HANDLE_SIZE];
    char name[RTPS_DISCOVERY_NAME_MAX]; /* participant or topic name */
    char type_name[RTPS_DISCOVERY_TYPE_MAX];
    /* Participant views: the announced address. */
    char host[RTPS_DISCOVERY_NAME_MAX];
    uint16_t port;
    int is_local;
    /* Endpoint views. */
    int is_writer;
    char partition[RTPS_DISCOVERY_PARTITION_MAX];
    uint32_t reliability;
    uint32_t durability;
    uint32_t history_kind;
    uint32_t history_depth;
    uint32_t deadline_ms;
    uint32_t lifespan_ms;
    uint32_t liveliness_kind;
    uint32_t liveliness_lease_ms;
    int32_t ownership_strength;
    /* Participant lease, or the writer/reader counts for a topic view. */
    uint32_t lease_ms;
    uint32_t writer_count;
    uint32_t reader_count;
} rtps_discovery_sample_t;

#ifdef __cplusplus
}
#endif

#endif /* RTPS_DISCOVERY_H */
