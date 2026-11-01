#include "rtps_param.h"
#include "rtps_sedp.h"

#define NB_LOG_TAG "test"
#include <nb/log.h>

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            nb_log_error("FAIL %s:%d: %s", __FILE__, __LINE__, #cond);  \
            g_fail = 1;                                                 \
        }                                                               \
    } while (0)

static void check_reliability_parameter(const uint8_t *buf, size_t len, uint32_t kind)
{
    nb_rtps_param_reader_t reader;
    const uint8_t *value = NULL;
    size_t value_len = 0;
    uint16_t pid = 0;
    int found = 0;
    int rc;

    nb_rtps_param_reader_init(&reader, buf + 4, len - 4);
    for (;;) {
        rc = nb_rtps_param_reader_next(&reader, &pid, &value, &value_len);
        if (rc <= 0) {
            break;
        }
        if (pid == NB_RTPS_PID_RELIABILITY) {
            found = 1;
            CHECK(value_len == 12);
            CHECK(value && value[0] == (uint8_t)(kind & 0xffu));
            CHECK(value && value[1] == (uint8_t)((kind >> 8) & 0xffu));
            CHECK(value && value[4] == 0 && value[5] == 0 && value[6] == 0 && value[7] == 0);
            CHECK(value && value[8] == 0 && value[9] == 0 && value[10] == 0 && value[11] == 0);
        }
    }
    CHECK(found);
}

static void check_locator_parameters(const uint8_t *buf,
                                     size_t len,
                                     uint16_t wanted_pid,
                                     int wanted_count)
{
    nb_rtps_param_reader_t reader;
    const uint8_t *value = NULL;
    size_t value_len = 0;
    uint16_t pid = 0;
    int found = 0;
    int rc;

    nb_rtps_param_reader_init(&reader, buf + 4, len - 4);
    for (;;) {
        rc = nb_rtps_param_reader_next(&reader, &pid, &value, &value_len);
        if (rc <= 0) {
            break;
        }
        if (pid == wanted_pid) {
            found++;
            CHECK(value_len == 24);
            CHECK(value != NULL);
        }
    }
    CHECK(found == wanted_count);
}

static int find_parameter(const uint8_t *buf,
                          size_t len,
                          uint16_t wanted_pid,
                          const uint8_t **value,
                          size_t *value_len)
{
    nb_rtps_param_reader_t reader;
    uint16_t pid = 0;
    const uint8_t *current = NULL;
    size_t current_len = 0;

    nb_rtps_param_reader_init(&reader, buf + 4, len - 4);
    for (;;) {
        int rc = nb_rtps_param_reader_next(&reader, &pid, &current, &current_len);
        if (rc <= 0) {
            return 0;
        }
        if (pid == wanted_pid) {
            if (value) {
                *value = current;
            }
            if (value_len) {
                *value_len = current_len;
            }
            return 1;
        }
    }
}

