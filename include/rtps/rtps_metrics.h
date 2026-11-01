#ifndef RTPS_METRICS_H
#define RTPS_METRICS_H

/*
 * Cumulative counters and current gauges of one participant. Values are read
 * without stopping the participant and may be sampled at any time.
 */

#include <stdint.h>

#include "rtps_version.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rtps_metrics {
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
    /* Gauges. */
    uint64_t participants;
    uint64_t publications;
    uint64_t subscriptions;
} rtps_metrics_t;

#ifdef __cplusplus
}
#endif

#endif /* RTPS_METRICS_H */
