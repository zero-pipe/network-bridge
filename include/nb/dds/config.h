#ifndef NB_DDS_CONFIG_H
#define NB_DDS_CONFIG_H

/*
 * DDS spine configuration and JSON loading.
 *
 * A document may also contain an `rpc` block; this parser ignores it. The DDS
 * spine is independent of the RPC node: it reads name/log plus the `dds` block
 * (backend, domain, partition, prefix, QoS, backend tuning).
 */

#include <nb/base/config_error.h>
#include <nb/base/types.h>
#include <nb/dds/participant.h>
#include <nb/dds/qos.h>
#include <nb/dds/topic.h>

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NB_DDS_CONFIG_MAX_PEERS 32

typedef struct nb_dds_peer_config {
    char host[NB_HOST_MAX];
    uint16_t port;
} nb_dds_peer_config_t;

typedef struct nb_dds_rtps_config {
    char bind_host[NB_HOST_MAX];
    char advertise_host[NB_HOST_MAX];
    char multicast_group[NB_HOST_MAX];
    uint16_t spdp_port;
    uint16_t unicast_port;
    uint32_t announce_interval_ms;
    uint32_t lease_ms;
    uint32_t data_heartbeat_ms;
    uint32_t data_history_depth;
    bool enable_tcp;
    uint16_t tcp_port;
    bool standard_ports;
    uint32_t participant_id;
    const nb_dds_peer_config_t *peers;
    size_t peer_count;
} nb_dds_rtps_config_t;

typedef struct nb_dds_config {
    nb_dds_backend_t backend;
    uint32_t domain;
    char partition[NB_DDS_QOS_PARTITION_MAX];
    char prefix[NB_DDS_TOPIC_NAME_MAX];
    char name[NB_NAME_MAX];
    nb_dds_qos_t qos;
    bool has_rtps;
    nb_dds_rtps_config_t rtps;
    bool has_cyclone;
    char cyclone_config_xml[512];
} nb_dds_config_t;

typedef struct nb_dds_config_doc nb_dds_config_doc_t;

void nb_dds_config_default(nb_dds_config_t *config);

int nb_dds_config_parse(const char *json, nb_dds_config_doc_t **out, nb_config_error_t *error);
int nb_dds_config_parse_file(const char *path,
                             nb_dds_config_doc_t **out,
                             nb_config_error_t *error);
void nb_dds_config_doc_destroy(nb_dds_config_doc_t *doc);
/* Read-only view of the parsed DDS configuration; NULL for a NULL document. */
const nb_dds_config_t *nb_dds_config_get(const nb_dds_config_doc_t *doc);
/* Create a participant from a parsed document. */
int nb_dds_participant_create_from_config(const nb_dds_config_doc_t *doc,
                                          nb_dds_participant_t **out);

#ifdef __cplusplus
}
#endif

#endif /* NB_DDS_CONFIG_H */
