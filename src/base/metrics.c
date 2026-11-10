/* metrics 运行期开关的全局状态（见 atomic.h / config.h）。 */

#include "atomic.h"

#include <nb/base/metrics.h>

#include <string.h>

volatile int nb_metrics_enabled = 1;
volatile int nb_latency_enabled = 1;
volatile uint32_t nb_metrics_sample_every = 1;

/* 延迟直方图分桶单一真源，node.c / dispatch.c 共用。 */
const uint32_t nb_latency_bounds[NB_LATENCY_HIST_BUCKETS - 1] = {
    1, 2, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000
};

#define METRIC(field, type, unit, name, scope) \
    { name, type, unit, offsetof(nb_metrics_t, field), scope }

/* 名称/类型/单位登记（Prometheus 风格命名）。 */
const nb_metric_desc_t nb_metric_table[] = {
    METRIC(requests_sent, NB_METRIC_TYPE_COUNTER, "requests", "requests_sent_total", NB_METRIC_SCOPE_NODE),
    METRIC(responses_received, NB_METRIC_TYPE_COUNTER, "responses", "responses_received_total", NB_METRIC_SCOPE_NODE),
    METRIC(requests_failed, NB_METRIC_TYPE_COUNTER, "requests", "requests_failed_total", NB_METRIC_SCOPE_NODE),
    METRIC(timeouts, NB_METRIC_TYPE_COUNTER, "requests", "timeouts_total", NB_METRIC_SCOPE_NODE),
    METRIC(retries, NB_METRIC_TYPE_COUNTER, "requests", "retries_total", NB_METRIC_SCOPE_NODE),
    METRIC(bytes_sent, NB_METRIC_TYPE_COUNTER, "bytes", "bytes_sent_total", NB_METRIC_SCOPE_NODE),
    METRIC(bytes_received, NB_METRIC_TYPE_COUNTER, "bytes", "bytes_received_total", NB_METRIC_SCOPE_NODE),
    METRIC(udp_packets_sent, NB_METRIC_TYPE_COUNTER, "packets", "udp_packets_sent_total", NB_METRIC_SCOPE_UDP),
    METRIC(udp_packets_received, NB_METRIC_TYPE_COUNTER, "packets", "udp_packets_received_total", NB_METRIC_SCOPE_UDP),
    METRIC(udp_retransmissions, NB_METRIC_TYPE_COUNTER, "packets", "udp_retransmissions_total", NB_METRIC_SCOPE_UDP),
    METRIC(udp_nacks_sent, NB_METRIC_TYPE_COUNTER, "packets", "udp_nacks_sent_total", NB_METRIC_SCOPE_UDP),
    METRIC(udp_reassembly_expired, NB_METRIC_TYPE_COUNTER, "messages", "udp_reassembly_expired_total", NB_METRIC_SCOPE_UDP),
    METRIC(tcp_frames_sent, NB_METRIC_TYPE_COUNTER, "frames", "tcp_frames_sent_total", NB_METRIC_SCOPE_TCP),
    METRIC(tcp_frames_received, NB_METRIC_TYPE_COUNTER, "frames", "tcp_frames_received_total", NB_METRIC_SCOPE_TCP),
    METRIC(backpressure_events, NB_METRIC_TYPE_COUNTER, "events", "backpressure_events_total", NB_METRIC_SCOPE_NODE),
    METRIC(rtcp_sr_sent, NB_METRIC_TYPE_COUNTER, "packets", "rtcp_sr_sent_total", NB_METRIC_SCOPE_UDP_RTCP),
    METRIC(rtcp_rr_sent, NB_METRIC_TYPE_COUNTER, "packets", "rtcp_rr_sent_total", NB_METRIC_SCOPE_UDP_RTCP),
    METRIC(rtcp_sr_received, NB_METRIC_TYPE_COUNTER, "packets", "rtcp_sr_received_total", NB_METRIC_SCOPE_UDP_RTCP),
    METRIC(rtcp_rr_received, NB_METRIC_TYPE_COUNTER, "packets", "rtcp_rr_received_total", NB_METRIC_SCOPE_UDP_RTCP),
    METRIC(rtcp_bye_sent, NB_METRIC_TYPE_COUNTER, "packets", "rtcp_bye_sent_total", NB_METRIC_SCOPE_UDP_RTCP),
    METRIC(rtcp_bye_received, NB_METRIC_TYPE_COUNTER, "packets", "rtcp_bye_received_total", NB_METRIC_SCOPE_UDP_RTCP),
    METRIC(rtcp_rtt_ms, NB_METRIC_TYPE_GAUGE, "ms", "rtcp_rtt_ms", NB_METRIC_SCOPE_UDP_RTCP),
    METRIC(rtcp_jitter_ms, NB_METRIC_TYPE_GAUGE, "ms", "rtcp_jitter_ms", NB_METRIC_SCOPE_UDP_RTCP),
    METRIC(rtcp_remote_lost, NB_METRIC_TYPE_GAUGE, "packets", "rtcp_remote_lost", NB_METRIC_SCOPE_UDP_RTCP),
    METRIC(connections, NB_METRIC_TYPE_GAUGE, "connections", "connections", NB_METRIC_SCOPE_NODE),
    METRIC(inflight_requests, NB_METRIC_TYPE_GAUGE, "requests", "inflight_requests", NB_METRIC_SCOPE_NODE),
    METRIC(inflight_handlers, NB_METRIC_TYPE_GAUGE, "tasks", "inflight_handlers", NB_METRIC_SCOPE_NODE),
    METRIC(latency_count, NB_METRIC_TYPE_COUNTER, "samples", "latency_count_total", NB_METRIC_SCOPE_NODE),
    METRIC(latency_sum_ms, NB_METRIC_TYPE_COUNTER, "ms", "latency_sum_ms_total", NB_METRIC_SCOPE_NODE),
    METRIC(latency_hist, NB_METRIC_TYPE_HISTOGRAM, "ms", "latency_ms", NB_METRIC_SCOPE_NODE),
    METRIC(latency_p50_ms, NB_METRIC_TYPE_GAUGE, "ms", "latency_p50_ms", NB_METRIC_SCOPE_NODE),
    METRIC(latency_p90_ms, NB_METRIC_TYPE_GAUGE, "ms", "latency_p90_ms", NB_METRIC_SCOPE_NODE),
    METRIC(latency_p99_ms, NB_METRIC_TYPE_GAUGE, "ms", "latency_p99_ms", NB_METRIC_SCOPE_NODE),
    METRIC(rate_limited, NB_METRIC_TYPE_COUNTER, "requests", "rate_limited_total", NB_METRIC_SCOPE_NODE),
    METRIC(streams_rejected, NB_METRIC_TYPE_COUNTER, "streams", "streams_rejected_total", NB_METRIC_SCOPE_NODE),
    METRIC(rtps_packets_sent, NB_METRIC_TYPE_COUNTER, "packets", "rtps_packets_sent_total", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_packets_received, NB_METRIC_TYPE_COUNTER, "packets", "rtps_packets_received_total", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_bytes_sent, NB_METRIC_TYPE_COUNTER, "bytes", "rtps_bytes_sent_total", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_bytes_received, NB_METRIC_TYPE_COUNTER, "bytes", "rtps_bytes_received_total", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_spdp_sent, NB_METRIC_TYPE_COUNTER, "samples", "rtps_spdp_sent_total", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_spdp_received, NB_METRIC_TYPE_COUNTER, "samples", "rtps_spdp_received_total", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_sedp_sent, NB_METRIC_TYPE_COUNTER, "samples", "rtps_sedp_sent_total", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_sedp_received, NB_METRIC_TYPE_COUNTER, "samples", "rtps_sedp_received_total", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_data_sent, NB_METRIC_TYPE_COUNTER, "samples", "rtps_data_sent_total", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_data_received, NB_METRIC_TYPE_COUNTER, "samples", "rtps_data_received_total", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_heartbeats_sent, NB_METRIC_TYPE_COUNTER, "packets", "rtps_heartbeats_sent_total", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_heartbeats_received, NB_METRIC_TYPE_COUNTER, "packets", "rtps_heartbeats_received_total", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_acknacks_sent, NB_METRIC_TYPE_COUNTER, "packets", "rtps_acknacks_sent_total", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_acknacks_received, NB_METRIC_TYPE_COUNTER, "packets", "rtps_acknacks_received_total", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_gaps_sent, NB_METRIC_TYPE_COUNTER, "packets", "rtps_gaps_sent_total", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_gaps_received, NB_METRIC_TYPE_COUNTER, "packets", "rtps_gaps_received_total", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_fragments_sent, NB_METRIC_TYPE_COUNTER, "fragments", "rtps_fragments_sent_total", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_fragments_received, NB_METRIC_TYPE_COUNTER, "fragments", "rtps_fragments_received_total", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_nack_frags_sent, NB_METRIC_TYPE_COUNTER, "packets", "rtps_nack_frags_sent_total", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_nack_frags_received, NB_METRIC_TYPE_COUNTER, "packets", "rtps_nack_frags_received_total", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_parse_errors, NB_METRIC_TYPE_COUNTER, "packets", "rtps_parse_errors_total", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_samples_dropped, NB_METRIC_TYPE_COUNTER, "samples", "rtps_samples_dropped_total", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_lease_expired, NB_METRIC_TYPE_COUNTER, "participants", "rtps_lease_expired_total", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_participants, NB_METRIC_TYPE_GAUGE, "participants", "rtps_participants", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_publications, NB_METRIC_TYPE_GAUGE, "endpoints", "rtps_publications", NB_METRIC_SCOPE_RTPS),
    METRIC(rtps_subscriptions, NB_METRIC_TYPE_GAUGE, "endpoints", "rtps_subscriptions", NB_METRIC_SCOPE_RTPS),
};

const size_t nb_metric_table_count = sizeof(nb_metric_table) / sizeof(nb_metric_table[0]);

const nb_metric_desc_t *nb_metric_find(const char *name)
{
    size_t i;

    if (!name) {
        return NULL;
    }
    for (i = 0; i < nb_metric_table_count; i++) {
        if (strcmp(nb_metric_table[i].name, name) == 0) {
            return &nb_metric_table[i];
        }
    }
    return NULL;
}

size_t nb_metric_latency_bounds(const uint32_t **bounds)
{
    if (bounds) {
        *bounds = nb_latency_bounds;
    }
    return NB_LATENCY_HIST_BUCKETS - 1;
}
