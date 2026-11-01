#ifndef NB_DISCOVERY_VIEW_H
#define NB_DISCOVERY_VIEW_H

/*
 * Read-only views over the discovery tables (the DDS built-in topics).
 *
 * DDS exposes discovery data as ordinary readers on DCPSParticipant,
 * DCPSPublication, DCPSSubscription and DCPSTopic. This stack has no typed
 * built-in topics, so the same information is returned as a flat snapshot
 * instead: one call returns the current entries of one built-in topic from
 * every transport that implements discovery (today only `rtps`).
 *
 * The snapshot is local to the process. It reflects what the local
 * participants have discovered so far, so it lags remote changes by the
 * discovery and lease intervals exactly like the built-in topics do.
 *
 * The `handle` field is the DDS instance handle of the entry: the RTPS GUID
 * for participants and endpoints, and the topic hash for topic entries.
 */

#include <nb/node.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NB_DISCOVERY_NAME_MAX 64
#define NB_DISCOVERY_TOPIC_MAX 160
#define NB_DISCOVERY_TYPE_MAX 160
#define NB_DISCOVERY_PARTITION_MAX 128
#define NB_DISCOVERY_HANDLE_SIZE 16

typedef enum nb_builtin_topic {
    NB_BUILTIN_TOPIC_PARTICIPANT = 0,  /* DCPSParticipant */
    NB_BUILTIN_TOPIC_PUBLICATION = 1,  /* DCPSPublication */
    NB_BUILTIN_TOPIC_SUBSCRIPTION = 2, /* DCPSSubscription */
    NB_BUILTIN_TOPIC_TOPIC = 3         /* DCPSTopic, derived from endpoints */
} nb_builtin_topic_t;

typedef struct nb_discovery_sample {
    nb_builtin_topic_t topic;
    /* Instance handle: participant or endpoint GUID, or the topic hash. */
    uint8_t handle[NB_DISCOVERY_HANDLE_SIZE];
    /* Participant name, or topic name for the topic view. */
    char name[NB_DISCOVERY_NAME_MAX];
    /* Endpoint and topic views. */
    char type_name[NB_DISCOVERY_TYPE_MAX];
    /* Participant views: address the participant announced. */
    char host[NB_DISCOVERY_NAME_MAX];
    uint16_t port;
    /* Whether the entry belongs to this process. */
    int is_local;
    /* Endpoint views: writer or reader, and its announced QoS. */
    int is_writer;
    char partition[NB_DISCOVERY_PARTITION_MAX];
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
} nb_discovery_sample_t;

/*
 * Copy up to max entries of one built-in topic into out and return the number
 * written (0 when discovery is unavailable). Entries are a snapshot; later
 * discovery changes do not modify them.
 */
size_t nb_node_discovery_read(nb_node_t *node,
                              nb_builtin_topic_t topic,
                              nb_discovery_sample_t *out,
                              size_t max);

#ifdef __cplusplus
}
#endif

#endif /* NB_DISCOVERY_VIEW_H */
