/*
 * DDS/RTPS one-way throughput and latency probe (same-host Linux benchmark).
 *
 * One process owns two RTPS participants joined by explicit unicast SPDP peers.
 * A best-effort writer sends fixed-size samples; a callback reader records
 * receiver QPS and one-way write-to-callback latency using the shared host
 * monotonic clock. This measures DDS/RTPS delivery, not RPC round-trip latency.
 *
 * Usage: dds_bench [duration-seconds=60] [payload-bytes=1024]
 *                   [domain=97] [port-base=48600]
 */
#define _POSIX_C_SOURCE 200809L

#include <nb/dds.h>
#include <nb/base/log.h>

#include <zn/zn.h>
#include <zb/os/sync.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DDS_BENCH_HIST_BINS 1000u
#define DDS_BENCH_BIN_NS 10000ull /* 10 us */
#define DDS_BENCH_WARMUP_MS 3000u
#define DDS_BENCH_DRAIN_MS 1000u
#define DDS_BENCH_MAX_PAYLOAD (1024u * 1024u)

typedef struct dds_bench_stats {
    zb_mutex *lock;
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t received;
    uint64_t bytes;
    uint64_t sum_latency_ns;
    uint64_t min_latency_ns;
    uint64_t max_latency_ns;
    uint64_t histogram[DDS_BENCH_HIST_BINS];
    uint64_t overflow;
    uint64_t malformed;
} dds_bench_stats_t;

