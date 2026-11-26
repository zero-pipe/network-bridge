/*
 * Bulk-publish throughput probe (same-host Linux), companion to dds_bench.c.
 *
 * Same participants, reader and metrics as dds_bench, but the writer drives
 * nb_dds_writer_write_batch() with a batch of K samples per call instead of one
 * write per sample. Small samples are packed into fewer datagrams and flushed
 * with a single sendmmsg, so throughput at K > 1 exposes the bulk path.
 *
 * Usage: dds_bench_batch [duration-seconds=60] [payload-bytes=1024]
 *                        [domain=97] [port-base=48600] [batch=16]
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

#define HIST_BINS 1000u
#define BIN_NS 10000ull
#define WARMUP_MS 3000u
#define DRAIN_MS 1000u
#define MAX_PAYLOAD (1024u * 1024u)
#define MAX_BATCH 256u

typedef struct stats {
    zb_mutex *lock;
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t received;
    uint64_t sum_latency_ns;
    uint64_t min_latency_ns;
    uint64_t max_latency_ns;
    uint64_t histogram[HIST_BINS];
    uint64_t overflow;
    uint64_t malformed;
} stats_t;

static uint64_t mono_ns(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
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
    stats_t *st = (stats_t *)user;
    const uint8_t *data;
    uint64_t sent_ns, now_ns, latency, bin;

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
    now_ns = mono_ns();
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
        st->sum_latency_ns += latency;
        if (st->received == 1 || latency < st->min_latency_ns) {
            st->min_latency_ns = latency;
        }
        if (latency > st->max_latency_ns) {
            st->max_latency_ns = latency;
        }
        bin = latency / BIN_NS;
        if (bin >= HIST_BINS) {
            st->overflow++;
        } else {
            st->histogram[bin]++;
        }
    }
    zb_mutex_unlock(st->lock);
}

static uint64_t percentile_us(const stats_t *st, double q)
{
    uint64_t rank, cum = 0;
    unsigned i;

    if (st->received == 0) {
        return 0;
    }
    rank = (uint64_t)((double)st->received * q);
    if (rank == 0) {
        rank = 1;
    }
    for (i = 0; i < HIST_BINS; i++) {
        cum += st->histogram[i];
        if (cum >= rank) {
            return ((uint64_t)i * BIN_NS) / 1000ull;
        }
    }
    return st->max_latency_ns / 1000ull;
}

static int make_participant(uint32_t domain, uint16_t spdp_port, uint16_t unicast_port,
                            const char *peer, nb_dds_participant_t **out)
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

int main(int argc, char **argv)
{
    unsigned duration_s = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 60u;
    size_t payload_size = argc > 2 ? (size_t)strtoul(argv[2], NULL, 10) : 1024u;
    uint32_t domain = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 97u;
    uint16_t port = argc > 4 ? (uint16_t)strtoul(argv[4], NULL, 10) : 48600u;
    size_t batch = argc > 5 ? (size_t)strtoul(argv[5], NULL, 10) : 16u;
    nb_dds_participant_t *pub = NULL, *sub = NULL;
    nb_dds_topic_t *pub_topic = NULL, *sub_topic = NULL;
    nb_dds_writer_t *writer = NULL;
    nb_dds_reader_t *reader = NULL;
    nb_dds_qos_t qos;
    stats_t stats;
    uint8_t *bufs[MAX_BATCH];
    const void *ptrs[MAX_BATCH];
    size_t sizes[MAX_BATCH];
    uint64_t writes = 0, warm_end, start_ns, end_ns, seq = 0;
    size_t i;
    int rc = 1;
    char peer_pub[64], peer_sub[64];

    if (duration_s == 0 || payload_size < 16u || payload_size > MAX_PAYLOAD ||
        port > 65531u || batch == 0) {
        fprintf(stderr, "usage: %s [dur] [bytes] [domain] [port] [batch]\n", argv[0]);
        return 2;
    }
    if (batch > MAX_BATCH) {
        batch = MAX_BATCH;
    }
    memset(&stats, 0, sizeof(stats));
    stats.lock = zb_mutex_create(ZB_MUTEX_NORMAL);
    for (i = 0; i < batch; i++) {
        size_t k;
        bufs[i] = (uint8_t *)calloc(payload_size, 1);
        if (!bufs[i]) {
            goto done;
        }
        for (k = 16; k < payload_size; k++) {
            bufs[i][k] = (uint8_t)(k * 31u + 7u);
        }
        ptrs[i] = bufs[i];
        sizes[i] = payload_size;
    }

    snprintf(peer_pub, sizeof(peer_pub), "127.0.0.1:%u", (unsigned)(port + 0u));
    snprintf(peer_sub, sizeof(peer_sub), "127.0.0.1:%u", (unsigned)(port + 2u));
    if (make_participant(domain, port, (uint16_t)(port + 1u), peer_sub, &pub) != NB_OK) {
        goto done;
    }
    if (make_participant(domain, (uint16_t)(port + 2u), (uint16_t)(port + 3u), peer_pub, &sub) !=
        NB_OK) {
        goto done;
    }
    nb_dds_qos_init(&qos);
    nb_dds_qos_set_reliability(&qos, NB_DDS_RELIABILITY_BEST_EFFORT);
    nb_dds_qos_set_history(&qos, NB_DDS_HISTORY_KEEP_LAST, 1);
    if (nb_dds_topic_create(pub, "dds_bench_1k", NULL, &qos, &pub_topic) != NB_OK ||
        nb_dds_topic_create(sub, "dds_bench_1k", NULL, &qos, &sub_topic) != NB_OK ||
        nb_dds_reader_create(sub_topic, &qos, on_sample, &stats, &reader) != NB_OK ||
        nb_dds_writer_create(pub_topic, &qos, &writer) != NB_OK) {
        fprintf(stderr, "setup failed\n");
        goto done;
    }

    /* Warmup (also discovery): fill timestamps, batch-publish, count nothing. */
    warm_end = mono_ns() + (uint64_t)WARMUP_MS * 1000000ull;
    while (mono_ns() < warm_end) {
        for (i = 0; i < batch; i++) {
            put_u64_le(bufs[i], seq++);
            put_u64_le(bufs[i] + 8, mono_ns());
        }
        if (batch > 1) {
            (void)nb_dds_writer_write_batch(writer, ptrs, sizes, batch);
        } else {
            (void)nb_dds_writer_write(writer, bufs[0], payload_size);
        }
        writes += batch;
    }

    start_ns = mono_ns();
    end_ns = start_ns + (uint64_t)duration_s * 1000000000ull;
    zb_mutex_lock(stats.lock);
    stats.start_ns = start_ns;
    stats.end_ns = end_ns;
    stats.received = 0;
    stats.sum_latency_ns = 0;
    stats.min_latency_ns = 0;
    stats.max_latency_ns = 0;
    stats.overflow = 0;
    stats.malformed = 0;
    memset(stats.histogram, 0, sizeof(stats.histogram));
    zb_mutex_unlock(stats.lock);

    writes = 0;
    while (mono_ns() < end_ns) {
        for (i = 0; i < batch; i++) {
            put_u64_le(bufs[i], seq++);
            put_u64_le(bufs[i] + 8, mono_ns());
        }
        if (batch > 1) {
            if (nb_dds_writer_write_batch(writer, ptrs, sizes, batch) == NB_OK) {
                writes += batch;
            }
        } else if (nb_dds_writer_write(writer, bufs[0], payload_size) == NB_OK) {
            writes++;
        }
    }
    zn_sleep_ms(DRAIN_MS);

    {
        double dur = (double)duration_s;
        double avg_us =
            stats.received ? (double)stats.sum_latency_ns / (double)stats.received / 1000.0 : 0.0;
        printf("DDS_BACKEND=RTPS_BATCH\n");
        printf("PAYLOAD_BYTES=%zu\n", payload_size);
        printf("BATCH_SIZE=%zu\n", batch);
        printf("WRITES=%" PRIu64 "\n", writes);
        printf("RECEIVED=%" PRIu64 "\n", stats.received);
        printf("DROPPED=%" PRIu64 "\n", writes > stats.received ? writes - stats.received : 0);
        printf("RECEIVED_QPS=%.2f\n", dur > 0 ? (double)stats.received / dur : 0.0);
        printf("LATENCY_US_AVG=%.2f\n", avg_us);
        printf("LATENCY_US_P50=%" PRIu64 "\n", percentile_us(&stats, 0.5));
        printf("LATENCY_US_P99=%" PRIu64 "\n", percentile_us(&stats, 0.99));
        printf("LATENCY_US_MAX=%.2f\n", (double)stats.max_latency_ns / 1000.0);
        printf("MALFORMED_SAMPLES=%" PRIu64 "\n", stats.malformed);
        rc = stats.received > 0 ? 0 : 1;
    }

done:
    nb_dds_reader_destroy(reader);
    nb_dds_writer_destroy(writer);
    nb_dds_topic_destroy(sub_topic);
    nb_dds_topic_destroy(pub_topic);
    nb_dds_participant_destroy(sub);
    nb_dds_participant_destroy(pub);
    for (i = 0; i < batch; i++) {
        free(bufs[i]);
    }
    if (stats.lock) {
        zb_mutex_destroy(stats.lock);
    }
    return rc;
}
