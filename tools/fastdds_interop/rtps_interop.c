/*
 * RTPS <-> Fast DDS interop probe.
 *
 *   rtps_interop reader <seconds> [domain]
 *   rtps_interop writer <count> [message_size] [domain]
 *
 * Uses the same topic/type as the Fast DDS hello_world example:
 *   topic "hello_world_topic", type "HelloWorld"
 * CDR body: u32 index, u32 string-length (including NUL), chars, NUL, pad.
 */
#include <rtps/rtps.h>
#include <nb/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TOPIC_NAME "hello_world_topic"
#define TYPE_NAME "HelloWorld"
#define FASTDDS_METATRAFFIC_PORT 7410u

static volatile int g_received;
static volatile int g_ok;

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void on_data(void *user, rtps_reader_t *reader,
                    const rtps_sample_t *samples, size_t n)
{
    size_t i;
    (void)user;
    (void)reader;
    for (i = 0; i < n; i++) {
        const uint8_t *p = (const uint8_t *)samples[i].data;
        size_t len = samples[i].size;
        if (p && len >= 8) {
            uint32_t index = rd_u32(p);
            uint32_t slen = rd_u32(p + 4);
            printf("RECV size=%zu index=%u string_len=%u", len, index, slen);
            if (slen > 0 && 8u + slen <= len) {
                printf(" msg='%.*s'", (int)(slen - 1 > 48 ? 48 : slen - 1),
                       (const char *)(p + 8));
            }
            printf("\n");
            fflush(stdout);
            g_received++;
            if (slen > 0 && 8u + slen <= len) {
                g_ok = 1;
            }
        } else {
            printf("RECV short sample size=%zu\n", len);
            fflush(stdout);
            g_received++;
        }
    }
}

static int make_participant(uint32_t domain, rtps_participant_t **out)
{
    rtps_participant_config_t pc;
    rtps_participant_config_init(&pc);
    pc.domain_id = domain;
    pc.bind_host = "0.0.0.0";
    pc.advertise_host = getenv("RTPS_TEST_ADVERTISE") ? getenv("RTPS_TEST_ADVERTISE") : "127.0.0.1";
    pc.announce_interval_ms = 500;
    pc.name = "nb-rtps-interop";
    if (getenv("RTPS_TEST_NO_MULTICAST")) {
        pc.multicast_group = "";
    }
    if (getenv("RTPS_TEST_STANDARD_PORTS")) {
        pc.standard_ports = 1;
        pc.participant_id = getenv("RTPS_TEST_PARTICIPANT_ID")
                                ? (uint32_t)atoi(getenv("RTPS_TEST_PARTICIPANT_ID"))
                                : 10u;
    }
    if (getenv("RTPS_TEST_UNICAST_PORT")) {
        pc.unicast_port = (uint16_t)atoi(getenv("RTPS_TEST_UNICAST_PORT"));
    }
    if (rtps_participant_create(&pc, out) != RTPS_OK) {
        fprintf(stderr, "participant create failed\n");
        return -1;
    }
    (void)rtps_participant_add_peer(*out, "127.0.0.1", FASTDDS_METATRAFFIC_PORT);
    (void)rtps_participant_add_peer(*out, "127.0.0.1", FASTDDS_METATRAFFIC_PORT + 2);
    (void)rtps_participant_add_peer(*out, "127.0.0.1", FASTDDS_METATRAFFIC_PORT + 4);
    (void)rtps_participant_add_peer(*out, "127.0.0.1", FASTDDS_METATRAFFIC_PORT + 6);
    if (rtps_participant_start(*out) != RTPS_OK) {
        fprintf(stderr, "participant start failed\n");
        rtps_participant_destroy(*out);
        return -1;
    }
    return 0;
}

static int make_topic(rtps_participant_t *p, rtps_topic_t **out)
{
    rtps_topic_desc_t td;
    rtps_topic_desc_init(&td);
    td.name = TOPIC_NAME;
    td.type_name = TYPE_NAME;
    rtps_qos_set_reliability(&td.qos,
                             getenv("RTPS_TEST_RELIABLE")
                                 ? RTPS_RELIABILITY_RELIABLE
                                 : RTPS_RELIABILITY_BEST_EFFORT);
    rtps_qos_set_history(&td.qos, RTPS_HISTORY_KEEP_LAST, 8);
    if (getenv("RTPS_TEST_TRANSIENT_LOCAL")) {
        rtps_qos_set_durability(&td.qos, RTPS_DURABILITY_TRANSIENT_LOCAL);
    }
    if (rtps_topic_create(p, &td, out) != RTPS_OK) {
        fprintf(stderr, "topic create failed\n");
        return -1;
    }
    return 0;
}

static int run_reader(int seconds, uint32_t domain, int expect)
{
    rtps_participant_t *p = NULL;
    rtps_topic_t *topic = NULL;
    rtps_reader_t *reader = NULL;
    int waited;

    if (expect < 1) {
        expect = 1;
    }
    if (make_participant(domain, &p) != 0 || make_topic(p, &topic) != 0) {
        return 1;
    }
    if (rtps_reader_create(topic, NULL, &reader) != RTPS_OK) {
        fprintf(stderr, "reader create failed\n");
        rtps_participant_destroy(p);
        return 1;
    }
    rtps_reader_set_data_callback(reader, on_data, NULL);
    printf("READY reader topic=%s type=%s domain=%u expect=%d\n", TOPIC_NAME, TYPE_NAME, domain,
           expect);
    fflush(stdout);

    for (waited = 0; waited < seconds * 10; waited++) {
        usleep(100000);
        if (g_received >= expect) {
            break;
        }
    }
    printf("RESULT received=%d parsed=%d matched=%zu\n", g_received, g_ok,
           rtps_reader_matched_count(reader));
    rtps_participant_destroy(p);
    return g_received > 0 ? 0 : 2;
}