static void test_endpoint_roundtrip(void)
{
    uint8_t buf[1024];
    nb_rtps_endpoint_info_t info;
    nb_rtps_endpoint_info_t parsed;
    size_t len = 0;
    const uint8_t *value = NULL;
    size_t value_len = 0;

    memset(&info, 0, sizeof(info));
    info.endpoint_guid.prefix.bytes[0] = 0x12;
    info.endpoint_guid.prefix.bytes[11] = 0x34;
    info.endpoint_guid.entity = nb_rtps_eid_sedp_publications_writer;
    memcpy(info.topic_name, "nb/test", 8);
    memcpy(info.type_name, "nb::Test", 9);
    info.reliability = NB_RTPS_RELIABILITY_RELIABLE;
    info.durability = NB_RTPS_DURABILITY_TRANSIENT_LOCAL;
    info.history_kind = NB_RTPS_HISTORY_KEEP_LAST;
    info.history_depth = 4;
    memcpy(info.partition, "part-a", 7);
    info.unicast_count = 2;
    nb_rtps_locator_ipv4(&info.unicast[0], 192, 168, 1, 10, 7400);
    nb_rtps_locator_ipv4(&info.unicast[1], 192, 168, 1, 11, 7401);
    info.multicast_count = 1;
    nb_rtps_locator_spdp_multicast(&info.multicast[0], 7400);
    info.user_data[0] = 0xde;
    info.user_data[1] = 0xad;
    info.user_data[2] = 0xbe;
    info.user_data[3] = 0xef;
    info.user_data_len = 4;
    info.deadline_ms = 250;
    info.latency_budget_ms = 5;
    info.lifespan_ms = 2500;
    info.min_separation_ms = 20;
    info.liveliness_kind = NB_RTPS_LIVELINESS_MANUAL_BY_TOPIC;
    info.liveliness_lease_ms = 1500;
    info.ownership = NB_RTPS_OWNERSHIP_EXCLUSIVE;
    info.ownership_strength = 3;
    info.destination_order = NB_RTPS_DESTINATION_ORDER_BY_SOURCE;
    info.max_samples = 100;
    info.max_instances = 7;
    info.max_samples_per_instance = 5;
    info.presentation_scope = NB_RTPS_PRESENTATION_GROUP;
    info.coherent_access = 1;
    info.ordered_access = 1;

    CHECK(nb_rtps_sedp_build(&info, buf, sizeof(buf), &len) == NB_OK);
    CHECK(len >= 4 && len % 4 == 0);
    CHECK(buf[0] == 0x00);
    CHECK(buf[1] == (uint8_t)NB_RTPS_ENCAPSULATION_PL_CDR_LE);
    CHECK(buf[2] == 0x00 && buf[3] == 0x00);
    check_reliability_parameter(buf, len, NB_RTPS_RELIABILITY_RELIABLE);
    check_locator_parameters(buf, len, NB_RTPS_PID_UNICAST_LOCATOR, 2);
    check_locator_parameters(buf, len, NB_RTPS_PID_MULTICAST_LOCATOR, 1);

    CHECK(nb_rtps_sedp_parse(buf, len, &parsed) == NB_OK);
    CHECK(nb_rtps_guid_equal(&parsed.endpoint_guid, &info.endpoint_guid));
    CHECK(strcmp(parsed.topic_name, "nb/test") == 0);
    CHECK(strcmp(parsed.type_name, "nb::Test") == 0);
    CHECK(parsed.reliability == NB_RTPS_RELIABILITY_RELIABLE);
    CHECK(parsed.durability == NB_RTPS_DURABILITY_TRANSIENT_LOCAL);
    CHECK(parsed.history_kind == NB_RTPS_HISTORY_KEEP_LAST);
    CHECK(parsed.history_depth == 4);
    CHECK(strcmp(parsed.partition, "part-a") == 0);
    CHECK(parsed.unicast_count == 2);
    CHECK(parsed.unicast[0].port == 7400);
    CHECK(parsed.unicast[0].address[12] == 192);
    CHECK(parsed.unicast[0].address[15] == 10);
    CHECK(parsed.unicast[1].port == 7401);
    CHECK(parsed.unicast[1].address[12] == 192);
    CHECK(parsed.unicast[1].address[15] == 11);
    CHECK(parsed.multicast_count == 1);
    CHECK(parsed.multicast[0].port == 7400);
    CHECK(parsed.multicast[0].address[12] == 239);
    CHECK(parsed.user_data_len == 4);
    CHECK(memcmp(parsed.user_data, info.user_data, 4) == 0);
    CHECK(parsed.deadline_ms == 250);
    CHECK(parsed.latency_budget_ms == 5);
    CHECK(parsed.lifespan_ms == 2500);
    CHECK(parsed.min_separation_ms == 20);
    CHECK(parsed.liveliness_kind == NB_RTPS_LIVELINESS_MANUAL_BY_TOPIC);
    CHECK(parsed.liveliness_lease_ms == 1500);
    CHECK(parsed.ownership == NB_RTPS_OWNERSHIP_EXCLUSIVE);
    CHECK(parsed.ownership_strength == 3);
    CHECK(parsed.destination_order == NB_RTPS_DESTINATION_ORDER_BY_SOURCE);
    CHECK(parsed.max_samples == 100);
    CHECK(parsed.max_instances == 7);
    CHECK(parsed.max_samples_per_instance == 5);
    CHECK(parsed.presentation_scope == NB_RTPS_PRESENTATION_GROUP);
    CHECK(parsed.coherent_access == 1);
    CHECK(parsed.ordered_access == 1);

    /* PID_PRESENTATION is the 4-byte access scope followed by the two flags,
     * the layout the DDS spec fixes (0x0021, the id Cyclone DDS uses). */
    CHECK(find_parameter(buf, len, NB_RTPS_PID_PRESENTATION, &value, &value_len));
    CHECK(value_len == 8);
    CHECK(value[0] == NB_RTPS_PRESENTATION_GROUP);
    CHECK(value[1] == 0 && value[2] == 0 && value[3] == 0);
    CHECK(value[4] == 1 && value[5] == 1);

    /* The deadline travels as a Duration: 0 s + 250 ms, where the fraction is
     * in units of 2^-32 s (0.25 * 2^32 = 0x40000000). */
    CHECK(find_parameter(buf, len, NB_RTPS_PID_DEADLINE, &value, &value_len));
    CHECK(value_len == 8);
    CHECK(value[0] == 0 && value[1] == 0 && value[2] == 0 && value[3] == 0);
    CHECK(value[4] == 0x00 && value[5] == 0x00 && value[6] == 0x00 && value[7] == 0x40);
    /* PID_LIVELINESS is kind + lease in one parameter: 1 s + 500 ms
     * (0.5 * 2^32 = 0x80000000). */
    CHECK(find_parameter(buf, len, NB_RTPS_PID_LIVELINESS, &value, &value_len));
    CHECK(value_len == 12);
    CHECK(value[0] == NB_RTPS_LIVELINESS_MANUAL_BY_TOPIC);
    CHECK(value[4] == 1 && value[5] == 0 && value[6] == 0 && value[7] == 0);
    CHECK(value[8] == 0x00 && value[9] == 0x00 && value[10] == 0x00 && value[11] == 0x80);

    /* PID_TIME_BASED_FILTER is a Duration like deadline: 0 s + 20 ms, and
     * selected by the same numeric id Cyclone DDS uses (0x0004). */
    CHECK(find_parameter(
        buf, len, NB_RTPS_PID_TIME_BASED_FILTER, &value, &value_len));
    CHECK(value_len == 8);
    CHECK(value[0] == 0 && value[1] == 0 && value[2] == 0 && value[3] == 0);
    CHECK(value[4] == 0x51 && value[5] == 0xb8 && value[6] == 0x1e && value[7] == 0x05);

    /* PID_RESOURCE_LIMITS travels as the three DDS uint32 values. */
    CHECK(find_parameter(buf, len, NB_RTPS_PID_RESOURCE_LIMITS, &value, &value_len));
    CHECK(value_len == 12);
    CHECK(value[0] == 100 && value[1] == 0 && value[2] == 0 && value[3] == 0);
    CHECK(value[4] == 7 && value[5] == 0 && value[6] == 0 && value[7] == 0);
    CHECK(value[8] == 5 && value[9] == 0 && value[10] == 0 && value[11] == 0);

    /* Truncated payload is rejected. */
    CHECK(nb_rtps_sedp_parse(buf, len - 2, &parsed) != NB_OK);
}

