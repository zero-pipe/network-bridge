#ifndef NB_RTPS_PARTICIPANT_H
#define NB_RTPS_PARTICIPANT_H

/*
 * SPDP participant runtime.
 *
 * One instance owns one RTPS participant: it announces itself on the SPDP
 * multicast group (optional) and to configured unicast peers, receives
 * announcements from other participants, and tracks their leases.
 *
 * The runtime owns discovery plus a minimal best-effort application-data send
 * and receive path. Incoming samples are delivered to a local subscription
 * through nb_rtps_participant_set_data_callback().
 */

#include <stddef.h>
#include <stdint.h>

#include <nb/error.h>
#include <nb/discovery_view.h>
#include <nb/metrics.h>

#include "rtps_data.h"
#include "rtps_sedp.h"
#include "rtps_spdp.h"
#include "rtps_tcp.h"
#include "rtps_types.h"

#define NB_RTPS_PEER_HOST_MAX 64
#define NB_RTPS_MAX_PEERS 64
#define NB_RTPS_DEFAULT_MAX_REMOTES 1024u
/* Largest payload carried inline in a single DATA submessage. Bigger samples
 * are split into DATA_FRAG fragments of NB_RTPS_FRAGMENT_SIZE octets. */
#define NB_RTPS_DATA_PAYLOAD_MAX 1200u
/* Largest sample accepted by nb_rtps_participant_publish(). The reliable
 * history keeps at most NB_RTPS_RELIABLE_MAX_BYTES, so a reliable sample larger
 * than that budget is retained alone and earlier samples are evicted. */
#define NB_RTPS_SAMPLE_MAX (1024u * 1024u)
/* Fragment size used when a sample does not fit in one DATA submessage. Must
 * stay well below the packet budget so a fragment plus its submessage header
 * fits in one datagram on a 1500-byte MTU, must be a multiple of four, and must
 * be greater than 1024: RTPS 2.5 9.4.5.3.3 requires fragmentSize > 1024, and a
 * strict peer discards smaller fragments as malformed. */
#define NB_RTPS_FRAGMENT_SIZE 1028u

typedef enum nb_rtps_participant_event {
    NB_RTPS_PARTICIPANT_ADDED = 0,
    NB_RTPS_PARTICIPANT_UPDATED = 1,
    NB_RTPS_PARTICIPANT_REMOVED = 2
} nb_rtps_participant_event_t;

typedef enum nb_rtps_endpoint_kind {
    NB_RTPS_ENDPOINT_PUBLICATION = 0,
    NB_RTPS_ENDPOINT_SUBSCRIPTION = 1
} nb_rtps_endpoint_kind_t;

typedef enum nb_rtps_endpoint_event {
    NB_RTPS_ENDPOINT_ADDED = 0,
    NB_RTPS_ENDPOINT_UPDATED = 1,
    NB_RTPS_ENDPOINT_REMOVED = 2
} nb_rtps_endpoint_event_t;

typedef struct nb_rtps_peer {
    char host[NB_RTPS_PEER_HOST_MAX];
    uint16_t port;
} nb_rtps_peer_t;

typedef struct nb_rtps_participant_config {
    uint32_t domain_id; /* 0 = default domain */
    /* Interface to bind. NULL or "0.0.0.0" binds every interface. */
    const char *bind_host;
    /* Address advertised in locators. NULL = bind_host when it is a concrete
     * IPv4 address, otherwise 127.0.0.1. Set this explicitly in production. */
    const char *advertise_host;
    /* SPDP multicast group. NULL = 239.255.0.1; "" disables multicast. */
    const char *multicast_group;
    /* SPDP port. 0 = 7400 + 250 * domain_id when multicast is enabled, or an
     * ephemeral port when multicast is disabled. */
    uint16_t spdp_port;
    /* Metatraffic unicast port. 0 = ephemeral; the chosen value is advertised. */
    uint16_t unicast_port;
    uint32_t announce_interval_ms; /* 0 = 3000, minimum 100 */
    uint32_t lease_ms;             /* 0 = 20000, range [1000, 3600000] */
    const char *name;
    /* Optional framework data-plane locator advertised as default unicast. */
    nb_rtps_locator_t default_unicast;
    const nb_rtps_peer_t *peers;
    size_t peer_count;
    uint32_t max_remote_participants; /* 0 = 1024 */
    uint32_t max_local_endpoints;    /* 0 = 1024 */
    uint32_t max_remote_endpoints;   /* 0 = 4096 */
    uint32_t sedp_heartbeat_ms;      /* 0 = 1000, minimum 100 */
    /* HEARTBEAT interval for reliable application writers. 0 = 1000, minimum
     * 100. */
    uint32_t data_heartbeat_ms;
    /* Default writer history depth for reliable publications. 0 = 8. The
     * endpoint's history_depth wins when it is non-zero. */
    uint32_t data_history_depth;
    /* Log every datagram sent and received (hex, first 48 octets) at debug
     * level. The rtps transport also honours NB_RTPS_DUMP_FRAMES=1. */
    int dump_frames;
    /* RTPS over TCP. 0 keeps the participant UDP-only. When enabled, the
     * listening port is advertised as a TCP locator in SPDP; tcp_port 0 asks
     * the system for one. Everything except SPDP then follows the peer onto
     * the stream once the two sides have discovered each other. */
    int enable_tcp;
    uint16_t tcp_port;
    /* Standard PB/DG/PG port mapping, see the public config. */
    int standard_ports;
    uint32_t participant_id;
} nb_rtps_participant_config_t;

