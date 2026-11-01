#define _CRT_SECURE_NO_WARNINGS

/*
 * Tests for the public rtps_* interface. This file includes only <rtps/rtps.h>
 * (plus ntk for sleeping), which is the point of the API layer: no engine
 * header and no nb_* type is needed.
 */

#include <rtps/rtps.h>

#include <ntk.h>

#include <stdio.h>
#include <stdlib.h>
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
    uint8_t last[64];
    size_t last_len;
    rtps_sample_info_t last_info;
    uint8_t last_handle[RTPS_INSTANCE_HANDLE_SIZE];
    int last_has_handle;
} rx_state_t;

static void on_data(void *user, rtps_reader_t *reader, const rtps_sample_t *samples,
                    size_t count)
{
    rx_state_t *rx = (rx_state_t *)user;
    size_t i;

    (void)reader;
    if (!rx) {
        return;
    }
    ntk_mutex_lock(rx->lock);
    for (i = 0; i < count; i++) {
        size_t copy = samples[i].size;

        if (copy > sizeof(rx->last)) {
            copy = sizeof(rx->last);
        }
        if (copy > 0 && samples[i].data) {
            memcpy(rx->last, samples[i].data, copy);
        }
        rx->last_len = samples[i].size;
        rx->last_info = samples[i].info;
        rx->last_has_handle = samples[i].info.has_instance_handle;
        if (samples[i].info.has_instance_handle) {
            memcpy(rx->last_handle,
                   samples[i].info.instance_handle.bytes,
                   sizeof(rx->last_handle));
        }
        rx->count++;
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

static void rx_snapshot(rx_state_t *rx, rtps_sample_info_t *info, uint8_t *data,
                        size_t *len, uint8_t *handle, int *has_handle)
{
    ntk_mutex_lock(rx->lock);
    *info = rx->last_info;
    *len = rx->last_len;
    if (data && *len <= sizeof(rx->last)) {
        memcpy(data, rx->last, *len);
    }
    if (handle) {
        memcpy(handle, rx->last_handle, sizeof(rx->last_handle));
    }
    if (has_handle) {
        *has_handle = rx->last_has_handle;
    }
    ntk_mutex_unlock(rx->lock);
}

static int wait_for(int (*predicate)(void *), void *arg, int timeout_ms)
{
    uint64_t start = ntk_wall_ms();

    for (;;) {
        if (predicate(arg)) {
            return 1;
        }
        if (ntk_wall_ms() - start >= (uint64_t)timeout_ms) {
            return 0;
        }
        ntk_sleep_ms(20);
    }
}

static int pred_writer_matched(void *arg)
{
    return rtps_writer_matched_count((rtps_writer_t *)arg) > 0;
}

static int pred_reader_matched(void *arg)
{
    return rtps_reader_matched_count((rtps_reader_t *)arg) > 0;
}

static int pred_rx1(void *arg)
{
    return rx_count((rx_state_t *)arg) >= 1;
}

static int pred_size3(void *arg)
{
    return rtps_reader_size((rtps_reader_t *)arg) >= 3;
}

static int pred_keyed_alive(void *arg)
{
    rx_state_t *rx = (rx_state_t *)arg;
    rtps_sample_info_t info;
    size_t len = 0;

    rx_snapshot(rx, &info, NULL, &len, NULL, NULL);
    return info.valid_data && info.has_instance_handle &&
           info.instance_state == RTPS_INSTANCE_ALIVE;
}

static int make_participant(uint16_t spdp_port, uint16_t unicast_port,
                            rtps_participant_t **out)
{
    rtps_participant_config_t config;

    rtps_participant_config_init(&config);
    config.domain_id = 0;
    config.bind_host = "127.0.0.1";
    config.advertise_host = "127.0.0.1";
    config.multicast_group = "";
    config.spdp_port = spdp_port;
    config.unicast_port = unicast_port;
    config.announce_interval_ms = 200;
    config.lease_ms = 3000;
    config.name = "rtps-api-test";
    return rtps_participant_create(&config, out);
}

static int make_topic(rtps_participant_t *participant, const char *name,
                      const char *type_name, rtps_topic_t **out)
{
    rtps_topic_desc_t desc;

    rtps_topic_desc_init(&desc);
    desc.name = name;
    desc.type_name = type_name;
    return rtps_topic_create(participant, &desc, out);
}

static void drain_history(rtps_reader_t *reader)
{
    rtps_sample_t sample;

    while (rtps_reader_take(reader, NULL, &sample, 1) == 1) {
        rtps_sample_release(&sample, 1);
    }
}

int main(void)
{
    rtps_participant_t *publisher = NULL;
    rtps_participant_t *subscriber = NULL;
    rtps_topic_t *pub_topic = NULL;
    rtps_topic_t *sub_topic = NULL;
    rtps_topic_t *again = NULL;
    rtps_writer_t *writer = NULL;
    rtps_reader_t *reader = NULL;
    rtps_waitset_t *waitset = NULL;
    rtps_topic_desc_t desc;
    rtps_qos_t effective;
    rx_state_t rx;
    int i;

    memset(&rx, 0, sizeof(rx));
    rx.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(rx.lock != NULL);
    if (!rx.lock) {
        return 1;
    }

    CHECK(make_participant(48200, 48201, &publisher) == RTPS_OK);
    CHECK(make_participant(48202, 48203, &subscriber) == RTPS_OK);
    if (!publisher || !subscriber) {
        goto out;
    }
    CHECK(rtps_participant_add_peer(publisher, "127.0.0.1", 48202) == RTPS_OK);
    CHECK(rtps_participant_add_peer(subscriber, "127.0.0.1", 48200) == RTPS_OK);
    CHECK(rtps_participant_start(publisher) == RTPS_OK);
    CHECK(rtps_participant_start(subscriber) == RTPS_OK);

    /* Topic identity: same name+type returns the same handle, a different type
     * is rejected. */
    CHECK(make_topic(publisher, "api/other", "test::Data", &again) == RTPS_OK);
    {
        rtps_topic_t *same = NULL;
        rtps_topic_desc_t bad;

        CHECK(make_topic(publisher, "api/other", "test::Data", &same) == RTPS_OK);
        CHECK(same == again);
        rtps_topic_desc_init(&bad);
        bad.name = "api/other";
        bad.type_name = "test::Other";
        CHECK(rtps_topic_create(publisher, &bad, &same) == RTPS_ERR_PRECONDITION);
    }
    CHECK(rtps_topic_destroy(again) == RTPS_OK);
    again = NULL;
    CHECK(rtps_topic_find(publisher, "api/other", &again) == RTPS_ERR_NOTFOUND);

    /* The topic carries the default QoS for its endpoints. */
    rtps_topic_desc_init(&desc);
    desc.name = "api/test";
    desc.type_name = "test::Data";
    rtps_qos_set_reliability(&desc.qos, RTPS_RELIABILITY_RELIABLE);
    CHECK(rtps_topic_create(publisher, &desc, &pub_topic) == RTPS_OK);
    CHECK(rtps_topic_create(subscriber, &desc, &sub_topic) == RTPS_OK);
    CHECK(strcmp(rtps_topic_name(pub_topic), "api/test") == 0);
    CHECK(strcmp(rtps_topic_type_name(pub_topic), "test::Data") == 0);

    CHECK(rtps_writer_create(pub_topic, NULL, &writer) == RTPS_OK);
    CHECK(rtps_reader_create(sub_topic, NULL, &reader) == RTPS_OK);
    CHECK(rtps_reader_set_data_callback(reader, on_data, &rx) == RTPS_OK);
    CHECK(rtps_reader_enable_history(reader, 4) == RTPS_OK);
    CHECK(rtps_writer_get_qos(writer, &effective) == RTPS_OK);
    CHECK((effective.present & RTPS_QOS_PRESENT_RELIABILITY) != 0);
    CHECK(effective.reliability == (uint32_t)RTPS_RELIABILITY_RELIABLE);
    CHECK(rtps_reader_get_qos(reader, &effective) == RTPS_OK);
    CHECK(effective.reliability == (uint32_t)RTPS_RELIABILITY_RELIABLE);

    CHECK(wait_for(pred_writer_matched, writer, 5000));
    CHECK(wait_for(pred_reader_matched, reader, 5000));

    /* Data callback and sample info. */
    for (i = 0; i < 100 && rx_count(&rx) == 0; i++) {
        CHECK(rtps_writer_write(writer, "hello", 5) == RTPS_OK);
        ntk_sleep_ms(50);
    }
    CHECK(wait_for(pred_rx1, &rx, 2000));
    {
        rtps_sample_info_t info;
        uint8_t data[64];
        size_t len = 0;

        rx_snapshot(&rx, &info, data, &len, NULL, NULL);
        /* The RTPS submessage is padded to 4 octets, so an opaque CDR body
         * may come back with up to three trailing padding octets. */
        CHECK(len >= 5);
        CHECK(len >= 5 && memcmp(data, "hello", 5) == 0);
        CHECK(info.valid_data);
        CHECK(info.writer_sn > 0);
        CHECK(info.reception_timestamp_ns > 0);
    }
    drain_history(reader);

    /* History: read keeps samples and marks them READ, take owns them. */
    CHECK(rtps_writer_write(writer, "one", 3) == RTPS_OK);
    CHECK(rtps_writer_write(writer, "two", 3) == RTPS_OK);
    CHECK(rtps_writer_write(writer, "tri", 3) == RTPS_OK);
    CHECK(wait_for(pred_size3, reader, 5000));
    {
        rtps_sample_t samples[4];
        rtps_sample_filter_t filter;
        size_t n;

        rtps_sample_filter_init(&filter);
        n = rtps_reader_read(reader, &filter, samples, 2);
        CHECK(n == 2);
        filter.sample_state_mask = RTPS_SAMPLE_STATE_READ_BIT;
        CHECK(rtps_reader_match_count(reader, &filter) == 2);
        filter.sample_state_mask = RTPS_SAMPLE_STATE_NOT_READ_BIT;
        CHECK(rtps_reader_match_count(reader, &filter) == 1);
        n = rtps_reader_take(reader, NULL, samples, 4);
        CHECK(n == 3);
        rtps_sample_release(samples, n);
        CHECK(rtps_reader_size(reader) == 0);
    }

    /* Wait set wakes on a new sample. */
    CHECK(rtps_waitset_create(subscriber, &waitset) == RTPS_OK);
    {
        rtps_sample_filter_t filter;

        rtps_sample_filter_init(&filter);
        CHECK(rtps_waitset_attach(waitset, reader, &filter) == RTPS_OK);
    }
    CHECK(rtps_writer_write(writer, "wake", 4) == RTPS_OK);
    {
        rtps_reader_t *ready[2];
        int count = rtps_waitset_wait(waitset, ready, 2, 3000);

        CHECK(count == 1);
        CHECK(count == 1 && ready[0] == reader);
    }
    drain_history(reader);
    CHECK(rtps_waitset_detach(waitset, reader) == RTPS_OK);
    {
        rtps_reader_t *none[1];

        CHECK(rtps_waitset_wait(waitset, none, 1, 100) == 0);
    }

    /* Keyed instance: a live sample carries the handle, dispose keeps it. */
    {
        rtps_instance_handle_t handle;
        rtps_sample_info_t info;
        uint8_t data[64];
        uint8_t got_handle[RTPS_INSTANCE_HANDLE_SIZE];
        size_t len = 0;
        int has_handle = 0;

        memset(&handle, 0, sizeof(handle));
        for (i = 0; i < RTPS_INSTANCE_HANDLE_SIZE; i++) {
            handle.bytes[i] = (uint8_t)(0xa0 + i);
        }
        CHECK(rtps_writer_write_instance(writer, &handle, RTPS_INSTANCE_ALIVE,
                                         "keyed", 5) == RTPS_OK);
        CHECK(wait_for(pred_keyed_alive, &rx, 5000));
        rx_snapshot(&rx, &info, data, &len, got_handle, &has_handle);
        CHECK(has_handle);
        CHECK(memcmp(got_handle, handle.bytes, sizeof(got_handle)) == 0);
        CHECK(info.instance_state == RTPS_INSTANCE_ALIVE);

        CHECK(rtps_writer_write_instance(writer, &handle,
                                         RTPS_INSTANCE_NOT_ALIVE_DISPOSED,
                                         NULL, 0) == RTPS_OK);
        for (i = 0; i < 100; i++) {
            rx_snapshot(&rx, &info, data, &len, got_handle, &has_handle);
            if (!info.valid_data &&
                info.instance_state == RTPS_INSTANCE_NOT_ALIVE_DISPOSED) {
                break;
            }
            ntk_sleep_ms(50);
        }
        CHECK(!info.valid_data);
        CHECK(info.instance_state == RTPS_INSTANCE_NOT_ALIVE_DISPOSED);
        CHECK(memcmp(got_handle, handle.bytes, sizeof(got_handle)) == 0);
    }

    /* Discovery snapshot and metrics. */
    {
        rtps_discovery_sample_t entries[8];
        size_t count = rtps_participant_discovery_read(
            publisher, RTPS_BUILTIN_TOPIC_PUBLICATION, entries, 8);
        size_t j;
        int saw_local = 0;

        CHECK(count >= 1);
        for (j = 0; j < count; j++) {
            if (entries[j].is_local && entries[j].is_writer &&
                strcmp(entries[j].name, "api/test") == 0) {
                saw_local = 1;
            }
        }
        CHECK(saw_local);
    }
    {
        rtps_metrics_t metrics;
        rtps_guid_t guid;

        memset(&metrics, 0, sizeof(metrics));
        CHECK(rtps_participant_get_guid(publisher, &guid) == RTPS_OK);
        CHECK(rtps_participant_metrics(publisher, &metrics) == RTPS_OK);
        CHECK(metrics.packets_sent > 0);
        CHECK(metrics.data_sent > 0);
        CHECK(metrics.publications >= 1);
        CHECK(rtps_participant_metrics(subscriber, &metrics) == RTPS_OK);
        CHECK(metrics.data_received > 0);
        CHECK(metrics.subscriptions >= 1);
    }

    /* Batch write and the loan path. */
    drain_history(reader);
    {
        rtps_payload_t batch[3];
        rtps_sample_t samples[4];
        size_t n;

        batch[0].data = "b0";
        batch[0].size = 2;
        batch[1].data = "b1";
        batch[1].size = 2;
        batch[2].data = "b2";
        batch[2].size = 2;
        CHECK(rtps_writer_write_batch(writer, batch, 3) == RTPS_OK);
        CHECK(wait_for(pred_size3, reader, 5000));
        n = rtps_reader_loan(reader, NULL, samples, 2);
        CHECK(n == 2);
        rtps_reader_return_loan(reader, samples, n);
        n = rtps_reader_take(reader, NULL, samples, 4);
        CHECK(n == 3);
        rtps_sample_release(samples, n);
        CHECK(rtps_reader_size(reader) == 0);
    }

    /* Publisher/subscriber grouping: QoS inherited through the group, and the
     * group owns its endpoints. */
    {
        rtps_publisher_t *pub = NULL;
        rtps_subscriber_t *sub = NULL;
        rtps_topic_t *group_pub_topic = NULL;
        rtps_topic_t *group_sub_topic = NULL;
        rtps_writer_t *group_writer = NULL;
        rtps_reader_t *group_reader = NULL;
        rtps_qos_t group_qos;
        rtps_qos_t got;
        rtps_topic_desc_t group_desc;

        rtps_qos_init(&group_qos);
        rtps_qos_set_partition(&group_qos, "api-group");
        rtps_qos_set_durability(&group_qos, RTPS_DURABILITY_TRANSIENT_LOCAL);
        CHECK(rtps_publisher_create(publisher, &group_qos, &pub) == RTPS_OK);
        CHECK(rtps_subscriber_create(subscriber, &group_qos, &sub) == RTPS_OK);

        rtps_topic_desc_init(&group_desc);
        group_desc.name = "api/group";
        group_desc.type_name = "test::Data";
        CHECK(rtps_topic_create(publisher, &group_desc, &group_pub_topic) == RTPS_OK);
        CHECK(rtps_topic_create(subscriber, &group_desc, &group_sub_topic) == RTPS_OK);
        CHECK(rtps_writer_create_on(pub, group_pub_topic, NULL, &group_writer) == RTPS_OK);
        CHECK(rtps_reader_create_on(sub, group_sub_topic, NULL, &group_reader) == RTPS_OK);
        CHECK(rtps_publisher_writer_count(pub) == 1);
        CHECK(rtps_subscriber_reader_count(sub) == 1);
        CHECK(rtps_writer_get_qos(group_writer, &got) == RTPS_OK);
        CHECK(strcmp(got.partition, "api-group") == 0);
        CHECK((got.present & RTPS_QOS_PRESENT_DURABILITY) != 0);
        CHECK(got.durability == (uint32_t)RTPS_DURABILITY_TRANSIENT_LOCAL);
        CHECK(rtps_reader_get_qos(group_reader, &got) == RTPS_OK);
        CHECK(strcmp(got.partition, "api-group") == 0);

        CHECK(rtps_publisher_destroy(pub) == RTPS_OK);
        pub = NULL;
        group_writer = NULL;
        CHECK(rtps_subscriber_destroy(sub) == RTPS_OK);
        sub = NULL;
        group_reader = NULL;
        CHECK(rtps_topic_destroy(group_pub_topic) == RTPS_OK);
        group_pub_topic = NULL;
        CHECK(rtps_topic_destroy(group_sub_topic) == RTPS_OK);
        group_sub_topic = NULL;
    }

    /* DCPSTopic snapshot: the local writer and the discovered reader. */
    {
        rtps_discovery_sample_t topics[8];
        size_t count = rtps_participant_discovery_read(
            publisher, RTPS_BUILTIN_TOPIC_TOPIC, topics, 8);
        size_t j;
        int saw_topic = 0;

        CHECK(count >= 1);
        for (j = 0; j < count; j++) {
            if (strcmp(topics[j].name, "api/test") == 0 &&
                topics[j].writer_count >= 1 && topics[j].reader_count >= 1) {
                saw_topic = 1;
            }
        }
        CHECK(saw_topic);
    }

    /* Destroying the topic cascades to its endpoints. */
    CHECK(rtps_topic_destroy(pub_topic) == RTPS_OK);
    pub_topic = NULL;
    writer = NULL;
    CHECK(rtps_topic_destroy(sub_topic) == RTPS_OK);
    sub_topic = NULL;
    reader = NULL;

out:
    if (waitset) {
        rtps_waitset_destroy(waitset);
    }
    if (publisher) {
        rtps_participant_destroy(publisher);
    }
    if (subscriber) {
        rtps_participant_destroy(subscriber);
    }
    ntk_mutex_destroy(rx.lock);
    if (g_fail) {
        fprintf(stderr, "rtps api test failed\n");
        return 1;
    }
    printf("rtps api test passed\n");
    return 0;
}