static void test_endpoint_defaults(void)
{
    uint8_t buf[512];
    nb_rtps_endpoint_info_t info;
    nb_rtps_endpoint_info_t parsed;
    size_t len = 0;

    memset(&info, 0, sizeof(info));
    info.endpoint_guid.prefix.bytes[0] = 0x56;
    info.endpoint_guid.entity = nb_rtps_eid_sedp_subscriptions_reader;
    memcpy(info.topic_name, "nb/minimal", 11);
    memcpy(info.type_name, "nb::Minimal", 12);

    CHECK(nb_rtps_sedp_build(&info, buf, sizeof(buf), &len) == NB_OK);
    CHECK(nb_rtps_sedp_parse(buf, len, &parsed) == NB_OK);
    CHECK(parsed.reliability == NB_RTPS_RELIABILITY_BEST_EFFORT);
    CHECK(parsed.durability == NB_RTPS_DURABILITY_VOLATILE);
    CHECK(parsed.history_kind == NB_RTPS_HISTORY_KEEP_LAST);
    CHECK(parsed.history_depth == 1);
    CHECK(parsed.partition[0] == '\0');
    CHECK(parsed.unicast_count == 0);
    CHECK(parsed.multicast_count == 0);
    CHECK(parsed.user_data_len == 0);
    /* Unset policies stay absent and are not announced. */
    CHECK(parsed.deadline_ms == 0);
    CHECK(parsed.latency_budget_ms == 0);
    CHECK(parsed.lifespan_ms == 0);
    CHECK(parsed.min_separation_ms == 0);
    CHECK(parsed.liveliness_kind == NB_RTPS_LIVELINESS_AUTOMATIC);
    CHECK(parsed.liveliness_lease_ms == 0);
    CHECK(parsed.ownership == NB_RTPS_OWNERSHIP_SHARED);
    CHECK(parsed.ownership_strength == 0);
    CHECK(parsed.destination_order == NB_RTPS_DESTINATION_ORDER_BY_RECEPTION);
    CHECK(parsed.max_samples == 0);
    CHECK(parsed.max_instances == 0);
    CHECK(parsed.max_samples_per_instance == 0);
    CHECK(!find_parameter(buf, len, NB_RTPS_PID_DEADLINE, NULL, NULL));
    CHECK(!find_parameter(buf, len, NB_RTPS_PID_TIME_BASED_FILTER, NULL, NULL));
    CHECK(!find_parameter(buf, len, NB_RTPS_PID_LIVELINESS, NULL, NULL));
    CHECK(!find_parameter(buf, len, NB_RTPS_PID_OWNERSHIP, NULL, NULL));
    CHECK(!find_parameter(buf, len, NB_RTPS_PID_RESOURCE_LIMITS, NULL, NULL));
}

static void test_endpoint_missing_required(void)
{
    uint8_t buf[256];
    nb_rtps_param_writer_t writer;
    nb_rtps_endpoint_info_t parsed;

    nb_rtps_param_writer_init(&writer, buf, sizeof(buf));
    CHECK(nb_rtps_param_put_encapsulation(&writer) == NB_OK);
    CHECK(nb_rtps_param_put_string(&writer, NB_RTPS_PID_TOPIC_NAME, "only-topic") == NB_OK);
    CHECK(nb_rtps_param_writer_finish(&writer) == NB_OK);
    CHECK(nb_rtps_sedp_parse(buf, nb_rtps_param_writer_len(&writer), &parsed) ==
          NB_ERR_BAD_MESSAGE);
}

int main(void)
{
    test_endpoint_roundtrip();
    test_endpoint_defaults();
    test_endpoint_missing_required();
    if (g_fail) {
        nb_log_error("rtps sedp tests failed");
        return 1;
    }
    printf("rtps sedp tests passed\n");
    return 0;
}
