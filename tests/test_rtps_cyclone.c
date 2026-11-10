#define _CRT_SECURE_NO_WARNINGS

#include "rtps_participant.h"

#include "cy_dds_message.h"

#define NB_LOG_TAG "test"
#include <nb/base/log.h>

#include <dds/dds.h>
#include <ntk.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

/* Unicast-only mode: nb and Cyclone talk over fixed ports with explicit
 * discovery peers, so the test does not depend on multicast routing. */
#define UNICAST_ONLY_NB_SPDP_PORT 47100
#define UNICAST_ONLY_NB_UNICAST_PORT 47101
#define UNICAST_ONLY_CYCLONE_SPDP_PORT 7410

static dds_entity_t g_cyclone_domain = 0;

static dds_entity_t create_cyclone_participant(int unicast_only, const char *addr)
{
    if (unicast_only) {
        char xml[512];

        (void)snprintf(xml,
                       sizeof(xml),
                       "<CycloneDDS><Domain id=\"any\"><Discovery>"
                       "<ParticipantIndex>0</ParticipantIndex>"
                       "<Peers><Peer address=\"%s:%d\"/></Peers>"
                       "</Discovery></Domain></CycloneDDS>",
                       addr,
                       UNICAST_ONLY_NB_SPDP_PORT);
        g_cyclone_domain = dds_create_domain(0, xml);
        if (g_cyclone_domain < 0) {
            nb_log_error("Cyclone rejected the unicast-only domain config: %d",
                         (int)g_cyclone_domain);
            return g_cyclone_domain;
        }
    }
    return dds_create_participant(0, NULL, NULL);
}

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            nb_log_error("FAIL %s:%d: %s", __FILE__, __LINE__, #cond);  \
            g_fail = 1;                                                 \
        }                                                               \
    } while (0)

/* Returns 1 when the builtin participant topic contains this vendor id. */
static int cyclone_sees_participant(dds_entity_t reader)
{
    void *sample = NULL;
    dds_sample_info_t info;
    dds_return_t n;
    int found = 0;

    for (;;) {
        sample = NULL;
        memset(&info, 0, sizeof(info));
        n = dds_take(reader, &sample, &info, 1, 1);
        if (n < 0) {
            nb_log_error("dds_read failed: %s", dds_strretcode(n));
            return found;
        }
        if (n == 0) {
            break;
        }
        if (info.valid_data && sample) {
            dds_builtintopic_participant_t *participant =
                (dds_builtintopic_participant_t *)sample;
            if (participant->key.v[0] == NB_RTPS_VENDOR_ID_0 &&
                participant->key.v[1] == NB_RTPS_VENDOR_ID_1) {
                found = 1;
            }
        }
        if (sample) {
            (void)dds_return_loan(reader, &sample, (size_t)n);
        }
    }
    return found;
}

static void cyclone_scan_publications(dds_entity_t reader,
                                      const nb_rtps_guid_prefix_t *prefix,
                                      int *saw_sedp,
                                      int *saw_message)
{
    void *sample = NULL;
    dds_sample_info_t info;
    dds_return_t n;

    for (;;) {
        dds_builtintopic_endpoint_t *endpoint;

        sample = NULL;
        memset(&info, 0, sizeof(info));
        n = dds_take(reader, &sample, &info, 1, 1);
        if (n < 0) {
            nb_log_error("dds_read publication failed: %s", dds_strretcode(n));
            return;
        }
        if (n == 0) {
            break;
        }
        endpoint = (dds_builtintopic_endpoint_t *)sample;
        if (info.valid_data && endpoint && endpoint->topic_name && endpoint->type_name &&
            memcmp(endpoint->participant_key.v, prefix->bytes, 12) == 0) {
            if (strcmp(endpoint->topic_name, "nb/sedp") == 0 &&
                strcmp(endpoint->type_name, "nb::Sedp") == 0) {
                *saw_sedp = 1;
            }
            if (strcmp(endpoint->topic_name, NB_RTPS_DDS_MESSAGE_TOPIC) == 0 &&
                strcmp(endpoint->type_name, NB_RTPS_DDS_MESSAGE_TYPE) == 0) {
                *saw_message = 1;
            }
        }
        if (sample) {
            (void)dds_return_loan(reader, &sample, (size_t)n);
        }
    }
}

static int cyclone_takes_message(dds_entity_t reader,
                                 const uint8_t *expected,
                                 size_t expected_len)
{
    void *sample = NULL;
    dds_sample_info_t info;
    dds_return_t n;
    int found = 0;

    memset(&info, 0, sizeof(info));
    n = dds_take(reader, &sample, &info, 1, 1);
    if (n < 0) {
        nb_log_error("dds_take message failed: %s", dds_strretcode(n));
        return 0;
    }
    if (n > 0) {
        dds_Message *received = (dds_Message *)sample;
        if (info.valid_data && received &&
            received->payload._length == (uint32_t)expected_len &&
            (expected_len == 0 ||
             (received->payload._buffer != NULL &&
              memcmp(received->payload._buffer, expected, expected_len) == 0))) {
            found = 1;
        }
        (void)dds_return_loan(reader, &sample, (size_t)n);
    }
    return found;
}

typedef struct test_inbound_state {
    ntk_mutex *lock;
    int received;
    nb_rtps_guid_t reader_guid;
    nb_rtps_guid_t writer_guid;
    uint8_t payload[NB_RTPS_DATA_PAYLOAD_MAX];
    size_t payload_len;
    uint32_t hash;
} test_inbound_state_t;

