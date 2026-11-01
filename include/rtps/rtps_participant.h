#ifndef RTPS_PARTICIPANT_H
#define RTPS_PARTICIPANT_H

/*
 * Participant: one RTPS endpoint on the network. It owns topics, writers and
 * readers, runs the discovery thread, and is the only object that touches the
 * wire directly.
 */

#include <stdint.h>

#include "rtps_discovery.h"
#include "rtps_metrics.h"
#include "rtps_status.h"
#include "rtps_types.h"
#include "rtps_version.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rtps_participant rtps_participant_t;

typedef struct rtps_participant_config {
    uint32_t struct_size;
    uint32_t domain_id; /* 0 = default domain */
    /* Interface to bind. NULL or "0.0.0.0" binds every interface. */
    const char *bind_host;
    /* Address advertised in locators. NULL derives it from bind_host, or
     * falls back to 127.0.0.1; set it explicitly when multi-homed. */
    const char *advertise_host;
    /* SPDP multicast group. NULL = 239.255.0.1; "" disables multicast. */
    const char *multicast_group;
    /* SPDP port. 0 = 7400 + 250 * domain_id with multicast, otherwise
     * ephemeral. */
    uint16_t spdp_port;
    /* Metatraffic unicast port. 0 = ephemeral; the value is advertised. */
    uint16_t unicast_port;
    uint32_t announce_interval_ms; /* 0 = 3000, minimum 100 */
    uint32_t lease_ms;             /* 0 = 20000, range 1000..3600000 */
    /* Reliable user-data heartbeat cadence (0 = 1000 ms, minimum 100) and the
     * default KEEP_LAST depth for reliable publications (0 = 8). */
    uint32_t data_heartbeat_ms;
    uint32_t data_history_depth;
    const char *name;              /* optional participant name */
    /* RTPS over TCP: 0 keeps the participant UDP-only. */
    int enable_tcp;
    uint16_t tcp_port; /* 0 = ephemeral listening port */
    uint32_t max_remote_participants; /* 0 = 1024 */
    uint32_t max_local_endpoints;     /* 0 = 1024 */
    uint32_t max_remote_endpoints;    /* 0 = 4096 */
    /* Log every datagram (first 48 octets) at debug level. */
    int dump_frames;
} rtps_participant_config_t;

/* Zero a config and set struct_size; callers then override what they need. */
RTPS_API void rtps_participant_config_init(rtps_participant_config_t *config);

RTPS_API int rtps_participant_create(const rtps_participant_config_t *config,
                                     rtps_participant_t **out);
/* Destroys every topic, writer and reader, then stops the thread. */
RTPS_API void rtps_participant_destroy(rtps_participant_t *participant);

/* Start/stop the discovery and receive thread. Endpoints may be created
 * before or after start. */
RTPS_API int rtps_participant_start(rtps_participant_t *participant);
RTPS_API int rtps_participant_stop(rtps_participant_t *participant);

/* Add a unicast SPDP destination (unicast-only deployments). Only valid
 * before start. */
RTPS_API int rtps_participant_add_peer(rtps_participant_t *participant,
                                       const char *host,
                                       uint16_t port);

RTPS_API int rtps_participant_get_guid(const rtps_participant_t *participant,
                                       rtps_guid_t *out);
RTPS_API const char *rtps_participant_name(const rtps_participant_t *participant);

/* Status events run on the participant thread. */
RTPS_API int rtps_participant_set_status_callback(rtps_participant_t *participant,
                                                  rtps_status_callback_t callback,
                                                  void *user);

/* Snapshot one DDS built-in topic; returns the number of entries written. */
RTPS_API size_t rtps_participant_discovery_read(rtps_participant_t *participant,
                                                rtps_builtin_topic_t topic,
                                                rtps_discovery_sample_t *out,
                                                size_t max);

/* Snapshot the cumulative counters and current gauges. */
RTPS_API int rtps_participant_metrics(const rtps_participant_t *participant,
                                      rtps_metrics_t *out);

/* Listening port when TCP is enabled, else 0. */
RTPS_API uint16_t rtps_participant_tcp_port(const rtps_participant_t *participant);

#ifdef __cplusplus
}
#endif

#endif /* RTPS_PARTICIPANT_H */
