/*
 * DDS spine (nb_dds_*) over the in-tree RTPS backend.
 *
 * Two participants on the same domain, unicast discovery only, one writer and
 * two readers (callback + history). Covers prefix naming, QoS propagation,
 * read/take, wait sets and discovery snapshots.
 */
#include <nb/dds.h>

#include <ntk.h>

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail = 1;                                                 \
        }                                                               \
    } while (0)

typedef struct rx_state {
    ntk_mutex *lock;
    int count;
    char last[64];
} rx_state_t;

static void on_sample(void *user, const nb_dds_sample_t *sample)
{
    rx_state_t *rx = (rx_state_t *)user;

    ntk_mutex_lock(rx->lock);
    rx->count++;
    if (sample->data.len < sizeof(rx->last)) {
        memcpy(rx->last, sample->data.data, sample->data.len);
        rx->last[sample->data.len] = '\0';
    }
    ntk_mutex_unlock(rx->lock);
}

static int rx_count(rx_state_t *rx)
{
    int count;

    ntk_mutex_lock(rx->lock);
    count = rx->count;
    ntk_mutex_unlock(rx->lock);
    return count;
}

static void make_rtps_options(nb_dds_rtps_options_t *rtps,
                              uint16_t spdp_port,
                              uint16_t unicast_port,
                              const char *const *peers,
                              size_t peer_count)
{
    memset(rtps, 0, sizeof(*rtps));
    rtps->struct_size = sizeof(*rtps);
    rtps->bind_host = "127.0.0.1";
    rtps->advertise_host = "127.0.0.1";
    rtps->multicast_group = "";
    rtps->spdp_port = spdp_port;
    rtps->unicast_port = unicast_port;
    rtps->announce_interval_ms = 200;
    rtps->lease_ms = 4000;
    rtps->peers = peers;
    rtps->peer_count = peer_count;
}

static int make_participant(uint16_t spdp_port,
                            uint16_t unicast_port,
                            const char *const *peers,
                            size_t peer_count,
                            nb_dds_backend_t backend,
                            nb_dds_participant_t **out)
{
    nb_dds_rtps_options_t rtps;
    nb_dds_participant_options_t options;

    make_rtps_options(&rtps, spdp_port, unicast_port, peers, peer_count);
    memset(&options, 0, sizeof(options));
    options.backend = backend;
    options.domain = backend == NB_DDS_BACKEND_CYCLONE ? 71u : 0u;
    options.prefix = "app";
    options.name = "dds-api-test";
    options.rtps = backend == NB_DDS_BACKEND_RTPS ? &rtps : NULL;
    return nb_dds_participant_create(&options, out);
}