static uint64_t monotonic_ns(void)
{
#if defined(_WIN32)
    /* Windows fallback is millisecond resolution; the reported run is Linux. */
    return zn_monotonic_ms() * 1000000ull;
#else
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static void put_u64_le(uint8_t *p, uint64_t v)
{
    unsigned i;
    for (i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

static uint64_t get_u64_le(const uint8_t *p)
{
    uint64_t v = 0;
    unsigned i;
    for (i = 0; i < 8; i++) {
        v |= (uint64_t)p[i] << (8u * i);
    }
    return v;
}

static void on_sample(void *user, const nb_dds_sample_t *sample)
{
    dds_bench_stats_t *st = (dds_bench_stats_t *)user;
    const uint8_t *data;
    uint64_t sent_ns;
    uint64_t now_ns;
    uint64_t latency;
    uint64_t bin;

    if (!st || !sample || !sample->data.data || sample->data.len < 16u) {
        if (st) {
            zb_mutex_lock(st->lock);
            st->malformed++;
            zb_mutex_unlock(st->lock);
        }
        return;
    }
    data = (const uint8_t *)sample->data.data;
    sent_ns = get_u64_le(data + 8);
    now_ns = monotonic_ns();
    if (sent_ns == 0 || now_ns < sent_ns) {
        zb_mutex_lock(st->lock);
        st->malformed++;
        zb_mutex_unlock(st->lock);
        return;
    }
    latency = now_ns - sent_ns;

    zb_mutex_lock(st->lock);
    if (sent_ns >= st->start_ns && sent_ns < st->end_ns) {
        st->received++;
        st->bytes += sample->data.len;
        st->sum_latency_ns += latency;
        if (st->received == 1 || latency < st->min_latency_ns) {
            st->min_latency_ns = latency;
        }
        if (latency > st->max_latency_ns) {
            st->max_latency_ns = latency;
        }
        bin = latency / DDS_BENCH_BIN_NS;
        if (bin >= DDS_BENCH_HIST_BINS) {
            st->overflow++;
        } else {
            st->histogram[bin]++;
        }
    }
    zb_mutex_unlock(st->lock);
}

static uint64_t percentile_us(const dds_bench_stats_t *st, double q)
{
    uint64_t rank;
    uint64_t cumulative = 0;
    unsigned i;

    if (st->received == 0) {
        return 0;
    }
    rank = (uint64_t)((double)st->received * q);
    if (rank == 0) {
        rank = 1;
    }
    for (i = 0; i < DDS_BENCH_HIST_BINS; i++) {
        cumulative += st->histogram[i];
        if (cumulative >= rank) {
            return ((uint64_t)i * DDS_BENCH_BIN_NS) / 1000ull;
        }
    }
    return st->max_latency_ns / 1000ull;
}

static int make_participant(uint32_t domain,
                            uint16_t spdp_port,
                            uint16_t unicast_port,
                            const char *peer,
                            nb_dds_participant_t **out)
{
    nb_dds_rtps_options_t rtps;
    nb_dds_participant_options_t options;
    const char *peers[1];

    memset(&rtps, 0, sizeof(rtps));
    rtps.struct_size = (uint32_t)sizeof(rtps);
    rtps.bind_host = "127.0.0.1";
    rtps.advertise_host = "127.0.0.1";
    rtps.multicast_group = "";
    rtps.spdp_port = spdp_port;
    rtps.unicast_port = unicast_port;
    rtps.announce_interval_ms = 500;
    rtps.lease_ms = 5000;
    peers[0] = peer;
    rtps.peers = peers;
    rtps.peer_count = 1;

    memset(&options, 0, sizeof(options));
    options.domain = domain;
    options.prefix = "dds-bench";
    options.name = "dds-bench";
    options.rtps = &rtps;
    return nb_dds_participant_create(&options, out);
}

static void print_result(const dds_bench_stats_t *st,
                         uint64_t writes,
                         size_t payload_size,
                         double duration_s)
{
    double delivered_qps = duration_s > 0.0 ? (double)st->received / duration_s : 0.0;
    double offered_qps = duration_s > 0.0 ? (double)writes / duration_s : 0.0;
    double avg_us = st->received
                        ? (double)st->sum_latency_ns / (double)st->received / 1000.0
                        : 0.0;

    printf("DDS_BACKEND=RTPS\n");
    printf("LATENCY_KIND=one_way_write_to_reader_callback\n");
    printf("DURATION_SECONDS=%.3f\n", duration_s);
    printf("PAYLOAD_BYTES=%zu\n", payload_size);
    printf("WRITES=%" PRIu64 "\n", writes);
    printf("RECEIVED=%" PRIu64 "\n", st->received);
    printf("DROPPED=%" PRIu64 "\n", writes > st->received ? writes - st->received : 0);
    printf("OFFERED_QPS=%.2f\n", offered_qps);
    printf("RECEIVED_QPS=%.2f\n", delivered_qps);
    printf("LATENCY_US_MIN=%.2f\n", (double)st->min_latency_ns / 1000.0);
    printf("LATENCY_US_AVG=%.2f\n", avg_us);
    printf("LATENCY_US_P50=%" PRIu64 "\n", percentile_us(st, 0.50));
    printf("LATENCY_US_P95=%" PRIu64 "\n", percentile_us(st, 0.95));
    printf("LATENCY_US_P99=%" PRIu64 "\n", percentile_us(st, 0.99));
    printf("LATENCY_US_MAX=%.2f\n", (double)st->max_latency_ns / 1000.0);
    printf("LATENCY_OVER_10MS=%" PRIu64 "\n", st->overflow);
    printf("MALFORMED_SAMPLES=%" PRIu64 "\n", st->malformed);
}

int main(int argc, char **argv)
{
    unsigned duration_s = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 60u;
    size_t payload_size = argc > 2 ? (size_t)strtoul(argv[2], NULL, 10) : 1024u;
    uint32_t domain = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 97u;
    uint16_t port = argc > 4 ? (uint16_t)strtoul(argv[4], NULL, 10) : 48600u;
    nb_dds_participant_t *pub = NULL;
    nb_dds_participant_t *sub = NULL;
    nb_dds_topic_t *pub_topic = NULL;
    nb_dds_topic_t *sub_topic = NULL;
    nb_dds_writer_t *writer = NULL;
    nb_dds_reader_t *reader = NULL;
    nb_dds_qos_t qos;
    dds_bench_stats_t stats;
    uint8_t *payload = NULL;
    uint64_t writes = 0;
    uint64_t warmup_end;
    uint64_t start_ns;
    uint64_t end_ns;
    size_t i;
    int rc = 1;
    char peer_pub[64];
    char peer_sub[64];

    if (duration_s == 0 || payload_size < 16u || payload_size > DDS_BENCH_MAX_PAYLOAD ||
        port > 65531u) {
        fprintf(stderr, "usage: %s [duration-seconds=60] [payload-bytes=1024] "
                        "[domain=97] [port-base=48600]\n", argv[0]);
        return 2;
    }
    memset(&stats, 0, sizeof(stats));
    stats.lock = zb_mutex_create(ZB_MUTEX_NORMAL);
    payload = (uint8_t *)calloc(payload_size, 1);
    if (!stats.lock || !payload) {
        fprintf(stderr, "allocation failed\n");
        goto done;
    }
    for (i = 16; i < payload_size; i++) {
        payload[i] = (uint8_t)(i * 31u + 7u);
    }

    snprintf(peer_pub, sizeof(peer_pub), "127.0.0.1:%u", (unsigned)(port + 0u));
    snprintf(peer_sub, sizeof(peer_sub), "127.0.0.1:%u", (unsigned)(port + 2u));
    rc = make_participant(domain, port, (uint16_t)(port + 1u), peer_sub, &pub);
    if (rc != NB_OK) {
        fprintf(stderr, "publisher participant create failed: %d\n", rc);
        rc = 1;
        goto done;
    }
    rc = make_participant(domain, (uint16_t)(port + 2u), (uint16_t)(port + 3u),
                          peer_pub, &sub);
    if (rc != NB_OK) {
        fprintf(stderr, "subscriber participant create failed: %d\n", rc);
        rc = 1;
        goto done;
    }

    nb_dds_qos_init(&qos);
    nb_dds_qos_set_reliability(&qos, NB_DDS_RELIABILITY_BEST_EFFORT);
    nb_dds_qos_set_history(&qos, NB_DDS_HISTORY_KEEP_LAST, 1);
    rc = nb_dds_topic_create(pub, "dds_bench_1k", NULL, &qos, &pub_topic);
    if (rc == NB_OK) {
        rc = nb_dds_topic_create(sub, "dds_bench_1k", NULL, &qos, &sub_topic);
    }
    if (rc == NB_OK) {
        rc = nb_dds_reader_create(sub_topic, &qos, on_sample, &stats, &reader);
    }
    if (rc == NB_OK) {
        rc = nb_dds_writer_create(pub_topic, &qos, &writer);
    }
    if (rc != NB_OK) {
        fprintf(stderr, "topic/reader/writer setup failed: %d\n", rc);
        rc = 1;
        goto done;
    }

    /* Warm up discovery/matching; exclude these samples from measurement. */
    warmup_end = monotonic_ns() + (uint64_t)DDS_BENCH_WARMUP_MS * 1000000ull;
    while (monotonic_ns() < warmup_end) {
        put_u64_le(payload, writes);
        put_u64_le(payload + 8, monotonic_ns());
        (void)nb_dds_writer_write(writer, payload, payload_size);
        writes++;
    }

    start_ns = monotonic_ns();
    end_ns = start_ns + (uint64_t)duration_s * 1000000000ull;
    zb_mutex_lock(stats.lock);
    stats.start_ns = start_ns;
    stats.end_ns = end_ns;
    stats.received = 0;
    stats.bytes = 0;
    stats.sum_latency_ns = 0;
    stats.min_latency_ns = 0;
    stats.max_latency_ns = 0;
    stats.overflow = 0;
    stats.malformed = 0;
    memset(stats.histogram, 0, sizeof(stats.histogram));
    zb_mutex_unlock(stats.lock);

    writes = 0;
    while (monotonic_ns() < end_ns) {
        put_u64_le(payload, writes);
        put_u64_le(payload + 8, monotonic_ns());
        if (nb_dds_writer_write(writer, payload, payload_size) == NB_OK) {
            writes++;
        }
    }
    zn_sleep_ms(DDS_BENCH_DRAIN_MS);

    zb_mutex_lock(stats.lock);
    print_result(&stats, writes, payload_size, (double)duration_s);
    zb_mutex_unlock(stats.lock);
    rc = stats.received > 0 ? 0 : 1;

done:
    nb_dds_reader_destroy(reader);
    nb_dds_writer_destroy(writer);
    nb_dds_topic_destroy(sub_topic);
    nb_dds_topic_destroy(pub_topic);
    nb_dds_participant_destroy(sub);
    nb_dds_participant_destroy(pub);
    free(payload);
    if (stats.lock) {
        zb_mutex_destroy(stats.lock);
    }
    return rc;
}
