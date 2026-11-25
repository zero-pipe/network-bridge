/*
 * DDS/RTPS request-response round-trip benchmark (same-host Linux).
 *
 * Participant A writes 1 KiB to request_topic. Participant B receives it in a
 * reader callback and echoes the same sample to response_topic. A's response
 * reader signals one semaphore per completed round trip. Window = 1, matching
 * the RPC gate's sequential request measurement.
 *
 * This is DDS application-level ping-pong, not the RPC stack.
 * Usage: dds_rtt_bench [duration-seconds=60] [payload-bytes=1024]
 *                       [domain=103] [port-base=48700]
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

#define RTT_HIST_BINS 1000u
#define RTT_BIN_NS 10000ull /* 10 us */
#define RTT_WARMUP_REQUESTS 100u
#define RTT_REQUEST_TIMEOUT_MS 2000u
#define RTT_MAX_PAYLOAD (1024u * 1024u)

typedef struct rtt_state {
    zb_mutex *lock;
    zb_sem *sem;
    nb_dds_writer_t *response_writer; /* server-side echo writer */
    uint64_t expected_seq;
    uint64_t sent_ns;
    int measuring;
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t warmup_completed;
    uint64_t warmup_timed_out;
    uint64_t completed;
    uint64_t timed_out;
    uint64_t write_failed;
    uint64_t sum_ns;
    uint64_t min_ns;
    uint64_t max_ns;
    uint64_t histogram[RTT_HIST_BINS];
    uint64_t overflow;
} rtt_state_t;

