#ifndef NB_METRICS_H
#define NB_METRICS_H

/*
 * Runtime metrics: cumulative counters, gauges, and the end-to-end latency
 * histogram exposed through nb_node_metrics(), plus a name/type/unit registry
 * that exporters can iterate without hard-coding metric semantics.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* End-to-end latency histogram buckets: <=1,2,5,10,25,50,100,250,500,1000,2500,
 * 5000,10000 ms; the last bucket covers >10000 ms. */
#define NB_LATENCY_HIST_BUCKETS 14

/* Histogram upper bounds in milliseconds (NB_LATENCY_HIST_BUCKETS - 1 entries);
 * the last bucket means >=10000 ms. */
extern const uint32_t nb_latency_bounds[NB_LATENCY_HIST_BUCKETS - 1];

/* ---- Node cumulative metrics ---- */
typedef struct nb_metrics {
    uint64_t requests_sent;
    uint64_t responses_received;
    uint64_t requests_failed;
    uint64_t timeouts;
    uint64_t retries;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t udp_packets_sent;
    uint64_t udp_packets_received;
    uint64_t udp_retransmissions;
    uint64_t udp_nacks_sent;
    uint64_t udp_reassembly_expired;
    uint64_t tcp_frames_sent;
    uint64_t tcp_frames_received;
    uint64_t backpressure_events;
    /* RTCP reports (RFC 3550, UDP transport only) */
    uint64_t rtcp_sr_sent;      /* sender reports sent by this node */
    uint64_t rtcp_rr_sent;      /* receiver reports sent by this node */
    uint64_t rtcp_sr_received;  /* sender reports received */
    uint64_t rtcp_rr_received;  /* receiver reports received */
    uint64_t rtcp_bye_sent;     /* BYE packets sent */
    uint64_t rtcp_bye_received; /* BYE packets received */
    uint32_t rtcp_rtt_ms;       /* latest RTT derived from a peer report block */
    uint32_t rtcp_jitter_ms;    /* current inbound jitter estimate */
    int32_t rtcp_remote_lost;   /* cumulative loss this stream reports to the peer */

    /* Observability: current values (gauges) and end-to-end latency distribution */
    uint64_t connections;       /* active links/sessions (UDP session + TCP link) */
    uint64_t inflight_requests; /* client calls currently outstanding */
    uint64_t inflight_handlers; /* worker/executor tasks currently outstanding */
    uint64_t latency_count;     /* completed end-to-end samples */
    uint64_t latency_sum_ms;    /* latency total, for the mean */
    uint64_t latency_hist[NB_LATENCY_HIST_BUCKETS];
    uint64_t latency_p50_ms; /* quantiles estimated from the histogram */
    uint64_t latency_p90_ms;
    uint64_t latency_p99_ms;
    uint64_t rate_limited;  /* requests dropped/rolled back by per-peer rate limiting */
    uint64_t streams_rejected; /* new streams rejected by the concurrency cap */

    /* RTPS transport (scope NB_METRIC_SCOPE_RTPS). Zero for other transports. */
    uint64_t rtps_packets_sent;
    uint64_t rtps_packets_received;
    uint64_t rtps_bytes_sent;
    uint64_t rtps_bytes_received;
    uint64_t rtps_spdp_sent;        /* participant announcements */
    uint64_t rtps_spdp_received;
    uint64_t rtps_sedp_sent;        /* endpoint announcements */
    uint64_t rtps_sedp_received;
    uint64_t rtps_data_sent;        /* user DATA and DATA_FRAG samples */
    uint64_t rtps_data_received;
    uint64_t rtps_heartbeats_sent;
    uint64_t rtps_heartbeats_received;
    uint64_t rtps_acknacks_sent;
    uint64_t rtps_acknacks_received;
    uint64_t rtps_gaps_sent;
    uint64_t rtps_gaps_received;
    uint64_t rtps_fragments_sent;
    uint64_t rtps_fragments_received;
    uint64_t rtps_nack_frags_sent;
    uint64_t rtps_nack_frags_received;
    uint64_t rtps_parse_errors;     /* datagrams or submessages that did not decode */
    uint64_t rtps_samples_dropped;  /* matched reader found, sample not delivered */
    uint64_t rtps_lease_expired;    /* remote participants that timed out */
    /* Gauges: current discovery table sizes */
    uint64_t rtps_participants;
    uint64_t rtps_publications;
    uint64_t rtps_subscriptions;
} nb_metrics_t;

/* ---- Peer link quality ---- */
/*
 * Transport-neutral peer link statistics, see nb_node_peer_stats().
 *
 * UDP fills these from RTCP SR/RR; transports without link feedback (for
 * example TCP) return NB_ERR_NOTFOUND. Only "link quality" semantics are
 * exposed here; concrete RTP/RTCP packet types stay out of the public
 * abstraction.
 */
typedef struct nb_peer_stats {
    uint32_t local_id;    /* local link id (UDP: SSRC) */
    uint32_t remote_id;   /* peer link id (UDP: SSRC) */
    uint32_t rtt_ms;      /* round-trip time estimate */
    uint32_t jitter_ms;   /* inbound jitter estimate */
    int32_t lost_packets; /* cumulative loss reported by the peer */
    uint64_t feedback_sent;     /* link feedback packets sent (UDP: SR+RR) */
    uint64_t feedback_received; /* link feedback packets received (UDP: SR+RR) */
} nb_peer_stats_t;

/* ---- Metric registry ----
 *
 * Names/types/units are registered in one table, so exporters (Prometheus,
 * etc.) only iterate the table instead of guessing semantics from scattered
 * NB_METRIC_INC call sites. offset is the field offset inside nb_metrics_t.
 */
typedef enum nb_metric_type {
    NB_METRIC_TYPE_COUNTER = 0,
    NB_METRIC_TYPE_GAUGE,
    NB_METRIC_TYPE_HISTOGRAM
} nb_metric_type_t;

/*
 * Which transport populates a metric. nb_metrics_t stays one struct (no ABI
 * split), but fields outside the node's active transport scope are always
 * zero; exporters use this tag to filter instead of guessing from the name.
 */
typedef enum nb_metric_scope {
    NB_METRIC_SCOPE_NODE = 0,    /* transport-neutral */
    NB_METRIC_SCOPE_UDP = 1,     /* UDP transport only */
    NB_METRIC_SCOPE_TCP = 2,     /* TCP transport only */
    NB_METRIC_SCOPE_UDP_RTCP = 3, /* RFC 3550 link feedback, UDP transport only */
    NB_METRIC_SCOPE_RTPS = 4     /* RTPS provider only */
} nb_metric_scope_t;

typedef struct nb_metric_desc {
    const char *name;
    nb_metric_type_t type;
    const char *unit;
    size_t offset;
    nb_metric_scope_t scope;
} nb_metric_desc_t;

extern const nb_metric_desc_t nb_metric_table[];
extern const size_t nb_metric_table_count;

/* Look up an entry by name; returns NULL when absent. */
const nb_metric_desc_t *nb_metric_find(const char *name);
/* Latency histogram bucket bounds in milliseconds; returns the bucket count.
 * bounds may be NULL. */
size_t nb_metric_latency_bounds(const uint32_t **bounds);

#ifdef __cplusplus
}
#endif

#endif /* NB_METRICS_H */