static uint32_t payload_hash(const uint8_t *data, size_t len)
{
    uint32_t hash = 2166136261u;
    size_t i;

    for (i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static void on_inbound_data(void *user,
                            const nb_rtps_guid_t *reader_guid,
                            const nb_rtps_guid_t *writer_guid,
                            const nb_rtps_dds_message_t *message)
{
    test_inbound_state_t *state = (test_inbound_state_t *)user;
    size_t copy_len;

    if (!state || !reader_guid || !writer_guid || !message) {
        return;
    }
    ntk_mutex_lock(state->lock);
    if (state->received) {
        ntk_mutex_unlock(state->lock);
        return;
    }
    state->reader_guid = *reader_guid;
    state->writer_guid = *writer_guid;
    state->payload_len = message->payload_len;
    state->hash = message->payload ? payload_hash(message->payload, message->payload_len) : 0;
    copy_len = message->payload_len;
    if (copy_len > sizeof(state->payload)) {
        copy_len = sizeof(state->payload);
    }
    if (copy_len > 0 && message->payload) {
        memcpy(state->payload, message->payload, copy_len);
    }
    state->received = 1;
    ntk_mutex_unlock(state->lock);
}

static int inbound_received(test_inbound_state_t *state)
{
    int received;

    ntk_mutex_lock(state->lock);
    received = state->received;
    ntk_mutex_unlock(state->lock);
    return received;
}

static void test_cyclone_spdp(void)
{
    const char *enabled = getenv("NB_TEST_RTPS_CYCLONE");
    const char *advertise_host = getenv("NB_TEST_RTPS_ADVERTISE_HOST");
    const char *unicast_only_env = getenv("NB_TEST_RTPS_UNICAST_ONLY");
    int unicast_only = unicast_only_env && strcmp(unicast_only_env, "1") == 0;
    const char *addr =
        (advertise_host && advertise_host[0]) ? advertise_host : "127.0.0.1";
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *nb;
    dds_entity_t cyclone;
    dds_entity_t reader;
    dds_entity_t publication_reader;
    dds_entity_t message_topic;
    dds_entity_t message_reader;
    dds_entity_t message_writer;
    nb_rtps_endpoint_info_t endpoint;
    nb_rtps_endpoint_info_t message_endpoint;
    nb_rtps_endpoint_info_t subscription_endpoint;
    nb_rtps_participant_info_t local_info;
    test_inbound_state_t inbound_state;
    int saw_cyclone = 0;
    int saw_nb = 0;
    int saw_publication = 0;
    int saw_message_publication = 0;
    int saw_message = 0;
    int saw_writer_match = 0;
    int waited = 0;

    if (!enabled || strcmp(enabled, "1") != 0) {
        nb_log_info("Cyclone interop test skipped; set NB_TEST_RTPS_CYCLONE=1");
        return;
    }

    cyclone = create_cyclone_participant(unicast_only, addr);
    CHECK(cyclone > 0);
    if (cyclone <= 0) {
        return;
    }
    reader = dds_create_reader(cyclone, DDS_BUILTIN_TOPIC_DCPSPARTICIPANT, NULL, NULL);
    CHECK(reader > 0);
    if (reader <= 0) {
        (void)dds_delete(cyclone);
        return;
    }
    publication_reader =
        dds_create_reader(cyclone, DDS_BUILTIN_TOPIC_DCPSPUBLICATION, NULL, NULL);
    CHECK(publication_reader > 0);
    if (publication_reader <= 0) {
        (void)dds_delete(reader);
        (void)dds_delete(cyclone);
        return;
    }
    message_topic = dds_create_topic(
        cyclone, &dds_Message_desc, NB_RTPS_DDS_MESSAGE_TOPIC, NULL, NULL);
    CHECK(message_topic > 0);
    if (message_topic <= 0) {
        (void)dds_delete(publication_reader);
        (void)dds_delete(reader);
        (void)dds_delete(cyclone);
        return;
    }
    {
        dds_qos_t *reader_qos = dds_create_qos();
        CHECK(reader_qos != NULL);
        if (!reader_qos) {
            (void)dds_delete(message_topic);
            (void)dds_delete(publication_reader);
            (void)dds_delete(reader);
            (void)dds_delete(cyclone);
            return;
        }
        dds_qset_reliability(reader_qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
        /* Match the latency budget the nb publication offers: RxO requires the
         * offered budget to be no larger than the requested one. */
        dds_qset_latency_budget(reader_qos, DDS_MSECS(5));
        message_reader = dds_create_reader(cyclone, message_topic, reader_qos, NULL);
        dds_delete_qos(reader_qos);
    }
    CHECK(message_reader > 0);
    if (message_reader <= 0) {
        (void)dds_delete(message_topic);
        (void)dds_delete(publication_reader);
        (void)dds_delete(reader);
        (void)dds_delete(cyclone);
        return;
    }
    {
        dds_qos_t *writer_qos = dds_create_qos();
        CHECK(writer_qos != NULL);
        if (!writer_qos) {
            (void)dds_delete(message_reader);
            (void)dds_delete(message_topic);
            (void)dds_delete(publication_reader);
            (void)dds_delete(reader);
            (void)dds_delete(cyclone);
            return;
        }
        dds_qset_reliability(writer_qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
        message_writer = dds_create_writer(cyclone, message_topic, writer_qos, NULL);
        dds_delete_qos(writer_qos);
    }
    CHECK(message_writer > 0);
    if (message_writer <= 0) {
        (void)dds_delete(message_reader);
        (void)dds_delete(message_topic);
        (void)dds_delete(publication_reader);
        (void)dds_delete(reader);
        (void)dds_delete(cyclone);
        return;
    }

    memset(&config, 0, sizeof(config));
    config.domain_id = 0;
    config.bind_host = "0.0.0.0";
    config.advertise_host = addr;
    if (unicast_only) {
        config.multicast_group = "";
        config.spdp_port = UNICAST_ONLY_NB_SPDP_PORT;
        config.unicast_port = UNICAST_ONLY_NB_UNICAST_PORT;
    } else {
        config.multicast_group = "239.255.0.1";
        config.spdp_port = 0; /* 7400 + 250 * 0 */
    }
    config.announce_interval_ms = 200;
    config.lease_ms = 3000;
    config.name = "nb-rtps-interop";
    config.max_remote_participants = 32;
    nb = nb_rtps_participant_create(&config);
    CHECK(nb != NULL);
    if (!nb) {
        (void)dds_delete(message_writer);
        (void)dds_delete(message_reader);
        (void)dds_delete(message_topic);
        (void)dds_delete(reader);
        (void)dds_delete(publication_reader);
        (void)dds_delete(cyclone);
        return;
    }
    if (unicast_only) {
        CHECK(nb_rtps_participant_add_peer(
                  nb, addr, UNICAST_ONLY_CYCLONE_SPDP_PORT) == NB_OK);
    }
    memset(&inbound_state, 0, sizeof(inbound_state));
    inbound_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(inbound_state.lock != NULL);
    if (!inbound_state.lock) {
        nb_rtps_participant_destroy(nb);
        (void)dds_delete(message_writer);
        (void)dds_delete(message_reader);
        (void)dds_delete(message_topic);
        (void)dds_delete(reader);
        (void)dds_delete(publication_reader);
        (void)dds_delete(cyclone);
        return;
    }
    nb_rtps_participant_set_data_callback(nb, on_inbound_data, &inbound_state);
    CHECK(nb_rtps_participant_local_info(nb, &local_info) == NB_OK);
    memset(&endpoint, 0, sizeof(endpoint));
    (void)snprintf(endpoint.topic_name, sizeof(endpoint.topic_name), "%s", "nb/sedp");
    (void)snprintf(endpoint.type_name, sizeof(endpoint.type_name), "%s", "nb::Sedp");
    endpoint.reliability = NB_RTPS_RELIABILITY_RELIABLE;
    endpoint.durability = NB_RTPS_DURABILITY_VOLATILE;
    endpoint.history_kind = NB_RTPS_HISTORY_KEEP_LAST;
    endpoint.history_depth = 1;
    CHECK(nb_rtps_participant_add_endpoint(
              nb, NB_RTPS_ENDPOINT_PUBLICATION, &endpoint) == NB_OK);

    memset(&message_endpoint, 0, sizeof(message_endpoint));
    (void)snprintf(message_endpoint.topic_name,
                   sizeof(message_endpoint.topic_name),
                   "%s",
                   NB_RTPS_DDS_MESSAGE_TOPIC);
    (void)snprintf(message_endpoint.type_name,
                   sizeof(message_endpoint.type_name),
                   "%s",
                   NB_RTPS_DDS_MESSAGE_TYPE);
    message_endpoint.reliability = NB_RTPS_RELIABILITY_RELIABLE;
    message_endpoint.durability = NB_RTPS_DURABILITY_VOLATILE;
    message_endpoint.history_kind = NB_RTPS_HISTORY_KEEP_LAST;
    message_endpoint.history_depth = 1;
    /* Non-default QoS. The checks below read these back from Cyclone's
     * DCPSPublication sample, which proves it parsed our SEDP encoding. */
    message_endpoint.deadline_ms = 250;
    message_endpoint.latency_budget_ms = 5;
    message_endpoint.liveliness_kind = NB_RTPS_LIVELINESS_AUTOMATIC;
    message_endpoint.liveliness_lease_ms = 2000;
    message_endpoint.destination_order = NB_RTPS_DESTINATION_ORDER_BY_SOURCE;
    CHECK(nb_rtps_participant_add_endpoint(
              nb, NB_RTPS_ENDPOINT_PUBLICATION, &message_endpoint) == NB_OK);

    memset(&subscription_endpoint, 0, sizeof(subscription_endpoint));
    (void)snprintf(subscription_endpoint.topic_name,
                   sizeof(subscription_endpoint.topic_name),
                   "%s",
                   NB_RTPS_DDS_MESSAGE_TOPIC);
    (void)snprintf(subscription_endpoint.type_name,
                   sizeof(subscription_endpoint.type_name),
                   "%s",
                   NB_RTPS_DDS_MESSAGE_TYPE);
    subscription_endpoint.reliability = NB_RTPS_RELIABILITY_RELIABLE;
    subscription_endpoint.durability = NB_RTPS_DURABILITY_VOLATILE;
    subscription_endpoint.history_kind = NB_RTPS_HISTORY_KEEP_LAST;
    subscription_endpoint.history_depth = 1;
    CHECK(nb_rtps_participant_add_endpoint(
              nb, NB_RTPS_ENDPOINT_SUBSCRIPTION, &subscription_endpoint) == NB_OK);
    CHECK(subscription_endpoint.unicast_count > 0);
    CHECK(nb_rtps_participant_start(nb) == NB_OK);

    while (waited < 10000 &&
           (!saw_nb || !saw_cyclone || !saw_publication || !saw_message_publication)) {
        if (!saw_nb) {
            saw_nb = cyclone_sees_participant(reader);
        }
        if (!saw_cyclone) {
            saw_cyclone = nb_rtps_participant_remote_count(nb) > 0;
        }
        if (!saw_publication || !saw_message_publication) {
            cyclone_scan_publications(publication_reader,
                                      &local_info.guid.prefix,
                                      &saw_publication,
                                      &saw_message_publication);
        }
        if (!saw_nb || !saw_cyclone || !saw_publication || !saw_message_publication) {
            ntk_sleep_ms(100);
            waited += 100;
        }
    }
    CHECK(saw_nb);
    CHECK(saw_cyclone);
    CHECK(saw_publication);
    CHECK(saw_message_publication);
    if (!saw_nb || !saw_cyclone || !saw_publication || !saw_message_publication) {
        nb_log_error("Cyclone interop failed after %d ms (nb=%d cyclone=%d publication=%d message_publication=%d)",
                     waited,
                     saw_nb,
                     saw_cyclone,
                     saw_publication,
                     saw_message_publication);
    }

    {
        dds_instance_handle_t matched[8];
        dds_builtintopic_endpoint_t *ours = NULL;
        dds_return_t matched_count = 0;

        /*
         * Cyclone's own message writer is on the same topic, so "at least one
         * match" says nothing about us. Select the publication by GUID prefix
         * and then read back the QoS Cyclone parsed from our SEDP.
         */
        for (waited = 0; waited < 10000 && !ours; waited += 100) {
            dds_return_t i;

            matched_count = dds_get_matched_publications(message_reader, matched, 8);
            for (i = 0; i < matched_count && !ours; i++) {
                dds_builtintopic_endpoint_t *data =
                    dds_get_matched_publication_data(message_reader, matched[i]);

                if (!data) {
                    continue;
                }
                if (memcmp(data->key.v, local_info.guid.prefix.bytes, 12) == 0) {
                    ours = data;
                } else {
                    dds_builtintopic_free_endpoint(data);
                }
            }
            if (!ours) {
                ntk_sleep_ms(100);
            }
        }
        CHECK(matched_count > 0);
        CHECK(ours != NULL);
        if (!ours) {
            nb_log_error("Cyclone did not match the RTPS publication (matched=%d, %d ms)",
                         (int)matched_count,
                         waited);
        } else {
            dds_duration_t deadline = 0;
            dds_duration_t budget = 0;
            dds_duration_t lease = 0;
            dds_liveliness_kind_t kind = DDS_LIVELINESS_AUTOMATIC;
            dds_destination_order_kind_t order = DDS_DESTINATIONORDER_BY_RECEPTION_TIMESTAMP;

            CHECK(dds_qget_deadline(ours->qos, &deadline));
            CHECK(deadline == DDS_MSECS(250));
            CHECK(dds_qget_latency_budget(ours->qos, &budget));
            CHECK(budget == DDS_MSECS(5));
            CHECK(dds_qget_liveliness(ours->qos, &kind, &lease));
            CHECK(kind == DDS_LIVELINESS_AUTOMATIC);
            CHECK(lease == DDS_MSECS(2000));
            CHECK(dds_qget_destination_order(ours->qos, &order));
            CHECK(order == DDS_DESTINATIONORDER_BY_SOURCE_TIMESTAMP);
            if (deadline != DDS_MSECS(250) || budget != DDS_MSECS(5) ||
                lease != DDS_MSECS(2000) ||
                order != DDS_DESTINATIONORDER_BY_SOURCE_TIMESTAMP) {
                nb_log_error("Cyclone read back deadline=%lld budget=%lld "
                             "liveliness=%d lease=%lld order=%d",
                             (long long)deadline,
                             (long long)budget,
                             (int)kind,
                             (long long)lease,
                             (int)order);
            }
            dds_builtintopic_free_endpoint(ours);
        }
    }

    for (waited = 0;
         waited < 10000 && nb_rtps_participant_remote_endpoint_count(nb) == 0;
         waited += 50) {
        ntk_sleep_ms(50);
    }
    CHECK(nb_rtps_participant_remote_endpoint_count(nb) > 0);
    if (nb_rtps_participant_remote_endpoint_count(nb) == 0) {
        nb_log_error("nb participant did not discover the Cyclone subscription");
    }

    for (waited = 0; waited < 10000 && !saw_writer_match; waited += 50) {
        dds_instance_handle_t matched[8];
        dds_return_t matched_count =
            dds_get_matched_subscriptions(message_writer, matched, 8);
        if (matched_count > 0) {
            saw_writer_match = 1;
        } else {
            ntk_sleep_ms(50);
        }
    }
    CHECK(saw_writer_match);
    if (!saw_writer_match) {
        nb_log_error("Cyclone message writer did not match the RTPS subscription after %d ms",
                     waited);
    }

    for (waited = 0; waited < 10000 && !saw_message; waited += 50) {
        static const uint8_t expected[] = {
            0xde, 0xad, 0xbe, 0xef, 0x00, 0x11, 0x22
        };
        nb_rtps_dds_message_t message;
        int rc;

        message.payload = expected;
        message.payload_len = sizeof(expected);
        rc = nb_rtps_participant_publish_dds_message(
            nb, &message_endpoint.endpoint_guid, &message);
        if (rc != NB_OK && (waited % 1000) == 0) {
            nb_log_info("publish rc=%d remote_endpoints=%zu at %d ms",
                        rc,
                        nb_rtps_participant_remote_endpoint_count(nb),
                        waited);
        }
        if (rc == NB_OK) {
            saw_message = cyclone_takes_message(
                message_reader, expected, sizeof(expected));
        }
        if (!saw_message) {
            ntk_sleep_ms(50);
        }
    }
    CHECK(saw_message);
    if (!saw_message) {
        nb_log_error("Cyclone did not receive/deserialize RTPS DATA after %d ms", waited);
    }

    {
        static uint8_t inbound_expected[] = {
            0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77
        };
        dds_Message outgoing;

        memset(&outgoing, 0, sizeof(outgoing));
        outgoing.payload._length = (uint32_t)sizeof(inbound_expected);
        outgoing.payload._maximum = (uint32_t)sizeof(inbound_expected);
        outgoing.payload._buffer = inbound_expected;
        outgoing.payload._release = false;
        for (waited = 0;
             waited < 10000 && !inbound_received(&inbound_state);
             waited += 50) {
            dds_return_t rc = dds_write(message_writer, &outgoing);
            if (rc < 0 && (waited % 1000) == 0) {
                nb_log_info("Cyclone dds_write rc=%d at %d ms",
                            (int)rc,
                            waited);
            }
            if (!inbound_received(&inbound_state)) {
                ntk_sleep_ms(50);
            }
        }
        CHECK(inbound_received(&inbound_state));
        if (!inbound_received(&inbound_state)) {
            nb_log_error("RTPS subscription did not receive Cyclone DATA after %d ms",
                         waited);
        }
        CHECK(inbound_state.payload_len == sizeof(inbound_expected));
        CHECK(memcmp(inbound_state.payload,
                     inbound_expected,
                     sizeof(inbound_expected)) == 0);
        CHECK(nb_rtps_guid_equal(&inbound_state.reader_guid,
                                 &subscription_endpoint.endpoint_guid));
        CHECK(!nb_rtps_entity_id_equal(&inbound_state.writer_guid.entity,
                                       &nb_rtps_eid_unknown));
    }

    /*
     * Large samples travel as DATA_FRAG. Cyclone fragments its own writes and
     * reassembles ours, so this covers both directions of the fragmentation
     * path against a second implementation.
     */
    {
        const size_t big_len = 8192;
        uint8_t *big = (uint8_t *)malloc(big_len);
        uint8_t *serialized = (uint8_t *)malloc(big_len + 8u);
        size_t serialized_len = 0;
        size_t i;

        CHECK(big != NULL && serialized != NULL);
        if (!big || !serialized) {
            free(big);
            free(serialized);
        } else {
            nb_rtps_dds_message_t big_message;
            dds_Message outgoing;
            int saw_big = 0;
            uint32_t expected_hash;

            for (i = 0; i < big_len; i++) {
                big[i] = (uint8_t)((i * 37u + 11u) & 0xffu);
            }
            expected_hash = payload_hash(big, big_len);

            /* nb writer -> Cyclone reader. */
            big_message.payload = big;
            big_message.payload_len = big_len;
            CHECK(nb_rtps_dds_message_serialize(
                      &big_message, serialized, big_len + 8u, &serialized_len) == NB_OK);
            for (waited = 0; waited < 10000 && !saw_big; waited += 50) {
                if (nb_rtps_participant_publish(nb,
                                                &message_endpoint.endpoint_guid,
                                                serialized,
                                                serialized_len) == NB_OK) {
                    saw_big = cyclone_takes_message(message_reader, big, big_len);
                }
                if (!saw_big) {
                    ntk_sleep_ms(50);
                }
            }
            CHECK(saw_big);
            if (!saw_big) {
                nb_log_error("Cyclone did not reassemble the nb DATA_FRAG sample after %d ms",
                             waited);
            }

            /* Cyclone writer -> nb reader. */
            memset(&outgoing, 0, sizeof(outgoing));
            outgoing.payload._length = (uint32_t)big_len;
            outgoing.payload._maximum = (uint32_t)big_len;
            outgoing.payload._buffer = big;
            outgoing.payload._release = false;
            ntk_mutex_lock(inbound_state.lock);
            inbound_state.received = 0;
            inbound_state.payload_len = 0;
            inbound_state.hash = 0;
            ntk_mutex_unlock(inbound_state.lock);
            for (waited = 0; waited < 10000; waited += 50) {
                int done;

                (void)dds_write(message_writer, &outgoing);
                ntk_sleep_ms(50);
                ntk_mutex_lock(inbound_state.lock);
                done = inbound_state.received && inbound_state.payload_len == big_len &&
                       inbound_state.hash == expected_hash;
                ntk_mutex_unlock(inbound_state.lock);
                if (done) {
                    break;
                }
            }
            CHECK(inbound_state.received);
            CHECK(inbound_state.payload_len == big_len);
            CHECK(inbound_state.hash == expected_hash);
            if (inbound_state.payload_len != big_len) {
                nb_log_error("nb received %u of %u bytes of the Cyclone DATA_FRAG sample",
                             (unsigned)inbound_state.payload_len,
                             (unsigned)big_len);
            }
            free(serialized);
            free(big);
        }
    }

    nb_rtps_participant_stop(nb);
    nb_rtps_participant_destroy(nb);
    ntk_mutex_destroy(inbound_state.lock);
    (void)dds_delete(message_writer);
    (void)dds_delete(message_reader);
    (void)dds_delete(message_topic);
    (void)dds_delete(reader);
    (void)dds_delete(publication_reader);
    (void)dds_delete(cyclone);
    if (g_cyclone_domain > 0) {
        (void)dds_delete(g_cyclone_domain);
        g_cyclone_domain = 0;
    }
}

/* -------------------------------------------------------------------------
 * Scenario matrix
 *
 * The checks above prove one configuration: reliable, volatile, one reader and
 * one writer on each side. Real deployments exercise more of DDS than that, so
 * these scenarios add the paths that only a second implementation can validate:
 *
 *   - best-effort in both directions,
 *   - TRANSIENT_LOCAL late joiners in both directions,
 *   - one nb writer feeding two Cyclone readers,
 *   - a Cyclone participant that goes away and comes back,
 *   - samples large enough to need many fragments in both directions.
 *
 * Each scenario owns a DDS domain of its own so a failure cannot leak state
 * into the next one. Unicast-only mode pins both sides to explicit ports.
 * ------------------------------------------------------------------------- */

#define MATRIX_TYPE NB_RTPS_DDS_MESSAGE_TYPE
#define MATRIX_DEADLINE_MS 10000

typedef struct matrix_scenario {
    uint32_t domain;
    const char *addr;
    int unicast_only;
    uint16_t nb_spdp_port;
    dds_entity_t cy_domain; /* only created in unicast-only mode */
    dds_entity_t participant;
    dds_entity_t topic;
    nb_rtps_participant_t *nb;
    nb_rtps_participant_info_t local;
    test_inbound_state_t inbound;
    int started;
    /* Unique per run so a stray participant on the same lab network (or a
     * previous run) cannot match the readers and mask a failure. */
    char topic_name[NB_RTPS_SEDP_TOPIC_MAX];
} matrix_scenario_t;

static uint16_t matrix_nb_spdp_port(uint32_t domain)
{
    return (uint16_t)(47200u + 10u * domain);
}

static uint16_t matrix_cy_spdp_port(uint32_t domain)
{
    /* Cyclone default mapping: 7400 + 250 * domain + 10 + 2 * participant. */
    return (uint16_t)(7410u + 250u * domain);
}

static dds_entity_t matrix_cyclone_participant(matrix_scenario_t *s)
{
    if (s->unicast_only) {
        char xml[512];

        (void)snprintf(xml,
                       sizeof(xml),
                       "<CycloneDDS><Domain id=\"any\"><Discovery>"
                       "<ParticipantIndex>0</ParticipantIndex>"
                       "<Peers><Peer address=\"%s:%u\"/></Peers>"
                       "</Discovery></Domain></CycloneDDS>",
                       s->addr,
                       (unsigned)matrix_nb_spdp_port(s->domain));
        s->cy_domain = dds_create_domain(s->domain, xml);
        if (s->cy_domain < 0) {
            nb_log_error("Cyclone rejected the domain config: %d", (int)s->cy_domain);
            return s->cy_domain;
        }
    }
    return dds_create_participant(s->domain, NULL, NULL);
}

static void matrix_on_status(void *user, const nb_rtps_status_event_t *event)
{
    matrix_scenario_t *s = (matrix_scenario_t *)user;

    if (!s || !event) {
        return;
    }
    nb_log_info("matrix[%u] status kind=%d count=%u total=%u peer=%d alive=%d reason=%s",
                (unsigned)s->domain,
                (int)event->kind,
                event->count,
                event->total_count,
                event->has_peer,
                event->alive,
                event->reason);
}

static int matrix_start(matrix_scenario_t *s,
                        uint32_t domain,
                        const char *addr,
                        int unicast_only)
{
    nb_rtps_participant_config_t config;

    memset(s, 0, sizeof(*s));
    s->domain = domain;
    s->addr = addr;
    s->unicast_only = unicast_only;
    s->nb_spdp_port = matrix_nb_spdp_port(domain);
    s->inbound.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    if (!s->inbound.lock) {
        return -1;
    }
    s->participant = matrix_cyclone_participant(s);
    if (s->participant < 0) {
        nb_log_error("Cyclone participant for domain %u failed: %d",
                     (unsigned)domain,
                     (int)s->participant);
        return -1;
    }
    (void)snprintf(s->topic_name,
                   sizeof(s->topic_name),
                   "nb/matrix/%u/%llu",
                   (unsigned)domain,
                   (unsigned long long)ntk_wall_ms());
    s->topic = dds_create_topic(
        s->participant, &dds_Message_desc, s->topic_name, NULL, NULL);
    if (s->topic < 0) {
        nb_log_error("Cyclone topic failed: %s", dds_strretcode(s->topic));
        return -1;
    }

    memset(&config, 0, sizeof(config));
    config.domain_id = domain;
    config.bind_host = "0.0.0.0";
    config.advertise_host = addr;
    if (unicast_only) {
        config.multicast_group = "";
        config.spdp_port = s->nb_spdp_port;
        config.unicast_port = (uint16_t)(s->nb_spdp_port + 1u);
    }
    config.announce_interval_ms = 200;
    config.lease_ms = 4000;
    config.name = "nb-rtps-matrix";
    s->nb = nb_rtps_participant_create(&config);
    if (!s->nb) {
        nb_log_error("nb participant for domain %u failed", (unsigned)domain);
        return -1;
    }
    if (unicast_only &&
        nb_rtps_participant_add_peer(s->nb, addr, matrix_cy_spdp_port(domain)) != NB_OK) {
        nb_log_error("nb add_peer failed");
        return -1;
    }
    nb_rtps_participant_set_data_callback(s->nb, on_inbound_data, &s->inbound);
    nb_rtps_participant_set_status_callback(s->nb, matrix_on_status, s);
    if (nb_rtps_participant_local_info(s->nb, &s->local) != NB_OK ||
        nb_rtps_participant_start(s->nb) != NB_OK) {
        nb_log_error("nb participant start failed");
        return -1;
    }
    s->started = 1;
    return 0;
}

static void matrix_stop(matrix_scenario_t *s)
{
    if (s->nb) {
        if (s->started) {
            nb_rtps_participant_stop(s->nb);
        }
        nb_rtps_participant_destroy(s->nb);
        s->nb = NULL;
    }
    if (s->participant > 0) {
        (void)dds_delete(s->participant);
        s->participant = 0;
    }
    if (s->cy_domain > 0) {
        (void)dds_delete(s->cy_domain);
        s->cy_domain = 0;
    }
    if (s->inbound.lock) {
        ntk_mutex_destroy(s->inbound.lock);
        s->inbound.lock = NULL;
    }
}

static void matrix_inbound_reset(matrix_scenario_t *s)
{
    ntk_mutex_lock(s->inbound.lock);
    s->inbound.received = 0;
    s->inbound.payload_len = 0;
    s->inbound.hash = 0;
    ntk_mutex_unlock(s->inbound.lock);
}

static int matrix_wait_remotes(matrix_scenario_t *s, size_t want, int timeout_ms)
{
    int waited;

    for (waited = 0; waited < timeout_ms; waited += 50) {
        if (nb_rtps_participant_remote_endpoint_count(s->nb) >= want) {
            return 1;
        }
        ntk_sleep_ms(50);
    }
    nb_log_error("domain %u: %zu remote endpoints after %d ms",
                 (unsigned)s->domain,
                 nb_rtps_participant_remote_endpoint_count(s->nb),
                 timeout_ms);
    return 0;
}

static int matrix_add_pub(matrix_scenario_t *s,
                          uint32_t reliability,
                          uint32_t durability,
                          uint32_t depth,
                          nb_rtps_endpoint_info_t *out)
{
    nb_rtps_endpoint_info_t info;

    memset(&info, 0, sizeof(info));
    (void)snprintf(info.topic_name, sizeof(info.topic_name), "%s", s->topic_name);
    (void)snprintf(info.type_name, sizeof(info.type_name), "%s", MATRIX_TYPE);
    info.reliability = reliability;
    info.durability = durability;
    info.history_kind = NB_RTPS_HISTORY_KEEP_LAST;
    info.history_depth = depth;
    if (nb_rtps_participant_add_endpoint(s->nb, NB_RTPS_ENDPOINT_PUBLICATION, &info) !=
        NB_OK) {
        nb_log_error("domain %u: add publication failed", (unsigned)s->domain);
        return -1;
    }
    if (out) {
        *out = info;
    }
    return 0;
}

static int matrix_add_sub(matrix_scenario_t *s,
                          uint32_t reliability,
                          uint32_t durability,
                          uint32_t depth,
                          nb_rtps_endpoint_info_t *out)
{
    nb_rtps_endpoint_info_t info;

    memset(&info, 0, sizeof(info));
    (void)snprintf(info.topic_name, sizeof(info.topic_name), "%s", s->topic_name);
    (void)snprintf(info.type_name, sizeof(info.type_name), "%s", MATRIX_TYPE);
    info.reliability = reliability;
    info.durability = durability;
    info.history_kind = NB_RTPS_HISTORY_KEEP_LAST;
    info.history_depth = depth;
    if (nb_rtps_participant_add_endpoint(s->nb, NB_RTPS_ENDPOINT_SUBSCRIPTION, &info) !=
        NB_OK) {
        nb_log_error("domain %u: add subscription failed", (unsigned)s->domain);
        return -1;
    }
    if (out) {
        *out = info;
    }
    return 0;
}

static dds_entity_t matrix_cy_reader(matrix_scenario_t *s,
                                     dds_reliability_kind_t reliability,
                                     dds_durability_kind_t durability,
                                     int32_t depth)
{
    dds_qos_t *qos = dds_create_qos();
    dds_entity_t reader;

    if (!qos) {
        return DDS_RETCODE_OUT_OF_RESOURCES;
    }
    dds_qset_reliability(qos, reliability, DDS_SECS(1));
    dds_qset_durability(qos, durability);
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, depth);
    /* Do not match the local Cyclone writer: the match counters and the
     * samples must reflect only the remote nb endpoint. */
    dds_qset_ignorelocal(qos, DDS_IGNORELOCAL_PARTICIPANT);
    reader = dds_create_reader(s->participant, s->topic, qos, NULL);
    dds_delete_qos(qos);
    return reader;
}

static dds_entity_t matrix_cy_writer(matrix_scenario_t *s,
                                     dds_reliability_kind_t reliability,
                                     dds_durability_kind_t durability,
                                     int32_t depth)
{
    dds_qos_t *qos = dds_create_qos();
    dds_entity_t writer;

    if (!qos) {
        return DDS_RETCODE_OUT_OF_RESOURCES;
    }
    dds_qset_reliability(qos, reliability, DDS_SECS(1));
    dds_qset_durability(qos, durability);
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, depth);
    dds_qset_ignorelocal(qos, DDS_IGNORELOCAL_PARTICIPANT);
    writer = dds_create_writer(s->participant, s->topic, qos, NULL);
    dds_delete_qos(qos);
    return writer;
}

/* Wait for one message with exactly this payload; leaves other samples alone. */
static int matrix_cy_take(dds_entity_t reader,
                          const uint8_t *expected,
                          size_t expected_len,
                          int timeout_ms)
{
    int waited;

    for (waited = 0; waited < timeout_ms; waited += 20) {
        void *sample = NULL;
        dds_sample_info_t info;
        dds_return_t n;

        memset(&info, 0, sizeof(info));
        n = dds_take(reader, &sample, &info, 1, 1);
        if (n < 0) {
            nb_log_error("dds_take failed: %s", dds_strretcode(n));
            return 0;
        }
        if (n > 0) {
            dds_Message *message = (dds_Message *)sample;
            int match = info.valid_data && message &&
                        message->payload._length == (uint32_t)expected_len &&
                        (expected_len == 0 ||
                         (message->payload._buffer &&
                          memcmp(message->payload._buffer,
                                 expected,
                                 expected_len) == 0));
            if (!match) {
                nb_log_info("matrix: reader saw a sample that did not match "
                            "(valid=%d len=%u expected=%u)",
                            (int)info.valid_data,
                            (unsigned)(message ? message->payload._length : 0u),
                            (unsigned)expected_len);
            }
            (void)dds_return_loan(reader, &sample, (size_t)n);
            if (match) {
                return 1;
            }
        }
        ntk_sleep_ms(20);
    }
    return 0;
}

static int matrix_cy_write(dds_entity_t writer, uint8_t *buffer, size_t len)
{
    dds_Message message;

    memset(&message, 0, sizeof(message));
    message.payload._length = (uint32_t)len;
    message.payload._maximum = (uint32_t)len;
    message.payload._buffer = buffer;
    message.payload._release = false;
    return dds_write(writer, &message) >= 0;
}

static int matrix_nb_publish(matrix_scenario_t *s,
                             const nb_rtps_guid_t *writer,
                             const uint8_t *payload,
                             size_t len)
{
    nb_rtps_dds_message_t message;
    uint8_t *serialized = (uint8_t *)malloc(len + 8u);
    size_t serialized_len = 0;
    int rc;

    if (!serialized) {
        return 0;
    }
    message.payload = payload;
    message.payload_len = len;
    memset(&message.sample, 0, sizeof(message.sample));
    rc = nb_rtps_dds_message_serialize(&message, serialized, len + 8u, &serialized_len);
    if (rc == NB_OK) {
        rc = nb_rtps_participant_publish(s->nb, writer, serialized, serialized_len);
    }
    free(serialized);
    return rc == NB_OK;
}

/* Best-effort in both directions. */
static void matrix_best_effort(const char *addr, int unicast_only, uint32_t domain)
{
    static const uint8_t to_cyclone[] = {0xa1, 0xa2, 0xa3, 0xa4};
    static const uint8_t to_nb[] = {0xb1, 0xb2, 0xb3, 0xb4, 0xb5};
    matrix_scenario_t s;
    nb_rtps_endpoint_info_t pub;
    dds_entity_t cy_reader;
    dds_entity_t cy_writer;
    uint8_t buffer[sizeof(to_nb)];
    int ok = 1;

    if (matrix_start(&s, domain, addr, unicast_only) != 0) {
        CHECK(0);
        matrix_stop(&s);
        return;
    }
    cy_reader = matrix_cy_reader(&s, DDS_RELIABILITY_BEST_EFFORT, DDS_DURABILITY_VOLATILE, 1);
    cy_writer = matrix_cy_writer(&s, DDS_RELIABILITY_BEST_EFFORT, DDS_DURABILITY_VOLATILE, 1);
    CHECK(cy_reader > 0 && cy_writer > 0);
    if (cy_reader <= 0 || cy_writer <= 0 ||
        matrix_add_pub(&s, NB_RTPS_RELIABILITY_BEST_EFFORT, NB_RTPS_DURABILITY_VOLATILE, 1, &pub) != 0 ||
        matrix_add_sub(&s, NB_RTPS_RELIABILITY_BEST_EFFORT, NB_RTPS_DURABILITY_VOLATILE, 1, NULL) != 0) {
        ok = 0;
    }
    if (ok && !matrix_wait_remotes(&s, 2, MATRIX_DEADLINE_MS)) {
        ok = 0;
    }
    if (ok) {
        int waited;
        int taken = 0;

        ntk_sleep_ms(100);
        /* Best effort has no retransmission: keep writing until the reader
         * takes one, which also covers the window in which the two sides have
         * discovered each other but the reader has not processed our SEDP
         * publication yet. */
        for (waited = 0; waited < MATRIX_DEADLINE_MS && !taken; waited += 50) {
            (void)matrix_nb_publish(&s, &pub.endpoint_guid, to_cyclone, sizeof(to_cyclone));
            taken = matrix_cy_take(cy_reader, to_cyclone, sizeof(to_cyclone), 100);
            if (!taken) {
                ntk_sleep_ms(50);
            }
        }
        CHECK(taken);
        matrix_inbound_reset(&s);
        memcpy(buffer, to_nb, sizeof(to_nb));
        CHECK(matrix_cy_write(cy_writer, buffer, sizeof(buffer)));
        {
            int waited;
            int received = 0;

            for (waited = 0; waited < MATRIX_DEADLINE_MS; waited += 50) {
                ntk_mutex_lock(s.inbound.lock);
                received = s.inbound.received && s.inbound.payload_len == sizeof(to_nb) &&
                           s.inbound.hash == payload_hash(to_nb, sizeof(to_nb));
                ntk_mutex_unlock(s.inbound.lock);
                if (received) {
                    break;
                }
                /* Best effort: discovery may have completed between the match
                 * count and the write, so repeat the sample. */
                (void)matrix_cy_write(cy_writer, buffer, sizeof(buffer));
                ntk_sleep_ms(50);
            }
            CHECK(received);
        }
    }
    matrix_stop(&s);
    nb_log_info("interop matrix: best effort domain %u done", (unsigned)domain);
}

/* TRANSIENT_LOCAL late joiners, Cyclone writer -> nb reader. */
static void matrix_late_join_cyclone_writer(const char *addr, int unicast_only, uint32_t domain)
{
    static uint8_t payload[37];
    matrix_scenario_t s;
    dds_entity_t cy_writer;
    int waited;
    int received = 0;
    size_t i;

    for (i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(0x40u + i);
    }
    if (matrix_start(&s, domain, addr, unicast_only) != 0) {
        CHECK(0);
        matrix_stop(&s);
        return;
    }
    cy_writer = matrix_cy_writer(&s, DDS_RELIABILITY_RELIABLE, DDS_DURABILITY_TRANSIENT_LOCAL, 1);
    CHECK(cy_writer > 0);
    if (cy_writer > 0) {
        CHECK(matrix_cy_write(cy_writer, payload, sizeof(payload)));
        ntk_sleep_ms(300);
        /* The reader is created only now: the sample already sits in the
         * writer's TRANSIENT_LOCAL history. */
        CHECK(matrix_add_sub(&s,
                             NB_RTPS_RELIABILITY_RELIABLE,
                             NB_RTPS_DURABILITY_TRANSIENT_LOCAL,
                             1,
                             NULL) == 0);
        CHECK(matrix_wait_remotes(&s, 1, MATRIX_DEADLINE_MS));
        {
            dds_publication_matched_status_t matched;

            memset(&matched, 0, sizeof(matched));
            if (dds_get_publication_matched_status(cy_writer, &matched) == 0) {
                nb_log_info("matrix[%u] Cyclone writer matched current=%u total=%u",
                            (unsigned)domain,
                            matched.current_count,
                            matched.total_count);
            }
        }
        for (waited = 0; waited < MATRIX_DEADLINE_MS; waited += 50) {
            ntk_mutex_lock(s.inbound.lock);
            received = s.inbound.received && s.inbound.payload_len == sizeof(payload) &&
                       s.inbound.hash == payload_hash(payload, sizeof(payload));
            ntk_mutex_unlock(s.inbound.lock);
            if (received) {
                break;
            }
            ntk_sleep_ms(50);
        }
        CHECK(received);
        if (!received) {
            nb_log_error("domain %u: nb reader did not receive the Cyclone TRANSIENT_LOCAL history",
                         (unsigned)domain);
        }
    }
    matrix_stop(&s);
    nb_log_info("interop matrix: Cyclone-writer late join domain %u", (unsigned)domain);
}

/* TRANSIENT_LOCAL late joiners, nb writer -> Cyclone reader. */
static void matrix_late_join_nb_writer(const char *addr, int unicast_only, uint32_t domain)
{
    static uint8_t payload[41];
    matrix_scenario_t s;
    nb_rtps_endpoint_info_t pub;
    dds_entity_t cy_reader;
    size_t i;

    for (i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(0x80u + i);
    }
    if (matrix_start(&s, domain, addr, unicast_only) != 0) {
        CHECK(0);
        matrix_stop(&s);
        return;
    }
    CHECK(matrix_add_pub(&s,
                         NB_RTPS_RELIABILITY_RELIABLE,
                         NB_RTPS_DURABILITY_TRANSIENT_LOCAL,
                         1,
                         &pub) == 0);
    /* With no reader matched yet the write is retained in the history but
     * reports NOTFOUND; the replay on discovery is what the test checks. */
    (void)matrix_nb_publish(&s, &pub.endpoint_guid, payload, sizeof(payload));
    ntk_sleep_ms(300);
    /* The reader joins after the write; nb has to replay its retained
     * history when SEDP announces the reader. */
    cy_reader = matrix_cy_reader(&s, DDS_RELIABILITY_RELIABLE, DDS_DURABILITY_TRANSIENT_LOCAL, 1);
    CHECK(cy_reader > 0);
    if (cy_reader > 0) {
        CHECK(matrix_wait_remotes(&s, 1, MATRIX_DEADLINE_MS));
        CHECK(matrix_cy_take(cy_reader, payload, sizeof(payload), MATRIX_DEADLINE_MS));
    }
    matrix_stop(&s);
    nb_log_info("interop matrix: nb-writer late join domain %u", (unsigned)domain);
}

/* One nb writer feeding two Cyclone readers on the same topic. */
static void matrix_multi_reader(const char *addr, int unicast_only, uint32_t domain)
{
    static const uint8_t payload[] = {0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6};
    matrix_scenario_t s;
    nb_rtps_endpoint_info_t pub;
    dds_entity_t reader_a;
    dds_entity_t reader_b;

    if (matrix_start(&s, domain, addr, unicast_only) != 0) {
        CHECK(0);
        matrix_stop(&s);
        return;
    }
    reader_a = matrix_cy_reader(&s, DDS_RELIABILITY_RELIABLE, DDS_DURABILITY_VOLATILE, 1);
    reader_b = matrix_cy_reader(&s, DDS_RELIABILITY_RELIABLE, DDS_DURABILITY_VOLATILE, 1);
    CHECK(reader_a > 0 && reader_b > 0);
    CHECK(matrix_add_pub(&s,
                         NB_RTPS_RELIABILITY_RELIABLE,
                         NB_RTPS_DURABILITY_VOLATILE,
                         1,
                         &pub) == 0);
    if (reader_a > 0 && reader_b > 0 &&
        matrix_wait_remotes(&s, 2, MATRIX_DEADLINE_MS)) {
        dds_subscription_matched_status_t ma;
        dds_subscription_matched_status_t mb;
        int waited;

        /* nb's view of the two remote readers is not the same thing as the
         * readers having matched our publication: wait for Cyclone's side so
         * the check is about fan-out, not about discovery timing. */
        memset(&ma, 0, sizeof(ma));
        memset(&mb, 0, sizeof(mb));
        for (waited = 0; waited < MATRIX_DEADLINE_MS; waited += 50) {
            (void)dds_get_subscription_matched_status(reader_a, &ma);
            (void)dds_get_subscription_matched_status(reader_b, &mb);
            if (ma.current_count > 0 && mb.current_count > 0) {
                break;
            }
            ntk_sleep_ms(50);
        }
        CHECK(ma.current_count > 0 && mb.current_count > 0);
        nb_log_info("matrix[%u] Cyclone readers matched a=%u b=%u",
                    (unsigned)domain,
                    ma.current_count,
                    mb.current_count);
        /* Reliable: nb retransmits until both readers acknowledge, so one
         * write is enough even if the readers matched a moment apart. */
        CHECK(matrix_nb_publish(&s, &pub.endpoint_guid, payload, sizeof(payload)));
        CHECK(matrix_cy_take(reader_a, payload, sizeof(payload), MATRIX_DEADLINE_MS));
        CHECK(matrix_cy_take(reader_b, payload, sizeof(payload), MATRIX_DEADLINE_MS));
    }
    matrix_stop(&s);
    nb_log_info("interop matrix: multi-reader domain %u", (unsigned)domain);
}

/* A Cyclone participant that disappears and comes back. */
static void matrix_restart(const char *addr, int unicast_only, uint32_t domain)
{
    static const uint8_t first[] = {0xd1, 0xd2, 0xd3};
    static const uint8_t second[] = {0xe1, 0xe2, 0xe3, 0xe4};
    matrix_scenario_t s;
    dds_entity_t cy_writer;
    int waited;

    if (matrix_start(&s, domain, addr, unicast_only) != 0) {
        CHECK(0);
        matrix_stop(&s);
        return;
    }
    /* TRANSIENT_LOCAL so the sample the writer produced before the two sides
     * finished matching is legitimately available to the reader, both before
     * and after the restart. */
    CHECK(matrix_add_sub(&s,
                         NB_RTPS_RELIABILITY_RELIABLE,
                         NB_RTPS_DURABILITY_TRANSIENT_LOCAL,
                         1,
                         NULL) == 0);
    cy_writer = matrix_cy_writer(&s,
                                 DDS_RELIABILITY_RELIABLE,
                                 DDS_DURABILITY_TRANSIENT_LOCAL,
                                 1);
    CHECK(cy_writer > 0);
    if (cy_writer > 0 && matrix_wait_remotes(&s, 1, MATRIX_DEADLINE_MS)) {
        uint8_t buffer[sizeof(first)];

        ntk_sleep_ms(100);
        memcpy(buffer, first, sizeof(first));
        CHECK(matrix_cy_write(cy_writer, buffer, sizeof(buffer)));
        for (waited = 0; waited < MATRIX_DEADLINE_MS; waited += 50) {
            int received;

            ntk_mutex_lock(s.inbound.lock);
            received = s.inbound.received &&
                       s.inbound.hash == payload_hash(first, sizeof(first));
            ntk_mutex_unlock(s.inbound.lock);
            if (received) {
                break;
            }
            ntk_sleep_ms(50);
        }
        CHECK(waited < MATRIX_DEADLINE_MS);
    }

    /* Restart the Cyclone participant in the same domain. In unicast-only mode
     * the domain entity stays; deleting the participant is what a restarting
     * process does. */
    if (s.participant > 0) {
        (void)dds_delete(s.participant);
        s.participant = 0;
    }
    for (waited = 0; waited < MATRIX_DEADLINE_MS; waited += 100) {
        if (nb_rtps_participant_remote_endpoint_count(s.nb) == 0) {
            break;
        }
        ntk_sleep_ms(100);
    }
    CHECK(nb_rtps_participant_remote_endpoint_count(s.nb) == 0);

    s.participant = dds_create_participant(s.domain, NULL, NULL);
    CHECK(s.participant > 0);
    if (s.participant > 0) {
        s.topic = dds_create_topic(
            s.participant, &dds_Message_desc, s.topic_name, NULL, NULL);
        CHECK(s.topic > 0);
        cy_writer = matrix_cy_writer(&s,
                                     DDS_RELIABILITY_RELIABLE,
                                     DDS_DURABILITY_TRANSIENT_LOCAL,
                                     1);
        CHECK(cy_writer > 0);
        if (cy_writer > 0 && matrix_wait_remotes(&s, 1, MATRIX_DEADLINE_MS)) {
            uint8_t buffer[sizeof(second)];

            matrix_inbound_reset(&s);
            ntk_sleep_ms(100);
            memcpy(buffer, second, sizeof(second));
            CHECK(matrix_cy_write(cy_writer, buffer, sizeof(buffer)));
            for (waited = 0; waited < MATRIX_DEADLINE_MS; waited += 50) {
                int received;

                ntk_mutex_lock(s.inbound.lock);
                received = s.inbound.received &&
                           s.inbound.hash == payload_hash(second, sizeof(second));
                ntk_mutex_unlock(s.inbound.lock);
                if (received) {
                    break;
                }
                ntk_sleep_ms(50);
            }
            CHECK(waited < MATRIX_DEADLINE_MS);
        }
    }
    matrix_stop(&s);
    nb_log_info("interop matrix: participant restart domain %u", (unsigned)domain);
}

/* 200 KiB samples in both directions: many fragments, both assemblers. */
static void matrix_large_samples(const char *addr, int unicast_only, uint32_t domain)
{
    const size_t big_len = 200u * 1024u;
    matrix_scenario_t s;
    nb_rtps_endpoint_info_t pub;
    dds_entity_t cy_reader;
    dds_entity_t cy_writer;
    uint8_t *to_cyclone = (uint8_t *)malloc(big_len);
    uint8_t *to_nb = (uint8_t *)malloc(big_len);
    size_t i;
    int ok = 1;

    if (!to_cyclone || !to_nb) {
        free(to_cyclone);
        free(to_nb);
        CHECK(0);
        return;
    }
    for (i = 0; i < big_len; i++) {
        to_cyclone[i] = (uint8_t)((i * 131u + 7u) & 0xffu);
        to_nb[i] = (uint8_t)((i * 17u + 89u) & 0xffu);
    }
    if (matrix_start(&s, domain, addr, unicast_only) != 0) {
        CHECK(0);
        free(to_cyclone);
        free(to_nb);
        matrix_stop(&s);
        return;
    }
    cy_reader = matrix_cy_reader(&s, DDS_RELIABILITY_RELIABLE, DDS_DURABILITY_VOLATILE, 1);
    cy_writer = matrix_cy_writer(&s, DDS_RELIABILITY_RELIABLE, DDS_DURABILITY_VOLATILE, 1);
    CHECK(cy_reader > 0 && cy_writer > 0);
    if (cy_reader <= 0 || cy_writer <= 0 ||
        matrix_add_pub(&s,
                       NB_RTPS_RELIABILITY_RELIABLE,
                       NB_RTPS_DURABILITY_VOLATILE,
                       1,
                       &pub) != 0 ||
        matrix_add_sub(&s,
                       NB_RTPS_RELIABILITY_RELIABLE,
                       NB_RTPS_DURABILITY_VOLATILE,
                       1,
                       NULL) != 0) {
        ok = 0;
    }
    if (ok && !matrix_wait_remotes(&s, 2, MATRIX_DEADLINE_MS)) {
        ok = 0;
    }
    if (ok) {
        dds_subscription_matched_status_t matched;
        int waited;
        int received = 0;

        /* Cyclone's reader has to know our writer before the burst: an
         * unmatched reader discards the fragments and never NACKs them. */
        memset(&matched, 0, sizeof(matched));
        for (waited = 0; waited < MATRIX_DEADLINE_MS; waited += 50) {
            (void)dds_get_subscription_matched_status(cy_reader, &matched);
            if (matched.current_count > 0) {
                break;
            }
            ntk_sleep_ms(50);
        }
        CHECK(matched.current_count > 0);
        ntk_sleep_ms(200);
        CHECK(matrix_nb_publish(&s, &pub.endpoint_guid, to_cyclone, big_len));
        CHECK(matrix_cy_take(cy_reader, to_cyclone, big_len, MATRIX_DEADLINE_MS));
        {
            dds_subscription_matched_status_t after;

            memset(&after, 0, sizeof(after));
            (void)dds_get_subscription_matched_status(cy_reader, &after);
            nb_log_info("matrix[%u] after the large take: matched current=%u total=%u",
                        (unsigned)domain,
                        after.current_count,
                        after.total_count);
        }

        matrix_inbound_reset(&s);
        for (waited = 0; waited < MATRIX_DEADLINE_MS; waited += 100) {
            (void)matrix_cy_write(cy_writer, to_nb, big_len);
            ntk_sleep_ms(100);
            ntk_mutex_lock(s.inbound.lock);
            received = s.inbound.received && s.inbound.payload_len == big_len &&
                       s.inbound.hash == payload_hash(to_nb, big_len);
            ntk_mutex_unlock(s.inbound.lock);
            if (received) {
                break;
            }
        }
        CHECK(received);
        if (!received) {
            nb_log_error("domain %u: nb did not reassemble the %zu-byte sample",
                         (unsigned)domain,
                         big_len);
        }
    }
    free(to_cyclone);
    free(to_nb);
    matrix_stop(&s);
    nb_log_info("interop matrix: large samples domain %u", (unsigned)domain);
}

static void test_cyclone_matrix(void)
{
    const char *enabled = getenv("NB_TEST_RTPS_CYCLONE");
    const char *advertise_host = getenv("NB_TEST_RTPS_ADVERTISE_HOST");
    const char *unicast_only_env = getenv("NB_TEST_RTPS_UNICAST_ONLY");
    const char *only = getenv("NB_TEST_RTPS_MATRIX");
    const char *addr =
        (advertise_host && advertise_host[0]) ? advertise_host : "127.0.0.1";
    int unicast_only = unicast_only_env && strcmp(unicast_only_env, "1") == 0;

    if (!enabled || strcmp(enabled, "1") != 0) {
        return;
    }
    /* NB_TEST_RTPS_MATRIX=<substring> runs a subset, which keeps a single
     * scenario under the debugger without paying for the whole matrix. */
#define MATRIX_RUN(name, call)                       \
    do {                                             \
        if (!only || strstr(only, name) != NULL) {   \
            call;                                    \
        }                                            \
    } while (0)
    /* Domains are chosen away from 0 so the base scenario's participants have
     * already released their ports even when it failed halfway through. */
    MATRIX_RUN("best", matrix_best_effort(addr, unicast_only, 11));
    MATRIX_RUN("tl", matrix_late_join_cyclone_writer(addr, unicast_only, 12));
    MATRIX_RUN("tl", matrix_late_join_nb_writer(addr, unicast_only, 13));
    MATRIX_RUN("multi", matrix_multi_reader(addr, unicast_only, 14));
    MATRIX_RUN("restart", matrix_restart(addr, unicast_only, 15));
    MATRIX_RUN("large", matrix_large_samples(addr, unicast_only, 16));
#undef MATRIX_RUN
}

int main(void)
{
    if (getenv("NB_TEST_LOG_DEBUG")) {
        nb_log_configure(true, NB_LOG_LEVEL_DEBUG);
    }
    test_cyclone_spdp();
    test_cyclone_matrix();
    if (g_fail) {
        nb_log_error("rtps Cyclone interop test failed");
        return 1;
    }
    nb_log_info("rtps Cyclone interop test passed");
    return 0;
}