/* Cumulative RTPS counters. They are also surfaced through nb_node_metrics()
 * in the rtps_* fields (scope NB_METRIC_SCOPE_RTPS). */
typedef struct nb_rtps_stats {
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t spdp_sent;
    uint64_t spdp_received;
    uint64_t sedp_sent;
    uint64_t sedp_received;
    uint64_t data_sent;
    uint64_t data_received;
    uint64_t heartbeats_sent;
    uint64_t heartbeats_received;
    uint64_t acknacks_sent;
    uint64_t acknacks_received;
    uint64_t gaps_sent;
    uint64_t gaps_received;
    uint64_t fragments_sent;
    uint64_t fragments_received;
    uint64_t nack_frags_sent;
    uint64_t nack_frags_received;
    uint64_t parse_errors;
    uint64_t samples_dropped;
    uint64_t lease_expired;
} nb_rtps_stats_t;

typedef struct nb_rtps_participant nb_rtps_participant_t;

typedef void (*nb_rtps_participant_callback_t)(void *user,
                                               nb_rtps_participant_event_t event,
                                               const nb_rtps_participant_info_t *info);

typedef void (*nb_rtps_endpoint_callback_t)(void *user,
                                            nb_rtps_endpoint_event_t event,
                                            nb_rtps_endpoint_kind_t kind,
                                            const nb_rtps_endpoint_info_t *info);

/*
 * Called once for each local subscription that matches an incoming application
 * DATA sample. The message payload is a zero-copy view into the receive buffer
 * and is only valid for the duration of the callback.
 *
 * For a subscription whose type name is "dds::Message" the view holds the
 * deserialized dds::Message.payload bytes, exactly as before. For any other
 * type name the view holds the CDR body of the sample, that is the received
 * payload without its four-byte encapsulation header; the application owns the
 * codec for that type. Publish such samples with
 * nb_rtps_participant_publish() and a four-byte encapsulation header followed
 * by the CDR body.
 */
typedef void (*nb_rtps_data_callback_t)(void *user,
                                        const nb_rtps_guid_t *reader_guid,
                                        const nb_rtps_guid_t *writer_guid,
                                        const nb_rtps_dds_message_t *message);

/* Bind sockets and initialize state. Does not start the discovery thread. */
nb_rtps_participant_t *nb_rtps_participant_create(const nb_rtps_participant_config_t *config);
void nb_rtps_participant_destroy(nb_rtps_participant_t *participant);

int nb_rtps_participant_start(nb_rtps_participant_t *participant);
void nb_rtps_participant_stop(nb_rtps_participant_t *participant);

/* Set before start when possible; safe to change while running. */
void nb_rtps_participant_set_callback(nb_rtps_participant_t *participant,
                                      nb_rtps_participant_callback_t callback,
                                      void *user);

/* Set before start when possible; safe to change while running. Called for
 * remote endpoints discovered through SEDP. */
void nb_rtps_participant_set_endpoint_callback(nb_rtps_participant_t *participant,
                                               nb_rtps_endpoint_callback_t callback,
                                               void *user);

/* Set before start when possible; safe to change while running. */
void nb_rtps_participant_set_data_callback(nb_rtps_participant_t *participant,
                                           nb_rtps_data_callback_t callback,
                                           void *user);

/* Add a local endpoint. The runtime fills endpoint_guid and, when no unicast
 * locator is supplied, reuses the participant default unicast locator. */
int nb_rtps_participant_add_endpoint(nb_rtps_participant_t *participant,
                                     nb_rtps_endpoint_kind_t kind,
                                     nb_rtps_endpoint_info_t *info);