static size_t encode(const uint8_t *msg, size_t msg_len, uint8_t *out, size_t cap)
{
    size_t total = 8 + msg_len + 1;
    size_t padded = (total + 3u) & ~3u;
    if (padded > cap) {
        return 0;
    }
    memset(out, 0, padded);
    out[0] = 0; /* index low */
    out[1] = 0;
    out[2] = 0;
    out[3] = 0;
    {
        uint32_t slen = (uint32_t)(msg_len + 1);
        out[4] = (uint8_t)(slen & 0xff);
        out[5] = (uint8_t)((slen >> 8) & 0xff);
        out[6] = (uint8_t)((slen >> 16) & 0xff);
        out[7] = (uint8_t)((slen >> 24) & 0xff);
    }
    memcpy(out + 8, msg, msg_len);
    return padded;
}

static int run_writer(int count, size_t msg_len, uint32_t domain)
{
    rtps_participant_t *p = NULL;
    rtps_topic_t *topic = NULL;
    rtps_writer_t *writer = NULL;
    uint8_t *body;
    int i;
    int waited_ms = 0;

    if (make_participant(domain, &p) != 0 || make_topic(p, &topic) != 0) {
        return 1;
    }
    if (rtps_writer_create(topic, NULL, &writer) != RTPS_OK) {
        fprintf(stderr, "writer create failed\n");
        rtps_participant_destroy(p);
        return 1;
    }
    body = (uint8_t *)malloc(16 + msg_len);
    if (!body) {
        rtps_participant_destroy(p);
        return 1;
    }
    printf("READY writer topic=%s type=%s domain=%u\n", TOPIC_NAME, TYPE_NAME, domain);
    fflush(stdout);

    if (!getenv("RTPS_TEST_NO_WAIT")) {
        while (rtps_writer_matched_count(writer) == 0 && waited_ms < 30000) {
            usleep(100000);
            waited_ms += 100;
        }
        printf("matched=%zu after %d ms\n", rtps_writer_matched_count(writer), waited_ms);
        fflush(stdout);
        if (rtps_writer_matched_count(writer) == 0) {
            free(body);
            rtps_participant_destroy(p);
            return 3;
        }
    } else {
        printf("nowait: writing before any reader matches\n");
        fflush(stdout);
    }

    for (i = 0; i < count; i++) {
        char msg[128];
        size_t body_len;
        uint32_t index = (uint32_t)(i + 1);

        if (msg_len >= 64) {
            uint8_t *big = (uint8_t *)malloc(msg_len);
            if (!big) {
                break;
            }
            memset(big, 'A' + (i % 26), msg_len);
            memcpy(big, "hello-from-nb", 13);
            body_len = encode(big, msg_len, body, 16 + msg_len);
            free(big);
        } else {
            int n = snprintf(msg, sizeof(msg), "hello-from-nb-%u", index);
            body_len = encode((const uint8_t *)msg, (size_t)n, body, 16 + msg_len);
        }
        body[0] = (uint8_t)(index & 0xff);
        body[1] = (uint8_t)((index >> 8) & 0xff);
        body[2] = (uint8_t)((index >> 16) & 0xff);
        body[3] = (uint8_t)((index >> 24) & 0xff);
        if (rtps_writer_write(writer, body, body_len) != RTPS_OK) {
            fprintf(stderr, "write %u failed\n", index);
            break;
        }
        printf("SENT index=%u body=%zu\n", index, body_len);
        fflush(stdout);
        usleep(100000);
    }
    printf("RESULT sent=%d matched=%zu\n", i, rtps_writer_matched_count(writer));
    if (getenv("RTPS_TEST_NO_WAIT")) {
        sleep(15);
        printf("RESULT after late join matched=%zu\n", rtps_writer_matched_count(writer));
    }
    free(body);
    rtps_participant_destroy(p);
    return i > 0 ? 0 : 4;
}

int main(int argc, char **argv)
{
    if (getenv("NB_DEBUG")) {
        nb_log_configure(true, NB_LOG_LEVEL_DEBUG);
    }
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s reader <seconds> [domain] [expect]\n"
                "       %s writer <count> [message_size] [domain]\n",
                argv[0], argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "reader") == 0) {
        uint32_t domain = argc >= 4 ? (uint32_t)atoi(argv[3]) : 0;
        int expect = argc >= 5 ? atoi(argv[4]) : 1;
        return run_reader(atoi(argv[2]), domain, expect);
    }
    if (strcmp(argv[1], "writer") == 0) {
        int count = atoi(argv[2]);
        size_t msg_len = argc >= 4 ? (size_t)atoi(argv[3]) : 16;
        uint32_t domain = argc >= 5 ? (uint32_t)atoi(argv[4]) : 0;
        if (msg_len < 1) {
            msg_len = 16;
        }
        return run_writer(count, msg_len, domain);
    }
    fprintf(stderr, "unknown mode %s\n", argv[1]);
    return 1;
}
