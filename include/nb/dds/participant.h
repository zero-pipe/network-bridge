#ifndef NB_DDS_PARTICIPANT_H
#define NB_DDS_PARTICIPANT_H

/*
 * DDS spine: participant lifecycle and backend selection.
 *
 * A participant owns a DDS domain and one backend (rtps or cyclone). Unlike
 * the old pub/sub API it does not need an RPC node, bindings, routes or the
 * ntk_discovery control plane: the backend runs its own DDS discovery
 * (SPDP/SEDP).
 *
 *   nb_dds_participant_t   domain + backend + default QoS/partition/prefix
 *   nb_dds_topic_t         name + type name + QoS
 *   nb_dds_writer_t        publishes samples on a topic
 *   nb_dds_reader_t        receives samples (callback or history)
 */

#include <nb/base/types.h>
#include <nb/dds/discovery_types.h>
#include <nb/dds/qos.h>
#include <nb/dds/sample.h>
#include <nb/dds/status.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nb_dds_participant nb_dds_participant_t;
typedef struct nb_dds_topic nb_dds_topic_t;
typedef struct nb_dds_writer nb_dds_writer_t;
typedef struct nb_dds_reader nb_dds_reader_t;
typedef struct nb_dds_waitset nb_dds_waitset_t;

typedef enum nb_dds_backend {
    NB_DDS_BACKEND_RTPS = 0, /* in-tree RTPS engine (always available) */
    NB_DDS_BACKEND_CYCLONE = 1 /* Cyclone DDS (NB_ENABLE_DDS build option) */
} nb_dds_backend_t;

/* RTPS backend tuning. All fields are optional; 0/NULL selects the engine
 * default. peers entries are "host:port" unicast SPDP targets, which keeps
 * tests and single-host deployments away from multicast. */
typedef struct nb_dds_rtps_options {
    uint32_t struct_size;
    const char *bind_host;
    const char *advertise_host;
    const char *multicast_group; /* NULL = 239.255.0.1; "" disables multicast */
    uint16_t spdp_port;
    uint16_t unicast_port;
    uint32_t announce_interval_ms;
    uint32_t lease_ms;
    uint32_t data_heartbeat_ms;
    uint32_t data_history_depth;
    int enable_tcp;
    uint16_t tcp_port;
    int standard_ports;
    uint32_t participant_id;
    const char *const *peers;
    size_t peer_count;
} nb_dds_rtps_options_t;

/* Cyclone backend tuning. */
typedef struct nb_dds_cyclone_options {
    uint32_t struct_size;
    const char *config_xml; /* optional Cyclone DDS configuration */
} nb_dds_cyclone_options_t;

typedef struct nb_dds_participant_options {
    nb_dds_backend_t backend;
    uint32_t domain;
    /* Default partition/prefix/QoS. Topic and endpoint QoS may override them. */
    const char *partition;
    const char *prefix;
    const char *name;
    const nb_dds_qos_t *default_qos;
    const nb_dds_rtps_options_t *rtps;
    const nb_dds_cyclone_options_t *cyclone;
} nb_dds_participant_options_t;

/* Participant metrics. Zero for backends that do not report a counter. */
typedef struct nb_dds_metrics {
    uint64_t samples_sent;
    uint64_t samples_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t heartbeats_sent;
    uint64_t heartbeats_received;
    uint64_t nacks_sent;
    uint64_t nacks_received;
    uint64_t gaps_sent;
    uint64_t gaps_received;
    uint64_t parse_errors;
    uint64_t samples_dropped;
    uint64_t participants;
    uint64_t publications;
    uint64_t subscriptions;
} nb_dds_metrics_t;

int nb_dds_participant_create(const nb_dds_participant_options_t *options,
                              nb_dds_participant_t **out);
void nb_dds_participant_destroy(nb_dds_participant_t *participant);
const char *nb_dds_participant_name(const nb_dds_participant_t *participant);
nb_dds_backend_t nb_dds_participant_backend(const nb_dds_participant_t *participant);

/* Status events run on the backend thread; the application must not call back
 * into the same participant from the callback. */
int nb_dds_participant_set_status_callback(nb_dds_participant_t *participant,
                                           nb_dds_status_callback_t callback,
                                           void *user);

/* Snapshot of the DDS built-in topics. Returns 0 for backends without a
 * discovery view. */
size_t nb_dds_participant_discovery_read(nb_dds_participant_t *participant,
                                         nb_dds_builtin_topic_t topic,
                                         nb_dds_discovery_sample_t *out,
                                         size_t max);

/* Fill a metrics snapshot; zeroes fields the backend does not report. */
void nb_dds_participant_metrics(const nb_dds_participant_t *participant,
                                nb_dds_metrics_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NB_DDS_PARTICIPANT_H */