/* Remove a local endpoint and send a SEDP dispose to known peers. */
int nb_rtps_participant_remove_endpoint(nb_rtps_participant_t *participant,
                                        const nb_rtps_guid_t *endpoint_guid);

/* Send an already CDR-encapsulated payload as application DATA to every
 * matching remote subscription. writer_guid must identify a local publication
 * endpoint added with nb_rtps_participant_add_endpoint().
 *
 * Best-effort publications are sent once and not retained. Reliable
 * publications keep a bounded history, emit HEARTBEATs, retransmit samples
 * requested by ACKNACK, and announce evicted samples with GAP. The local
 * subscription side mirrors that: it deduplicates by writer sequence number,
 * answers HEARTBEATs with ACKNACK, and honours GAP. */
int nb_rtps_participant_publish(nb_rtps_participant_t *participant,
                                const nb_rtps_guid_t *writer_guid,
                                const void *payload,
                                size_t payload_len);

/*
 * Same, with an explicit RTPS source timestamp for the INFO_TS of the sample.
 * BY_SOURCE_TIMESTAMP subscriptions order samples by this value, so a caller
 * that already owns a capture or log timestamp must use this entry point
 * instead of letting the writer stamp the send time. A NULL timestamp falls
 * back to the wall clock. Not part of the public API; the framework transport
 * has no timestamped publish yet.
 */
int nb_rtps_participant_publish_at(nb_rtps_participant_t *participant,
                                   const nb_rtps_guid_t *writer_guid,
                                   const void *payload,
                                   size_t payload_len,
                                   const nb_rtps_time_t *source_ts);

/*
 * Publish a keyed sample. `status` is the DDS PID_STATUS_INFO value: 0 for a
 * live sample, 1 to dispose the instance and 2 to unregister it. The handle
 * travels in PID_KEY_HASH; for a state change the payload should be the
 * serialized key (the usual four-byte encapsulation header followed by the key
 * fields), and the sample goes out as key-only. Internal.
 */
int nb_rtps_participant_publish_instance(
    nb_rtps_participant_t *participant,
    const nb_rtps_guid_t *writer_guid,
    const uint8_t handle[NB_INSTANCE_HANDLE_SIZE],
    uint32_t status,
    const void *payload,
    size_t payload_len);

/*
 * Whether the (writer, reader) receive window has already consumed sn. Samples
 * that are filtered after the window advances (dispose, key-only, non-owner,
 * out-of-order) are consumed without being delivered, so this is how a test
 * asserts that a peer will stop retransmitting them. Internal.
 */
int nb_rtps_participant_reader_consumed(nb_rtps_participant_t *participant,
                                        const nb_rtps_guid_t *writer_guid,
                                        const nb_rtps_guid_t *reader_guid,
                                        uint64_t sn);

/* Serialize dds::Message (sequence<octet> payload) with CDR_LE and publish it. */
int nb_rtps_participant_publish_dds_message(
    nb_rtps_participant_t *participant,
    const nb_rtps_guid_t *writer_guid,
    const nb_rtps_dds_message_t *message);

/* Add a unicast SPDP destination. Only valid before start. */
int nb_rtps_participant_add_peer(nb_rtps_participant_t *participant,
                                 const char *host,
                                 uint16_t port);

/* Port actually bound, useful when spdp_port was 0. */
uint16_t nb_rtps_participant_spdp_port(const nb_rtps_participant_t *participant);
size_t nb_rtps_participant_remote_count(const nb_rtps_participant_t *participant);
size_t nb_rtps_participant_local_endpoint_count(const nb_rtps_participant_t *participant);
size_t nb_rtps_participant_remote_endpoint_count(const nb_rtps_participant_t *participant);

/* Send one announcement now. */
int nb_rtps_participant_announce(nb_rtps_participant_t *participant);

/* Current local participant record. */
int nb_rtps_participant_local_info(const nb_rtps_participant_t *participant,
                                   nb_rtps_participant_info_t *out);

/* Number of remote endpoints currently matched with one local endpoint. */
size_t nb_rtps_participant_matched_count(const nb_rtps_participant_t *participant,
                                         const nb_rtps_guid_t *endpoint_guid);

/*
 * Snapshot one of the DDS built-in topics (the DCPS* views). `topic` is an
 * nb_builtin_topic_t value; up to max entries are written to out and the
 * number written is returned. Topics are derived from the participant's
 * local and remote endpoints, so DCPSTopic reports the discovered
 * (topic, type) pairs with their writer/reader counts.
 */