static void make_qos(nb_dds_qos_t *qos)
{
    nb_dds_qos_init(qos);
    nb_dds_qos_set_reliability(qos, NB_DDS_RELIABILITY_RELIABLE);
    nb_dds_qos_set_history(qos, NB_DDS_HISTORY_KEEP_LAST, 4);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "rtps";
    nb_dds_backend_t backend =
        strcmp(mode, "cyclone") == 0 ? NB_DDS_BACKEND_CYCLONE : NB_DDS_BACKEND_RTPS;
    const char *p1_peers[] = {"127.0.0.1:48402"};
    const char *p2_peers[] = {"127.0.0.1:48400"};
    nb_dds_participant_t *p1 = NULL;
    nb_dds_participant_t *p2 = NULL;
    nb_dds_topic_t *pub_topic = NULL;
    nb_dds_topic_t *sub_topic = NULL;
    nb_dds_topic_t *dup = NULL;
    nb_dds_writer_t *writer = NULL;
    nb_dds_reader_t *history_reader = NULL;
    nb_dds_reader_t *callback_reader = NULL;
    nb_dds_waitset_t *waitset = NULL;
    nb_dds_qos_t qos;
    rx_state_t rx;
    nb_dds_sample_t samples[4];
    nb_dds_discovery_sample_t discovered[8];
    size_t taken;
    size_t discovered_count;
    int i;

    memset(&rx, 0, sizeof(rx));
    rx.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(rx.lock != NULL);
    if (!rx.lock) {
        return 1;
    }

    CHECK(make_participant(48400, 48401, p1_peers, 1, backend, &p1) == NB_OK);
    CHECK(make_participant(48402, 48403, p2_peers, 1, backend, &p2) == NB_OK);
    if (!p1 || !p2) {
        goto out;
    }
    CHECK(nb_dds_participant_backend(p1) == backend);
    CHECK(strcmp(nb_dds_participant_name(p1), "dds-api-test") == 0);

    make_qos(&qos);
    CHECK(nb_dds_topic_create(p1, "chat", "text", &qos, &pub_topic) == NB_OK);
    CHECK(nb_dds_topic_create(p2, "chat", "text", &qos, &sub_topic) == NB_OK);
    if (!pub_topic || !sub_topic) {
        goto out;
    }
    /* Same wire name on one participant is rejected instead of double-owned. */
    CHECK(nb_dds_topic_create(p2, "chat", "text", NULL, &dup) == NB_ERR_ALREADY);

    CHECK(nb_dds_writer_create(pub_topic, NULL, &writer) == NB_OK);
    CHECK(nb_dds_reader_create(sub_topic, NULL, NULL, NULL, &history_reader) == NB_OK);
    CHECK(nb_dds_reader_create(sub_topic, NULL, on_sample, &rx, &callback_reader) == NB_OK);
    if (!writer || !history_reader || !callback_reader) {
        goto out;
    }

    /* Callback readers have no history and cannot be waited on. */
    CHECK(nb_dds_waitset_create(p2, &waitset) == NB_OK);
    CHECK(nb_dds_waitset_attach_reader(waitset, callback_reader) == NB_ERR_INVALID);
    CHECK(nb_dds_waitset_attach_reader(waitset, history_reader) == NB_OK);
    CHECK(nb_dds_waitset_attach_reader(waitset, history_reader) == NB_OK);

    /* Discovery needs one SPDP round before matching. */
    for (i = 0; i < 100; i++) {
        (void)nb_dds_writer_write(writer, "hello", 5);
        if (nb_dds_reader_size(history_reader) > 0 && rx_count(&rx) > 0) {
            break;
        }
        ntk_sleep_ms(50);
    }
    CHECK(nb_dds_reader_size(history_reader) > 0);
    CHECK(rx_count(&rx) > 0);
    CHECK(strcmp(rx.last, "hello") == 0);

    CHECK(nb_dds_waitset_wait(waitset, NULL, 1000) == 1);
    CHECK(nb_dds_waitset_ready(waitset, NULL) >= 1);

    taken = nb_dds_reader_take(history_reader, NULL, samples, 4);
    CHECK(taken >= 1);
    if (taken >= 1) {
        CHECK(samples[0].data.len == 5);
        CHECK(memcmp(samples[0].data.data, "hello", 5) == 0);
        nb_dds_sample_release(samples, taken);
    }
    while ((taken = nb_dds_reader_take(history_reader, NULL, samples, 4)) > 0) {
        nb_dds_sample_release(samples, taken);
    }
    CHECK(nb_dds_reader_size(history_reader) == 0);

    /* Prefix applies to the wire name (RTPS backend exposes the built-in topic
     * snapshot; Cyclone reports none yet). */
    if (backend == NB_DDS_BACKEND_RTPS) {
        discovered_count = nb_dds_participant_discovery_read(p2, NB_DDS_BUILTIN_TOPIC_TOPIC,
                                                             discovered, 8);
        {
            int found = 0;
            size_t j;

            for (j = 0; j < discovered_count; j++) {
                if (strcmp(discovered[j].name, "app/chat") == 0) {
                    found = 1;
                }
            }
            CHECK(found);
        }
    }

    /* Argument validation. */
    CHECK(nb_dds_writer_write(writer, NULL, 0) == NB_ERR_INVALID);
    CHECK(nb_dds_topic_create(p1, "", "text", NULL, &dup) == NB_ERR_INVALID);
    CHECK(nb_dds_reader_create(NULL, NULL, NULL, NULL, &history_reader) == NB_ERR_INVALID);

out:
    nb_dds_waitset_destroy(waitset);
    nb_dds_reader_destroy(callback_reader);
    nb_dds_reader_destroy(history_reader);
    nb_dds_writer_destroy(writer);
    nb_dds_topic_destroy(sub_topic);
    nb_dds_topic_destroy(pub_topic);
    nb_dds_participant_destroy(p2);
    nb_dds_participant_destroy(p1);
    ntk_mutex_destroy(rx.lock);
    return g_fail ? 1 : 0;
}