static uint64_t mono_ns(void)
{
#if defined(_WIN32)
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

/* B's reader callback: echo the request to the response topic. */
static void on_request(void *user, const nb_dds_sample_t *sample)
{
    rtt_state_t *st = (rtt_state_t *)user;

    if (!st || !sample || !sample->data.data || sample->data.len < 16u ||
        !st->response_writer) {
        return;
    }
    (void)nb_dds_writer_write(st->response_writer, sample->data.data, sample->data.len);
}

/* A's response callback: accept only the response matching the one in flight. */
static void on_response(void *user, const nb_dds_sample_t *sample)
{
    rtt_state_t *st = (rtt_state_t *)user;
    const uint8_t *data;
    uint64_t seq;
    uint64_t now;
    uint64_t rtt;
    uint64_t bin;
    int matched = 0;

    if (!st || !sample || !sample->data.data || sample->data.len < 16u) {
        return;
    }
    data = (const uint8_t *)sample->data.data;
    seq = get_u64_le(data);
    now = mono_ns();

    zb_mutex_lock(st->lock);
    if (seq != st->expected_seq || st->sent_ns == 0 || now < st->sent_ns) {
        zb_mutex_unlock(st->lock);
        return; /* late/duplicate response, or not the current measured call */
    }
    rtt = now - st->sent_ns;
    if (!st->start_ns) {
        st->warmup_completed++;
    } else if (st->sent_ns < st->start_ns || st->sent_ns >= st->end_ns) {
        st->sent_ns = 0;
        matched = 1;
        goto unlock;
    }
    st->sent_ns = 0; /* Consume this sequence before another callback can match. */
    if (!st->start_ns) {
        matched = 1;
        goto unlock;
    }
    st->completed++;
    st->sum_ns += rtt;
    if (st->completed == 1 || rtt < st->min_ns) {
        st->min_ns = rtt;
    }
    if (rtt > st->max_ns) {
        st->max_ns = rtt;
    }
    bin = rtt / RTT_BIN_NS;
    if (bin < RTT_HIST_BINS) {
        st->histogram[bin]++;
    } else {
        st->overflow++;
    }
    matched = 1;
unlock:
    zb_mutex_unlock(st->lock);
    if (matched) {
        zb_sem_post(st->sem, 1);
    }
}

static int make_participant(uint32_t domain,
                            uint16_t spdp_port,
                            uint16_t unicast_port,
                            const char *peer,
                            const char *name,
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
    rtps.announce_interval_ms = 100;
    rtps.lease_ms = 3000;
    peers[0] = peer;
    rtps.peers = peers;
    rtps.peer_count = 1;

    memset(&options, 0, sizeof(options));
    options.backend = NB_DDS_BACKEND_RTPS;
    options.domain = domain;
    options.prefix = "dds-rtt";
    options.name = name;
    options.rtps = &rtps;
    return nb_dds_participant_create(&options, out);
}

static int create_topic_pair(nb_dds_participant_t *a,
                             nb_dds_participant_t *b,
                             const char *name,
                             const nb_dds_qos_t *qos,
                             nb_dds_topic_t **ta,
                             nb_dds_topic_t **tb)
{
    int rc = nb_dds_topic_create(a, name, NULL, qos, ta);
    if (rc != NB_OK) {
        return rc;
    }
    return nb_dds_topic_create(b, name, NULL, qos, tb);
}

static uint64_t percentile_us(const rtt_state_t *st, double q)
{
    uint64_t rank;
    uint64_t cumulative = 0;
    unsigned i;

    if (!st->completed) {
        return 0;
    }
    rank = (uint64_t)((double)st->completed * q);
    if (rank == 0) {
        rank = 1;
    }
    for (i = 0; i < RTT_HIST_BINS; i++) {
        cumulative += st->histogram[i];
        if (cumulative >= rank) {
            return (uint64_t)i * RTT_BIN_NS / 1000ull;
        }
    }
    return st->max_ns / 1000ull;
}

int main(int argc, char **argv)
{
    unsigned duration_s = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 60u;
    size_t payload_size = argc > 2 ? (size_t)strtoul(argv[2], NULL, 10) : 1024u;
    uint32_t domain = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 103u;
    uint16_t port = argc > 4 ? (uint16_t)strtoul(argv[4], NULL, 10) : 48700u;
    nb_dds_participant_t *a = NULL;
    nb_dds_participant_t *b = NULL;
    nb_dds_topic_t *req_a = NULL, *req_b = NULL;
    nb_dds_topic_t *resp_a = NULL, *resp_b = NULL;
    nb_dds_writer_t *req_writer = NULL, *resp_writer = NULL;
    nb_dds_reader_t *req_reader = NULL, *resp_reader = NULL;
    nb_dds_qos_t qos;
    rtt_state_t st;
    uint8_t *payload = NULL;
    uint64_t seq = 0;
    uint64_t started_ns;
    uint64_t deadline_ns;
    uint64_t measured_ns;
    uint64_t drain_seq;
    uint64_t i;
    int rc = 1;
    char peer_a[64], peer_b[64];

    if (!duration_s || payload_size < 16u || payload_size > RTT_MAX_PAYLOAD || port > 65531u) {
        fprintf(stderr, "usage: %s [duration-seconds=60] [payload-bytes=1024] "
                        "[domain=103] [port-base=48700]\n", argv[0]);
        return 2;
    }
    memset(&st, 0, sizeof(st));
    st.lock = zb_mutex_create(ZB_MUTEX_NORMAL);
    st.sem = zb_sem_create(0);
    payload = (uint8_t *)calloc(payload_size, 1);
    if (!st.lock || !st.sem || !payload) {
        fprintf(stderr, "allocation failed\n");
        goto done;
    }
    for (i = 16; i < payload_size; i++) {
        payload[i] = (uint8_t)(i * 29u + 3u);
    }

    snprintf(peer_a, sizeof(peer_a), "127.0.0.1:%u", (unsigned)port);
    snprintf(peer_b, sizeof(peer_b), "127.0.0.1:%u", (unsigned)(port + 2u));
    rc = make_participant(domain, port, (uint16_t)(port + 1u), peer_b, "dds-rtt-a", &a);
    if (rc != NB_OK) {
        fprintf(stderr, "participant A create failed: %d\n", rc);
        rc = 1;
        goto done;
    }
    rc = make_participant(domain, (uint16_t)(port + 2u), (uint16_t)(port + 3u),
                          peer_a, "dds-rtt-b", &b);
    if (rc != NB_OK) {
        fprintf(stderr, "participant B create failed: %d\n", rc);
        rc = 1;
        goto done;
    }

    nb_dds_qos_init(&qos);
    nb_dds_qos_set_reliability(&qos, NB_DDS_RELIABILITY_BEST_EFFORT);
    nb_dds_qos_set_history(&qos, NB_DDS_HISTORY_KEEP_LAST, 1);

    rc = create_topic_pair(a, b, "dds_rtt_request", &qos, &req_a, &req_b);
    if (rc == NB_OK) {
        rc = create_topic_pair(a, b, "dds_rtt_response", &qos, &resp_a, &resp_b);
    }
    if (rc == NB_OK) {
        rc = nb_dds_writer_create(a ? req_a : NULL, &qos, &req_writer);
    }
    if (rc == NB_OK) {
        rc = nb_dds_reader_create(req_b, &qos, on_request, &st, &req_reader);
    }
    if (rc == NB_OK) {
        rc = nb_dds_writer_create(resp_b, &qos, &resp_writer);
        st.response_writer = resp_writer;
    }
    if (rc == NB_OK) {
        rc = nb_dds_reader_create(resp_a, &qos, on_response, &st, &resp_reader);
    }
    if (rc != NB_OK) {
        fprintf(stderr, "DDS endpoints create failed: %d\n", rc);
        rc = 1;
        goto done;
    }

    /* Warm discovery/matching with a bounded request-response warm-up. */
    for (i = 0; i < RTT_WARMUP_REQUESTS; i++) {
        int write_rc;
        uint64_t warm_deadline = mono_ns() + 3000000000ull;
        uint64_t warm_seq = UINT64_MAX - i;
        put_u64_le(payload, warm_seq);
        zb_mutex_lock(st.lock);
        st.expected_seq = warm_seq;
        st.sent_ns = mono_ns();
        zb_mutex_unlock(st.lock);
        write_rc = nb_dds_writer_write(req_writer, payload, payload_size);
        if (write_rc == NB_OK) {
            while (mono_ns() < warm_deadline) {
                if (zb_sem_timedwait(st.sem, 100) == ZN_OK) {
                    break;
                }
            }
        }
        zb_mutex_lock(st.lock);
        if (st.sent_ns != 0) {
            st.warmup_timed_out++;
            st.sent_ns = 0;
        } else if (write_rc != NB_OK) {
            st.warmup_timed_out++;
        }
        zb_mutex_unlock(st.lock);
        while (zb_sem_timedwait(st.sem, 0) == ZN_OK) {
        }
    }
    /* Drain any warm-up response token before starting the measured window. */
    while (zb_sem_timedwait(st.sem, 1) == ZN_OK) {
    }

    zb_mutex_lock(st.lock);
    started_ns = mono_ns();
    deadline_ns = started_ns + (uint64_t)duration_s * 1000000000ull;
    st.start_ns = started_ns;
    st.end_ns = deadline_ns;
    st.measuring = 1;
    zb_mutex_unlock(st.lock);

    while (mono_ns() < deadline_ns) {
        uint64_t current_seq = ++seq;
        uint64_t sent_ns = mono_ns();
        uint64_t remaining_ms;

        if (sent_ns >= deadline_ns) {
            break;
        }
        put_u64_le(payload, current_seq);
        put_u64_le(payload + 8, sent_ns);
        zb_mutex_lock(st.lock);
        st.expected_seq = current_seq;
        st.sent_ns = sent_ns;
        zb_mutex_unlock(st.lock);

        if (nb_dds_writer_write(req_writer, payload, payload_size) != NB_OK) {
            zb_mutex_lock(st.lock);
            st.write_failed++;
            st.sent_ns = 0;
            zb_mutex_unlock(st.lock);
            continue;
        }
        {
            uint64_t now_ns = mono_ns();
            remaining_ms = now_ns < deadline_ns ?
                (deadline_ns - now_ns + 999999ull) / 1000000ull : 0;
        }
        if (remaining_ms > RTT_REQUEST_TIMEOUT_MS) {
            remaining_ms = RTT_REQUEST_TIMEOUT_MS;
        }
        if (zb_sem_timedwait(st.sem, (unsigned)remaining_ms) != ZN_OK) {
            if (mono_ns() >= deadline_ns) {
                break; /* Preserve the final in-flight request for the drain. */
            }
            zb_mutex_lock(st.lock);
            if (st.sent_ns != 0) {
                st.timed_out++;
                st.sent_ns = 0;
            }
            zb_mutex_unlock(st.lock);
            while (zb_sem_timedwait(st.sem, 0) == ZN_OK) {
            }
        }
    }

    zb_mutex_lock(st.lock);
    st.end_ns = mono_ns();
    measured_ns = st.end_ns - started_ns;
    st.measuring = 0;
    drain_seq = st.sent_ns;
    zb_mutex_unlock(st.lock);
    if (drain_seq != 0) {
        (void)zb_sem_timedwait(st.sem, RTT_REQUEST_TIMEOUT_MS);
        zb_mutex_lock(st.lock);
        if (st.sent_ns != 0 && st.expected_seq == seq) {
            st.timed_out++;
            st.sent_ns = 0;
        }
        zb_mutex_unlock(st.lock);
        while (zb_sem_timedwait(st.sem, 0) == ZN_OK) {
        }
    }

    zb_mutex_lock(st.lock);
    {
        rtt_state_t result = st;
        double elapsed = (double)measured_ns / 1000000000.0;
        double qps;
        double avg_us;
        zb_mutex_unlock(st.lock);

        qps = elapsed > 0.0 ? (double)result.completed / elapsed : 0.0;
        avg_us = result.completed ?
            (double)result.sum_ns / (double)result.completed / 1000.0 : 0.0;
        printf("DDS_BACKEND=RTPS\n");
        printf("MODE=request_response_ping_pong\n");
        printf("LATENCY_KIND=round_trip_write_to_response_callback\n");
        printf("QOS=best_effort_keep_last_1\n");
        printf("TRANSPORT=same_host_unicast_loopback\n");
        printf("WINDOW=1\n");
        printf("DURATION_SECONDS=%.3f\n", elapsed);
        printf("MEASURED_SECONDS=%.6f\n", elapsed);
        printf("PAYLOAD_BYTES=%zu\n", payload_size);
        printf("WARMUP_COMPLETED=%" PRIu64 "\n", result.warmup_completed);
        printf("WARMUP_TIMED_OUT=%" PRIu64 "\n", result.warmup_timed_out);
        printf("COMPLETED=%" PRIu64 "\n", result.completed);
        printf("TIMED_OUT=%" PRIu64 "\n", result.timed_out);
        printf("WRITE_FAILED=%" PRIu64 "\n", result.write_failed);
        printf("QPS=%.2f\n", qps);
        printf("RTT_US_MIN=%.2f\n", (double)result.min_ns / 1000.0);
        printf("RTT_US_AVG=%.2f\n", avg_us);
        printf("RTT_US_P50=%" PRIu64 "\n", percentile_us(&result, 0.50));
        printf("RTT_US_P95=%" PRIu64 "\n", percentile_us(&result, 0.95));
        printf("RTT_US_P99=%" PRIu64 "\n", percentile_us(&result, 0.99));
        printf("RTT_US_MAX=%.2f\n", (double)result.max_ns / 1000.0);
        printf("RTT_OVER_10MS=%" PRIu64 "\n", result.overflow);
        rc = result.completed > 0 ? 0 : 1;
    }


done:
    nb_dds_reader_destroy(resp_reader);
    nb_dds_reader_destroy(req_reader);
    nb_dds_writer_destroy(resp_writer);
    nb_dds_writer_destroy(req_writer);
    nb_dds_topic_destroy(resp_a);
    nb_dds_topic_destroy(resp_b);
    nb_dds_topic_destroy(req_a);
    nb_dds_topic_destroy(req_b);
    nb_dds_participant_destroy(b);
    nb_dds_participant_destroy(a);
    free(payload);
    if (st.sem) {
        zb_sem_destroy(st.sem);
    }
    if (st.lock) {
        zb_mutex_destroy(st.lock);
    }
    return rc;
}