size_t nb_rtps_participant_discovery_read(nb_rtps_participant_t *participant,
                                          int topic,
                                          nb_discovery_sample_t *out,
                                          size_t max);

/* RTPS over TCP. */
int nb_rtps_participant_tcp_enabled(const nb_rtps_participant_t *participant);
uint16_t nb_rtps_participant_tcp_port(const nb_rtps_participant_t *participant);
size_t nb_rtps_participant_tcp_links(const nb_rtps_participant_t *participant);
void nb_rtps_participant_tcp_stats(const nb_rtps_participant_t *participant,
                                   nb_rtps_tcp_stats_t *out);

/*
 * Snapshot the cumulative counters plus the current discovery table sizes.
 * Only the rtps_* fields of out are written; the rest is left untouched so a
 * caller can merge several providers into one nb_metrics_t.
 */
void nb_rtps_participant_metrics(const nb_rtps_participant_t *participant,
                                 nb_metrics_t *out);

/*
 * Status events. These are the RTPS-level half of the DDS status contract:
 * `NB_RTPS_STATUS_OFFERED_DEADLINE_MISSED` is raised on a local publication
 * whose write cadence exceeded its `deadline_ms`, and
 * `NB_RTPS_STATUS_REQUESTED_DEADLINE_MISSED` on a local subscription that has
 * not received a sample from a matched writer within its `deadline_ms`. Both
 * are rate limited to one event per deadline period and carry the running miss
 * count, so a caller can distinguish "still missing" from a new stall.
 * `NB_RTPS_STATUS_LIVELINESS_CHANGED` is raised on a local subscription when a
 * matched automatic-liveliness writer stops or resumes asserting its lease
 * through ParticipantMessageData; `alive` says which way it moved and the
 * `alive_count`/`not_alive_count` pair is the DDS LivelinessChangedStatus
 * counted over that subscription's matched writers.
 * `NB_RTPS_STATUS_PUBLICATION_MATCHED`/`NB_RTPS_STATUS_SUBSCRIPTION_MATCHED`
 * are raised whenever a local endpoint gains or loses a matched remote endpoint
 * with the same topic and type, carrying the current match count in `count`
 * (a drop means the peer went away) and the cumulative count in `total_count`.
 * `NB_RTPS_STATUS_QOS_INCOMPATIBLE` is raised instead when such a pair exists
 * but the QoS cannot match; `reason` names the failing policy.
 * `NB_RTPS_STATUS_SAMPLE_LOST` counts samples the reader will never receive (a
 * GAP covered them, or the writer withdrew them) and
 * `NB_RTPS_STATUS_SAMPLE_REJECTED` counts samples the reader dropped on purpose
 * (TimeBasedFilter, source ordering, resource limits); both carry a cumulative
 * count in `count`.
 *
 * The callback runs on the participant thread; it must not call back into the
 * participant.
 */
typedef enum nb_rtps_status_kind {
    NB_RTPS_STATUS_OFFERED_DEADLINE_MISSED = 1,
    NB_RTPS_STATUS_REQUESTED_DEADLINE_MISSED = 2,
    NB_RTPS_STATUS_LIVELINESS_CHANGED = 3,
    NB_RTPS_STATUS_PUBLICATION_MATCHED = 4,
    NB_RTPS_STATUS_SUBSCRIPTION_MATCHED = 5,
    NB_RTPS_STATUS_QOS_INCOMPATIBLE = 6,
    NB_RTPS_STATUS_SAMPLE_LOST = 7,
    NB_RTPS_STATUS_SAMPLE_REJECTED = 8
} nb_rtps_status_kind_t;

typedef struct nb_rtps_status_event {
    nb_rtps_status_kind_t kind;
    /* The local endpoint the event belongs to. */
    nb_rtps_guid_t endpoint;
    uint32_t count;
    /* Match events only: cumulative matches for that local endpoint. */
    uint32_t total_count;
    /* Liveliness only: the remote writer whose lease changed, whether it is
     * alive now, and the subscription's writer totals. */
    nb_rtps_guid_t writer;
    /* Whether `writer` names a peer: set for liveliness and match events. */
    int has_peer;
    int alive;
    uint32_t alive_count;
    uint32_t not_alive_count;
    /* QoS incompatible only: the policy that failed. */
    char reason[32];
} nb_rtps_status_event_t;

typedef void (*nb_rtps_status_callback_t)(void *user, const nb_rtps_status_event_t *event);

int nb_rtps_participant_set_status_callback(nb_rtps_participant_t *participant,
                                            nb_rtps_status_callback_t callback,
                                            void *user);

#endif /* NB_RTPS_PARTICIPANT_H */
