#ifndef NB_DDS_DISCOVERY_TYPES_H
#define NB_DDS_DISCOVERY_TYPES_H

/*
 * Discovery snapshot types shared by the DDS spine and the RTPS engine.
 *
 * This header is deliberately free of any RPC or node dependency: the RTPS
 * engine only needs the plain data types. The reader function lives in
 * <nb/dds/discovery_view.h> (old pub/sub API) and <nb/dds/participant.h>
 * (DDS spine).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NB_DDS_DISCOVERY_NAME_MAX 64
#define NB_DDS_DISCOVERY_TOPIC_MAX 160
#define NB_DDS_DISCOVERY_TYPE_MAX 160
#define NB_DDS_DISCOVERY_PARTITION_MAX 128
#define NB_DDS_DISCOVERY_HANDLE_SIZE 16

typedef enum nb_dds_builtin_topic {
    NB_DDS_BUILTIN_TOPIC_PARTICIPANT = 0,  /* DCPSParticipant */
    NB_DDS_BUILTIN_TOPIC_PUBLICATION = 1,  /* DCPSPublication */
    NB_DDS_BUILTIN_TOPIC_SUBSCRIPTION = 2, /* DCPSSubscription */
    NB_DDS_BUILTIN_TOPIC_TOPIC = 3         /* DCPSTopic, derived from endpoints */
} nb_dds_builtin_topic_t;

typedef struct nb_dds_discovery_sample {
    nb_dds_builtin_topic_t topic;
    /* Instance handle: participant or endpoint GUID, or the topic hash. */
    uint8_t handle[NB_DDS_DISCOVERY_HANDLE_SIZE];
    /* Participant name, or topic name for the topic view. */
    char name[NB_DDS_DISCOVERY_NAME_MAX];
    char type_name[NB_DDS_DISCOVERY_TYPE_MAX];
    /* Participant views: address the participant announced. */
    char host[NB_DDS_DISCOVERY_NAME_MAX];
    uint16_t port;
    int is_local;
    /* Endpoint views: writer or reader, and its announced QoS. */
    int is_writer;
    char partition[NB_DDS_DISCOVERY_PARTITION_MAX];
    uint32_t reliability;
    uint32_t durability;
    uint32_t history_kind;
    uint32_t history_depth;
    uint32_t deadline_ms;
    uint32_t lifespan_ms;
    uint32_t liveliness_kind;
    uint32_t liveliness_lease_ms;
    int32_t ownership_strength;
    /* Participant views: lease in milliseconds.
     * Topic views: how many discovered endpoints publish/subscribe it. */
    uint32_t lease_ms;
    uint32_t writer_count;
    uint32_t reader_count;
} nb_dds_discovery_sample_t;

#ifdef __cplusplus
}
#endif

#endif /* NB_DDS_DISCOVERY_TYPES_H */
