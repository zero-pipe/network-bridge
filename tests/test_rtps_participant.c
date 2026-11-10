#define _CRT_SECURE_NO_WARNINGS

#include "rtps_participant.h"
#include "rtps_message.h"
#include "rtps_param.h"

#define NB_LOG_TAG "test"
#include <nb/base/log.h>

#include <ntk.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            nb_log_error("FAIL %s:%d: %s", __FILE__, __LINE__, #cond);  \
            g_fail = 1;                                                 \
        }                                                               \
    } while (0)

typedef struct test_state {
    ntk_mutex *lock;
    int added;
    int updated;
    int removed;
    int endpoint_added;
    int endpoint_updated;
    int endpoint_removed;
    int last_endpoint_kind;
    nb_rtps_endpoint_info_t last_endpoint;
    char last_name[NB_RTPS_SPDP_NAME_MAX];
} test_state_t;

typedef struct test_data_state {
    ntk_mutex *lock;
    int received;
    nb_rtps_guid_t reader_guid;
    nb_rtps_guid_t writer_guid;
    uint8_t payload[NB_RTPS_DATA_PAYLOAD_MAX];
    size_t payload_len;
} test_data_state_t;

typedef struct reliable_data_state {
    ntk_mutex *lock;
    int count;
    uint8_t payloads[8][8];
    size_t payload_lens[8];
    nb_rtps_guid_t reader_guids[8];
    nb_rtps_guid_t writer_guids[8];
    nb_sample_state_t states[8];
    int has_handle[8];
    uint8_t handles[8][NB_INSTANCE_HANDLE_SIZE];
    int64_t source_ts[8];
    int64_t reception_ts[8];
    nb_instance_state_t instance_states[8];
    nb_view_state_t view_states[8];
    int32_t generation_ranks[8];
} reliable_data_state_t;

static void on_participant(void *user,
                           nb_rtps_participant_event_t event,
                           const nb_rtps_participant_info_t *info)
{
    test_state_t *state = (test_state_t *)user;

    ntk_mutex_lock(state->lock);
    if (event == NB_RTPS_PARTICIPANT_ADDED) {
        state->added++;
        (void)snprintf(state->last_name, sizeof(state->last_name), "%s", info->name);
    } else if (event == NB_RTPS_PARTICIPANT_UPDATED) {
        state->updated++;
    } else if (event == NB_RTPS_PARTICIPANT_REMOVED) {
        state->removed++;
    }
    ntk_mutex_unlock(state->lock);
}

static void on_endpoint(void *user,
                        nb_rtps_endpoint_event_t event,
                        nb_rtps_endpoint_kind_t kind,
                        const nb_rtps_endpoint_info_t *info)
{
    test_state_t *state = (test_state_t *)user;

    ntk_mutex_lock(state->lock);
    if (event == NB_RTPS_ENDPOINT_ADDED) {
        state->endpoint_added++;
    } else if (event == NB_RTPS_ENDPOINT_UPDATED) {
        state->endpoint_updated++;
    } else if (event == NB_RTPS_ENDPOINT_REMOVED) {
        state->endpoint_removed++;
    }
    state->last_endpoint_kind = (int)kind;
    state->last_endpoint = *info;
    ntk_mutex_unlock(state->lock);
}

static void on_data(void *user,
                    const nb_rtps_guid_t *reader_guid,
                    const nb_rtps_guid_t *writer_guid,
                    const nb_rtps_dds_message_t *message)
{
    test_data_state_t *state = (test_data_state_t *)user;
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

static int data_received(test_data_state_t *state)
{
    int received;

    ntk_mutex_lock(state->lock);
    received = state->received;
    ntk_mutex_unlock(state->lock);
    return received;
}

static void on_reliable_data(void *user,
                             const nb_rtps_guid_t *reader_guid,
                             const nb_rtps_guid_t *writer_guid,
                             const nb_rtps_dds_message_t *message)
{
    reliable_data_state_t *state = (reliable_data_state_t *)user;
    int index;

    if (!state || !reader_guid || !writer_guid || !message) {
        return;
    }
    ntk_mutex_lock(state->lock);
    index = state->count;
    if (index < (int)(sizeof(state->payloads) / sizeof(state->payloads[0]))) {
        size_t copy_len = message->payload_len;

        if (copy_len > sizeof(state->payloads[0])) {
            copy_len = sizeof(state->payloads[0]);
        }
        if (copy_len > 0 && message->payload) {
            memcpy(state->payloads[index], message->payload, copy_len);
        }
        state->payload_lens[index] = message->payload_len;
        state->reader_guids[index] = *reader_guid;
        state->writer_guids[index] = *writer_guid;
        state->states[index] = message->sample.state;
        state->has_handle[index] = message->sample.has_instance_handle ? 1 : 0;
        memcpy(state->handles[index],
               message->sample.instance_handle,
               sizeof(state->handles[index]));
        state->source_ts[index] = message->sample.source_timestamp_ns;
        state->reception_ts[index] = message->sample.reception_timestamp_ns;
        state->instance_states[index] = message->sample.instance_state;
        state->view_states[index] = message->sample.view_state;
        state->generation_ranks[index] = message->sample.generation_rank;
    }
    state->count++;
    ntk_mutex_unlock(state->lock);
}

static int reliable_data_count(reliable_data_state_t *state)
{
    int count;

    ntk_mutex_lock(state->lock);
    count = state->count;
    ntk_mutex_unlock(state->lock);
    return count;
}

static int wait_reliable_data(reliable_data_state_t *state, int wanted, int timeout_ms)
{
    int waited = 0;

    while (waited < timeout_ms) {
        if (reliable_data_count(state) >= wanted) {
            return 1;
        }
        ntk_sleep_ms(20);
        waited += 20;
    }
    return 0;
}

/* Inject a GAP datagram from the writer's identity, as an interoperating RTPS
 * peer would when it drops samples from its history. */
static int send_gap_datagram(const nb_rtps_guid_prefix_t *dst_prefix,
                             const nb_rtps_guid_t *writer,
                             const nb_rtps_guid_t *reader,
                             uint32_t gap_start,
                             uint32_t gap_base,
                             uint32_t gap_bits,
                             uint32_t gap_bitmap,
                             uint16_t port)
{
    ntk_socket *sock;
    uint8_t message[256];
    (void)dst_prefix;
    nb_rtps_header_t header;
    nb_rtps_gap_t gap;
    nb_cdr_writer_t cdr;
    size_t message_len;
    int rc = NB_ERR_IO;

    sock = ntk_socket_create();
    if (!sock) {
        return NB_ERR_NOMEM;
    }
    if (ntk_socket_bind_udp(sock, "127.0.0.1", 0, 0) != NTK_OK) {
        ntk_socket_destroy(sock);
        return NB_ERR_IO;
    }
    memset(&header, 0, sizeof(header));
    header.version_major = NB_RTPS_PROTOCOL_VERSION_MAJOR;
    header.version_minor = NB_RTPS_PROTOCOL_VERSION_MINOR;
    header.vendor_id[0] = NB_RTPS_VENDOR_ID_0;
    header.vendor_id[1] = NB_RTPS_VENDOR_ID_1;
    header.guid_prefix = writer->prefix;
    memset(&gap, 0, sizeof(gap));
    gap.reader_id = reader->entity;
    gap.writer_id = writer->entity;
    gap.gap_start.high = 0;
    gap.gap_start.low = gap_start;
    gap.base.high = 0;
    gap.base.low = gap_base;
    gap.num_bits = gap_bits;
    gap.bitmap[0] = gap_bitmap;
    if (nb_rtps_message_init(&cdr, message, sizeof(message), &header) == NB_OK &&
        nb_rtps_info_dst(&cdr, &reader->prefix) == NB_OK &&
        nb_rtps_gap(&cdr, &gap) == NB_OK && nb_cdr_writer_ok(&cdr)) {
        message_len = nb_cdr_writer_len(&cdr);
        if (ntk_socket_sendto(sock, message, message_len, "127.0.0.1", port) > 0) {
            rc = NB_OK;
        }
    }
    ntk_socket_destroy(sock);
    return rc;
}

/*
 * Inject a DATA with the dispose flag from a writer the subscriber has already
 * discovered, the way a peer announces that an instance is gone. The payload is
 * a key-only sample, which this stack does not deliver but must still consume.
 */
/* Instance handle the injected dispose announces in its inline QoS. */
static const uint8_t g_dispose_handle[NB_INSTANCE_HANDLE_SIZE] = {
    0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
    0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0, 0x01};

/* status: 0 = live sample, 1 = dispose, 2 = unregister. Every variant carries
 * the instance handle in the inline QoS, the way a keyed writer does. */
static int send_instance_datagram(const nb_rtps_guid_prefix_t *dst_prefix,
                                  const nb_rtps_guid_t *writer,
                                  const nb_rtps_guid_t *reader,
                                  uint32_t sn,
                                  const uint8_t *handle,
                                  int status,
                                  uint16_t port)
{
    ntk_socket *sock;
    uint8_t message[256];
    (void)dst_prefix;
    uint8_t key[4] = {0x00, 0x01, 0x00, 0x00};
    /* Encapsulated dds::Message with a one-byte payload. */
    uint8_t value[9] = {0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x42};
    uint8_t qos[64];
    uint8_t status_info[4] = {0x00, 0x00, 0x00, (uint8_t)status};
    nb_rtps_param_writer_t params;
    size_t qos_len = 0;
    nb_rtps_header_t header;
    nb_rtps_data_t data;
    nb_cdr_writer_t cdr;
    size_t message_len;
    int rc = NB_ERR_IO;

    /* The inline QoS a real writer sends with a dispose: the instance handle
     * and the status bits. */
    nb_rtps_param_writer_init(&params, qos, sizeof(qos));
    if (nb_rtps_param_put_bytes(&params,
                                NB_RTPS_PID_KEY_HASH,
                                handle,
                                NB_INSTANCE_HANDLE_SIZE) != NB_OK ||
        (status != 0 &&
         nb_rtps_param_put_bytes(&params,
                                 NB_RTPS_PID_STATUS_INFO,
                                 status_info,
                                 sizeof(status_info)) != NB_OK) ||
        nb_rtps_param_writer_finish(&params) != NB_OK) {
        return NB_ERR_IO;
    }
    qos_len = nb_rtps_param_writer_len(&params);

    sock = ntk_socket_create();
    if (!sock) {
        return NB_ERR_NOMEM;
    }
    if (ntk_socket_bind_udp(sock, "127.0.0.1", 0, 0) != NTK_OK) {
        ntk_socket_destroy(sock);
        return NB_ERR_IO;
    }
    memset(&header, 0, sizeof(header));
    header.version_major = NB_RTPS_PROTOCOL_VERSION_MAJOR;
    header.version_minor = NB_RTPS_PROTOCOL_VERSION_MINOR;
    header.vendor_id[0] = NB_RTPS_VENDOR_ID_0;
    header.vendor_id[1] = NB_RTPS_VENDOR_ID_1;
    header.guid_prefix = writer->prefix;
    memset(&data, 0, sizeof(data));
    data.reader_id = reader->entity;
    data.writer_id = writer->entity;
    data.writer_sn.high = 0;
    data.writer_sn.low = sn;
    data.payload = status != 0 ? key : value;
    data.payload_len = status != 0 ? sizeof(key) : sizeof(value);
    data.inline_qos = qos;
    data.inline_qos_len = qos_len;
    data.dispose = status == 1;
    data.key = status != 0;
    if (nb_rtps_message_init(&cdr, message, sizeof(message), &header) == NB_OK &&
        nb_rtps_info_dst(&cdr, &reader->prefix) == NB_OK && nb_rtps_data(&cdr, &data) == NB_OK &&
        nb_cdr_writer_ok(&cdr)) {
        message_len = nb_cdr_writer_len(&cdr);
        if (ntk_socket_sendto(sock, message, message_len, "127.0.0.1", port) > 0) {
            rc = NB_OK;
        }
    }
    ntk_socket_destroy(sock);
    return rc;
}

static int send_dispose_datagram(const nb_rtps_guid_prefix_t *dst_prefix,
                                 const nb_rtps_guid_t *writer,
                                 const nb_rtps_guid_t *reader,
                                 uint32_t sn,
                                 uint16_t port)
{
    return send_instance_datagram(
        dst_prefix, writer, reader, sn, g_dispose_handle, 1, port);
}

static int state_count(test_state_t *state, int removed)
{
    int value;

    ntk_mutex_lock(state->lock);
    value = removed ? state->removed : state->added;
    ntk_mutex_unlock(state->lock);
    return value;
}

static int wait_count(test_state_t *state, int removed, int wanted, int timeout_ms)
{
    int waited = 0;

    while (waited < timeout_ms) {
        if (state_count(state, removed) >= wanted) {
            return 1;
        }
        ntk_sleep_ms(20);
        waited += 20;
    }
    return 0;
}

static int endpoint_count(test_state_t *state, int removed)
{
    int value;

    ntk_mutex_lock(state->lock);
    value = removed ? state->endpoint_removed : state->endpoint_added;
    ntk_mutex_unlock(state->lock);
    return value;
}

static int wait_endpoint_count(test_state_t *state, int removed, int wanted, int timeout_ms)
{
    int waited = 0;

    while (waited < timeout_ms) {
        if (endpoint_count(state, removed) >= wanted) {
            return 1;
        }
        ntk_sleep_ms(20);
        waited += 20;
    }
    return 0;
}

static void fill_config(nb_rtps_participant_config_t *config, const char *name)
{
    memset(config, 0, sizeof(*config));
    config->domain_id = 0;
    config->bind_host = "127.0.0.1";
    config->advertise_host = "127.0.0.1";
    config->multicast_group = ""; /* unicast peers only */
    config->spdp_port = 0;        /* ephemeral */
    config->unicast_port = 0;
    config->announce_interval_ms = 100;
    config->lease_ms = 1200;
    config->name = name;
    config->max_remote_participants = 8;
}

/* Fill a dds::Message endpoint description with the framework defaults. */
static void init_data_endpoint(nb_rtps_endpoint_info_t *info, int reliable)
{
    memset(info, 0, sizeof(*info));
    (void)snprintf(info->topic_name,
                   sizeof(info->topic_name),
                   "%s",
                   NB_RTPS_DDS_MESSAGE_TOPIC);
    (void)snprintf(info->type_name,
                   sizeof(info->type_name),
                   "%s",
                   NB_RTPS_DDS_MESSAGE_TYPE);
    info->reliability =
        reliable ? NB_RTPS_RELIABILITY_RELIABLE : NB_RTPS_RELIABILITY_BEST_EFFORT;
    info->durability = NB_RTPS_DURABILITY_VOLATILE;
    info->history_kind = NB_RTPS_HISTORY_KEEP_LAST;
    info->history_depth = 8;
}

static void test_unicast_peers(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *a;
    nb_rtps_participant_t *b;
    test_state_t state_a;
    test_state_t state_b;
    uint16_t port_a;
    uint16_t port_b;

    memset(&state_a, 0, sizeof(state_a));
    memset(&state_b, 0, sizeof(state_b));

    fill_config(&config, "node-a");
    a = nb_rtps_participant_create(&config);
    CHECK(a != NULL);
    if (!a) {
        return;
    }
    port_a = nb_rtps_participant_spdp_port(a);
    CHECK(port_a != 0);

    fill_config(&config, "node-b");
    config.peers = NULL;
    config.peer_count = 0;
    b = nb_rtps_participant_create(&config);
    CHECK(b != NULL);
    if (!b) {
        nb_rtps_participant_destroy(a);
        return;
    }
    port_b = nb_rtps_participant_spdp_port(b);
    CHECK(port_b != 0);
    CHECK(port_a != port_b);
    CHECK(nb_rtps_participant_add_peer(a, "127.0.0.1", port_b) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(b, "127.0.0.1", port_a) == NB_OK);

    state_a.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    state_b.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(state_a.lock != NULL);
    CHECK(state_b.lock != NULL);
    if (!state_a.lock || !state_b.lock) {
        nb_rtps_participant_destroy(a);
        nb_rtps_participant_destroy(b);
        if (state_a.lock) {
            ntk_mutex_destroy(state_a.lock);
        }
        if (state_b.lock) {
            ntk_mutex_destroy(state_b.lock);
        }
        return;
    }
    nb_rtps_participant_set_callback(a, on_participant, &state_a);
    nb_rtps_participant_set_callback(b, on_participant, &state_b);

    CHECK(nb_rtps_participant_start(a) == NB_OK);
    CHECK(nb_rtps_participant_start(b) == NB_OK);

    CHECK(wait_count(&state_a, 0, 1, 5000));
    CHECK(wait_count(&state_b, 0, 1, 5000));
    CHECK(strcmp(state_a.last_name, "node-b") == 0);
    CHECK(strcmp(state_b.last_name, "node-a") == 0);
    CHECK(nb_rtps_participant_remote_count(a) == 1);
    CHECK(nb_rtps_participant_remote_count(b) == 1);

    nb_rtps_participant_stop(b);
    CHECK(wait_count(&state_a, 1, 1, 5000));
    CHECK(nb_rtps_participant_remote_count(a) == 0);

    nb_rtps_participant_stop(a);
    nb_rtps_participant_destroy(a);
    nb_rtps_participant_destroy(b);
    ntk_mutex_destroy(state_a.lock);
    ntk_mutex_destroy(state_b.lock);
}

static void test_create_destroy(void)
{
    nb_rtps_participant_config_t config;
    int i;

    fill_config(&config, "short-lived");
    for (i = 0; i < 3; i++) {
        nb_rtps_participant_t *participant = nb_rtps_participant_create(&config);
        CHECK(participant != NULL);
        if (!participant) {
            return;
        }
        CHECK(nb_rtps_participant_spdp_port(participant) != 0);
        nb_rtps_participant_destroy(participant);
    }
}

static void test_sedp_endpoints(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *a;
    nb_rtps_participant_t *b;
    nb_rtps_endpoint_info_t publication;
    nb_rtps_endpoint_info_t subscription;
    nb_rtps_participant_info_t info_a;
    test_state_t state_a;
    test_state_t state_b;
    uint16_t port_a;
    uint16_t port_b;

    memset(&state_a, 0, sizeof(state_a));
    memset(&state_b, 0, sizeof(state_b));
    fill_config(&config, "sedp-a");
    a = nb_rtps_participant_create(&config);
    CHECK(a != NULL);
    if (!a) {
        return;
    }
    port_a = nb_rtps_participant_spdp_port(a);
    fill_config(&config, "sedp-b");
    b = nb_rtps_participant_create(&config);
    CHECK(b != NULL);
    if (!b) {
        nb_rtps_participant_destroy(a);
        return;
    }
    port_b = nb_rtps_participant_spdp_port(b);
    CHECK(nb_rtps_participant_add_peer(a, "127.0.0.1", port_b) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(b, "127.0.0.1", port_a) == NB_OK);

    state_a.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    state_b.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(state_a.lock != NULL);
    CHECK(state_b.lock != NULL);
    if (!state_a.lock || !state_b.lock) {
        nb_rtps_participant_destroy(a);
        nb_rtps_participant_destroy(b);
        if (state_a.lock) {
            ntk_mutex_destroy(state_a.lock);
        }
        if (state_b.lock) {
            ntk_mutex_destroy(state_b.lock);
        }
        return;
    }
    nb_rtps_participant_set_callback(a, on_participant, &state_a);
    nb_rtps_participant_set_callback(b, on_participant, &state_b);
    nb_rtps_participant_set_endpoint_callback(a, on_endpoint, &state_a);
    nb_rtps_participant_set_endpoint_callback(b, on_endpoint, &state_b);
    CHECK(nb_rtps_participant_start(a) == NB_OK);
    CHECK(nb_rtps_participant_start(b) == NB_OK);
    CHECK(wait_count(&state_a, 0, 1, 5000));
    CHECK(wait_count(&state_b, 0, 1, 5000));

    memset(&publication, 0, sizeof(publication));
    (void)snprintf(publication.topic_name, sizeof(publication.topic_name), "%s", "nb/sedp");
    (void)snprintf(publication.type_name, sizeof(publication.type_name), "%s", "nb::Sedp");
    publication.reliability = NB_RTPS_RELIABILITY_RELIABLE;
    publication.durability = NB_RTPS_DURABILITY_VOLATILE;
    publication.history_kind = NB_RTPS_HISTORY_KEEP_LAST;
    publication.history_depth = 4;
    CHECK(nb_rtps_participant_add_endpoint(
              a, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);
    CHECK(nb_rtps_participant_local_endpoint_count(a) == 1);
    CHECK(wait_endpoint_count(&state_b, 0, 1, 5000));
    CHECK(state_b.last_endpoint_kind == NB_RTPS_ENDPOINT_PUBLICATION);
    CHECK(strcmp(state_b.last_endpoint.topic_name, "nb/sedp") == 0);
    CHECK(strcmp(state_b.last_endpoint.type_name, "nb::Sedp") == 0);
    CHECK(nb_rtps_participant_remote_endpoint_count(b) == 1);

    memset(&subscription, 0, sizeof(subscription));
    (void)snprintf(subscription.topic_name, sizeof(subscription.topic_name), "%s", "nb/sedp");
    (void)snprintf(subscription.type_name, sizeof(subscription.type_name), "%s", "nb::Sedp");
    subscription.reliability = NB_RTPS_RELIABILITY_BEST_EFFORT;
    subscription.durability = NB_RTPS_DURABILITY_VOLATILE;
    CHECK(nb_rtps_participant_add_endpoint(
              b, NB_RTPS_ENDPOINT_SUBSCRIPTION, &subscription) == NB_OK);
    CHECK(nb_rtps_participant_local_endpoint_count(b) == 1);
    CHECK(wait_endpoint_count(&state_a, 0, 1, 5000));
    CHECK(state_a.last_endpoint_kind == NB_RTPS_ENDPOINT_SUBSCRIPTION);
    CHECK(strcmp(state_a.last_endpoint.topic_name, "nb/sedp") == 0);
    CHECK(nb_rtps_participant_remote_endpoint_count(a) == 1);

    CHECK(nb_rtps_participant_remote_count(a) == 1);
    CHECK(nb_rtps_participant_local_info(a, &info_a) == NB_OK);
    CHECK(nb_rtps_guid_prefix_equal(&state_b.last_endpoint.endpoint_guid.prefix,
                                    &info_a.guid.prefix));

    CHECK(nb_rtps_participant_remove_endpoint(a, &publication.endpoint_guid) == NB_OK);
    CHECK(nb_rtps_participant_local_endpoint_count(a) == 0);
    CHECK(wait_endpoint_count(&state_b, 1, 1, 5000));
    CHECK(nb_rtps_participant_remote_endpoint_count(b) == 0);

    nb_rtps_participant_stop(a);
    nb_rtps_participant_stop(b);
    nb_rtps_participant_destroy(a);
    nb_rtps_participant_destroy(b);
    ntk_mutex_destroy(state_a.lock);
    ntk_mutex_destroy(state_b.lock);
}

static void test_metrics_counters(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *a;
    nb_rtps_participant_t *b;
    nb_rtps_endpoint_info_t publication;
    nb_rtps_endpoint_info_t subscription;
    test_state_t state_a;
    test_state_t state_b;
    nb_metrics_t metrics;
    uint16_t port_a;
    uint16_t port_b;

    memset(&state_a, 0, sizeof(state_a));
    memset(&state_b, 0, sizeof(state_b));
    fill_config(&config, "metrics-a");
    a = nb_rtps_participant_create(&config);
    CHECK(a != NULL);
    if (!a) {
        return;
    }
    port_a = nb_rtps_participant_spdp_port(a);
    fill_config(&config, "metrics-b");
    b = nb_rtps_participant_create(&config);
    CHECK(b != NULL);
    if (!b) {
        nb_rtps_participant_destroy(a);
        return;
    }
    port_b = nb_rtps_participant_spdp_port(b);
    CHECK(nb_rtps_participant_add_peer(a, "127.0.0.1", port_b) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(b, "127.0.0.1", port_a) == NB_OK);
    state_a.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    state_b.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(state_a.lock != NULL);
    CHECK(state_b.lock != NULL);
    if (!state_a.lock || !state_b.lock) {
        nb_rtps_participant_destroy(a);
        nb_rtps_participant_destroy(b);
        ntk_mutex_destroy(state_a.lock);
        ntk_mutex_destroy(state_b.lock);
        return;
    }
    nb_rtps_participant_set_callback(a, on_participant, &state_a);
    nb_rtps_participant_set_callback(b, on_participant, &state_b);
    nb_rtps_participant_set_endpoint_callback(a, on_endpoint, &state_a);
    nb_rtps_participant_set_endpoint_callback(b, on_endpoint, &state_b);
    CHECK(nb_rtps_participant_start(a) == NB_OK);
    CHECK(nb_rtps_participant_start(b) == NB_OK);
    CHECK(wait_count(&state_a, 0, 1, 5000));
    CHECK(wait_count(&state_b, 0, 1, 5000));

    init_data_endpoint(&publication, 1);
    CHECK(nb_rtps_participant_add_endpoint(
              a, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);
    init_data_endpoint(&subscription, 1);
    CHECK(nb_rtps_participant_add_endpoint(
              b, NB_RTPS_ENDPOINT_SUBSCRIPTION, &subscription) == NB_OK);
    CHECK(wait_endpoint_count(&state_b, 0, 1, 5000));
    CHECK(wait_endpoint_count(&state_a, 0, 1, 5000));

    memset(&metrics, 0, sizeof(metrics));
    nb_rtps_participant_metrics(a, &metrics);
    CHECK(metrics.rtps_packets_sent > 0);
    CHECK(metrics.rtps_bytes_sent > 0);
    CHECK(metrics.rtps_spdp_sent > 0);
    CHECK(metrics.rtps_spdp_received > 0);
    CHECK(metrics.rtps_sedp_sent > 0);
    CHECK(metrics.rtps_packets_received > 0);
    CHECK(metrics.rtps_bytes_received > 0);
    CHECK(metrics.rtps_parse_errors == 0);
    CHECK(metrics.rtps_participants == 2);
    CHECK(metrics.rtps_publications == 1);
    CHECK(metrics.rtps_subscriptions == 1);
    if (metrics.rtps_packets_sent == 0) {
        CHECK(0);
    }

    /* The frame dump is a switch, not an accumulation: enabling it must not
     * change the counters it sits next to. */
    {
        nb_metrics_t again;
        nb_rtps_participant_t *dumping;
        uint64_t before;

        before = metrics.rtps_packets_sent;
        fill_config(&config, "metrics-dump");
        config.dump_frames = 1;
        dumping = nb_rtps_participant_create(&config);
        CHECK(dumping != NULL);
        if (dumping) {
            CHECK(nb_rtps_participant_add_peer(dumping, "127.0.0.1", port_a) == NB_OK);
            CHECK(nb_rtps_participant_start(dumping) == NB_OK);
            ntk_sleep_ms(150);
            memset(&again, 0, sizeof(again));
            nb_rtps_participant_metrics(dumping, &again);
            CHECK(again.rtps_packets_sent > 0);
            nb_rtps_participant_stop(dumping);
            nb_rtps_participant_destroy(dumping);
        }
        CHECK(before > 0);
    }

    nb_rtps_participant_stop(a);
    nb_rtps_participant_stop(b);
    nb_rtps_participant_destroy(a);
    nb_rtps_participant_destroy(b);
    ntk_mutex_destroy(state_a.lock);
    ntk_mutex_destroy(state_b.lock);
}

static void test_discovery_views(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *a;
    nb_rtps_participant_t *b;
    nb_rtps_endpoint_info_t publication;
    nb_rtps_endpoint_info_t subscription;
    test_state_t state_a;
    test_state_t state_b;
    nb_dds_discovery_sample_t views[8];
    uint16_t port_a;
    uint16_t port_b;
    size_t n;
    size_t i;
    int local_count;
    int saw_topic_a = 0;
    int saw_topic_b = 0;

    memset(&state_a, 0, sizeof(state_a));
    memset(&state_b, 0, sizeof(state_b));
    fill_config(&config, "view-a");
    a = nb_rtps_participant_create(&config);
    CHECK(a != NULL);
    if (!a) {
        return;
    }
    port_a = nb_rtps_participant_spdp_port(a);
    fill_config(&config, "view-b");
    b = nb_rtps_participant_create(&config);
    CHECK(b != NULL);
    if (!b) {
        nb_rtps_participant_destroy(a);
        return;
    }
    port_b = nb_rtps_participant_spdp_port(b);
    CHECK(nb_rtps_participant_add_peer(a, "127.0.0.1", port_b) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(b, "127.0.0.1", port_a) == NB_OK);

    state_a.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    state_b.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    if (!state_a.lock || !state_b.lock) {
        CHECK(0);
        nb_rtps_participant_destroy(a);
        nb_rtps_participant_destroy(b);
        ntk_mutex_destroy(state_a.lock);
        ntk_mutex_destroy(state_b.lock);
        return;
    }
    nb_rtps_participant_set_callback(a, on_participant, &state_a);
    nb_rtps_participant_set_callback(b, on_participant, &state_b);
    nb_rtps_participant_set_endpoint_callback(a, on_endpoint, &state_a);
    nb_rtps_participant_set_endpoint_callback(b, on_endpoint, &state_b);
    CHECK(nb_rtps_participant_start(a) == NB_OK);
    CHECK(nb_rtps_participant_start(b) == NB_OK);
    CHECK(wait_count(&state_a, 0, 1, 5000));
    CHECK(wait_count(&state_b, 0, 1, 5000));

    init_data_endpoint(&publication, 1);
    (void)snprintf(publication.topic_name, sizeof(publication.topic_name), "%s", "topic.a");
    CHECK(nb_rtps_participant_add_endpoint(
              a, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);
    init_data_endpoint(&subscription, 0);
    (void)snprintf(subscription.topic_name, sizeof(subscription.topic_name), "%s", "topic.b");
    CHECK(nb_rtps_participant_add_endpoint(
              b, NB_RTPS_ENDPOINT_SUBSCRIPTION, &subscription) == NB_OK);
    CHECK(wait_endpoint_count(&state_b, 0, 1, 5000));
    CHECK(wait_endpoint_count(&state_a, 0, 1, 5000));

    /* DCPSParticipant: the local participant and the discovered peer. */
    n = nb_rtps_participant_discovery_read(a, NB_DDS_BUILTIN_TOPIC_PARTICIPANT, views, 8);
    CHECK(n == 2);
    local_count = 0;
    for (i = 0; i < n; i++) {
        CHECK(views[i].topic == NB_DDS_BUILTIN_TOPIC_PARTICIPANT);
        if (views[i].is_local) {
            local_count++;
            CHECK(strcmp(views[i].name, "view-a") == 0);
        } else {
            CHECK(strcmp(views[i].name, "view-b") == 0);
            CHECK(views[i].lease_ms > 0);
        }
    }
    CHECK(local_count == 1);

    /* DCPSPublication on A: its own writer only. */
    n = nb_rtps_participant_discovery_read(a, NB_DDS_BUILTIN_TOPIC_PUBLICATION, views, 8);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(views[0].is_local);
        CHECK(views[0].is_writer);
        CHECK(strcmp(views[0].name, "topic.a") == 0);
        CHECK(views[0].reliability == NB_RTPS_RELIABILITY_RELIABLE);
        CHECK(views[0].history_depth == 8);
    }

    /* DCPSSubscription on A: the remote reader B announced. */
    n = nb_rtps_participant_discovery_read(a, NB_DDS_BUILTIN_TOPIC_SUBSCRIPTION, views, 8);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(!views[0].is_local);
        CHECK(!views[0].is_writer);
        CHECK(strcmp(views[0].name, "topic.b") == 0);
    }

    /* DCPSTopic: both (topic, type) pairs with their endpoint counts. */
    n = nb_rtps_participant_discovery_read(a, NB_DDS_BUILTIN_TOPIC_TOPIC, views, 8);
    CHECK(n == 2);
    for (i = 0; i < n; i++) {
        CHECK(views[i].topic == NB_DDS_BUILTIN_TOPIC_TOPIC);
        CHECK(views[i].type_name[0] != '\0');
        if (strcmp(views[i].name, "topic.a") == 0) {
            saw_topic_a = 1;
            CHECK(views[i].is_local == 1);
            CHECK(views[i].writer_count == 1);
            CHECK(views[i].reader_count == 0);
        } else if (strcmp(views[i].name, "topic.b") == 0) {
            saw_topic_b = 1;
            CHECK(views[i].is_local == 0);
            CHECK(views[i].writer_count == 0);
            CHECK(views[i].reader_count == 1);
        }
    }
    CHECK(saw_topic_a && saw_topic_b);
    if (n == 2) {
        CHECK(memcmp(views[0].handle, views[1].handle, NB_DDS_DISCOVERY_HANDLE_SIZE) != 0);
    }

    /* Bounds and invalid topic ids. */
    CHECK(nb_rtps_participant_discovery_read(a, NB_DDS_BUILTIN_TOPIC_PARTICIPANT, views, 1) == 1);
    CHECK(nb_rtps_participant_discovery_read(a, (nb_dds_builtin_topic_t)99, views, 8) == 0);

    nb_rtps_participant_stop(a);
    nb_rtps_participant_stop(b);
    nb_rtps_participant_destroy(a);
    nb_rtps_participant_destroy(b);
    ntk_mutex_destroy(state_a.lock);
    ntk_mutex_destroy(state_b.lock);
}

static void test_data_roundtrip(void)
{
    static const uint8_t expected[] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x11, 0x22};
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *publisher;
    nb_rtps_participant_t *subscriber;
    nb_rtps_endpoint_info_t publication;
    nb_rtps_endpoint_info_t subscription;
    nb_rtps_participant_info_t publisher_info;
    nb_rtps_dds_message_t message;
    test_state_t publisher_state;
    test_state_t subscriber_state;
    test_data_state_t data_state;
    uint16_t publisher_port;
    uint16_t subscriber_port;
    int waited;
    int received = 0;

    memset(&publisher_state, 0, sizeof(publisher_state));
    memset(&subscriber_state, 0, sizeof(subscriber_state));
    memset(&data_state, 0, sizeof(data_state));

    fill_config(&config, "data-publisher");
    publisher = nb_rtps_participant_create(&config);
    CHECK(publisher != NULL);
    if (!publisher) {
        return;
    }
    publisher_port = nb_rtps_participant_spdp_port(publisher);
    fill_config(&config, "data-subscriber");
    subscriber = nb_rtps_participant_create(&config);
    CHECK(subscriber != NULL);
    if (!subscriber) {
        nb_rtps_participant_destroy(publisher);
        return;
    }
    subscriber_port = nb_rtps_participant_spdp_port(subscriber);
    CHECK(nb_rtps_participant_add_peer(publisher, "127.0.0.1", subscriber_port) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(subscriber, "127.0.0.1", publisher_port) == NB_OK);

    publisher_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    subscriber_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    data_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(publisher_state.lock != NULL);
    CHECK(subscriber_state.lock != NULL);
    CHECK(data_state.lock != NULL);
    if (!publisher_state.lock || !subscriber_state.lock || !data_state.lock) {
        nb_rtps_participant_destroy(publisher);
        nb_rtps_participant_destroy(subscriber);
        if (publisher_state.lock) {
            ntk_mutex_destroy(publisher_state.lock);
        }
        if (subscriber_state.lock) {
            ntk_mutex_destroy(subscriber_state.lock);
        }
        if (data_state.lock) {
            ntk_mutex_destroy(data_state.lock);
        }
        return;
    }
    nb_rtps_participant_set_callback(publisher, on_participant, &publisher_state);
    nb_rtps_participant_set_callback(subscriber, on_participant, &subscriber_state);
    nb_rtps_participant_set_endpoint_callback(publisher, on_endpoint, &publisher_state);
    nb_rtps_participant_set_endpoint_callback(subscriber, on_endpoint, &subscriber_state);
    nb_rtps_participant_set_data_callback(subscriber, on_data, &data_state);
    CHECK(nb_rtps_participant_start(publisher) == NB_OK);
    CHECK(nb_rtps_participant_start(subscriber) == NB_OK);
    CHECK(wait_count(&publisher_state, 0, 1, 5000));
    CHECK(wait_count(&subscriber_state, 0, 1, 5000));

    memset(&publication, 0, sizeof(publication));
    (void)snprintf(publication.topic_name,
                   sizeof(publication.topic_name),
                   "%s",
                   NB_RTPS_DDS_MESSAGE_TOPIC);
    (void)snprintf(publication.type_name,
                   sizeof(publication.type_name),
                   "%s",
                   NB_RTPS_DDS_MESSAGE_TYPE);
    publication.reliability = NB_RTPS_RELIABILITY_BEST_EFFORT;
    publication.durability = NB_RTPS_DURABILITY_VOLATILE;
    publication.history_kind = NB_RTPS_HISTORY_KEEP_LAST;
    publication.history_depth = 1;
    CHECK(nb_rtps_participant_add_endpoint(
              publisher, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);

    memset(&subscription, 0, sizeof(subscription));
    (void)snprintf(subscription.topic_name,
                   sizeof(subscription.topic_name),
                   "%s",
                   NB_RTPS_DDS_MESSAGE_TOPIC);
    (void)snprintf(subscription.type_name,
                   sizeof(subscription.type_name),
                   "%s",
                   NB_RTPS_DDS_MESSAGE_TYPE);
    subscription.reliability = NB_RTPS_RELIABILITY_BEST_EFFORT;
    subscription.durability = NB_RTPS_DURABILITY_VOLATILE;
    subscription.history_kind = NB_RTPS_HISTORY_KEEP_LAST;
    subscription.history_depth = 1;
    CHECK(nb_rtps_participant_add_endpoint(
              subscriber, NB_RTPS_ENDPOINT_SUBSCRIPTION, &subscription) == NB_OK);
    CHECK(subscription.unicast_count > 0);
    CHECK(wait_endpoint_count(&subscriber_state, 0, 1, 5000));
    CHECK(wait_endpoint_count(&publisher_state, 0, 1, 5000));

    CHECK(nb_rtps_participant_local_info(publisher, &publisher_info) == NB_OK);
    message.payload = expected;
    message.payload_len = sizeof(expected);
    for (waited = 0; waited < 5000 && !received; waited += 50) {
        if (nb_rtps_participant_publish_dds_message(
                publisher, &publication.endpoint_guid, &message) == NB_OK) {
            received = data_received(&data_state);
        }
        if (!received) {
            ntk_sleep_ms(50);
        }
    }
    CHECK(received);
    CHECK(data_state.payload_len == sizeof(expected));
    CHECK(memcmp(data_state.payload, expected, sizeof(expected)) == 0);
    CHECK(nb_rtps_guid_equal(&data_state.reader_guid, &subscription.endpoint_guid));
    CHECK(nb_rtps_guid_prefix_equal(&data_state.writer_guid.prefix,
                                    &publisher_info.guid.prefix));
    CHECK(nb_rtps_entity_id_equal(&data_state.writer_guid.entity,
                                  &publication.endpoint_guid.entity));

    nb_rtps_participant_stop(publisher);
    nb_rtps_participant_stop(subscriber);
    nb_rtps_participant_destroy(publisher);
    nb_rtps_participant_destroy(subscriber);
    ntk_mutex_destroy(publisher_state.lock);
    ntk_mutex_destroy(subscriber_state.lock);
    ntk_mutex_destroy(data_state.lock);
}

/*
 * Fragmentation path: samples above NB_RTPS_DATA_PAYLOAD_MAX are split into
 * DATA_FRAG submessages and must be reassembled byte for byte. RELIABLE QoS is
 * used because a partly received sample stays missing and has to be replayed.
 */
typedef struct frag_state {
    ntk_mutex *lock;
    int received;
    size_t len;
    uint32_t hash;
} frag_state_t;

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

static void on_frag_data(void *user,
                         const nb_rtps_guid_t *reader_guid,
                         const nb_rtps_guid_t *writer_guid,
                         const nb_rtps_dds_message_t *message)
{
    frag_state_t *state = (frag_state_t *)user;
    uint32_t hash;

    (void)reader_guid;
    (void)writer_guid;
    if (!state || !message || !message->payload) {
        return;
    }
    hash = payload_hash(message->payload, message->payload_len);
    ntk_mutex_lock(state->lock);
    if (!state->received) {
        state->received = 1;
        state->len = message->payload_len;
        state->hash = hash;
    }
    ntk_mutex_unlock(state->lock);
}

static int frag_data_received(frag_state_t *state)
{
    int received;

    ntk_mutex_lock(state->lock);
    received = state->received;
    ntk_mutex_unlock(state->lock);
    return received;
}

/* Publish `sample` until the subscriber reports exactly `expected_len` bytes
 * whose hash matches (or the attempt times out). */
static int send_raw_sample(nb_rtps_participant_t *publisher,
                           const nb_rtps_guid_t *writer_guid,
                           const uint8_t *sample,
                           size_t sample_len,
                           const uint8_t *expected,
                           size_t expected_len,
                           frag_state_t *state)
{
    uint32_t expected_hash = payload_hash(expected, expected_len);
    int waited;
    int ok = 0;

    for (waited = 0; waited < 5000; waited += 50) {
        ntk_mutex_lock(state->lock);
        state->received = 0;
        ntk_mutex_unlock(state->lock);
        if (nb_rtps_participant_publish(publisher, writer_guid, sample, sample_len) == NB_OK) {
            ntk_sleep_ms(50);
            ntk_mutex_lock(state->lock);
            ok = state->received && state->len == expected_len &&
                 state->hash == expected_hash;
            ntk_mutex_unlock(state->lock);
            if (ok) {
                break;
            }
        } else {
            ntk_sleep_ms(50);
        }
    }
    return ok;
}

static int send_frag_sample(nb_rtps_participant_t *publisher,
                            const nb_rtps_endpoint_info_t *publication,
                            const uint8_t *payload,
                            size_t payload_len,
                            frag_state_t *state)
{
    nb_rtps_dds_message_t message;
    uint8_t *serialized;
    size_t serialized_len = 0;
    int ok;
    int rc;

    serialized = (uint8_t *)malloc(payload_len + 8u);
    if (!serialized) {
        return 0;
    }
    message.payload = payload;
    message.payload_len = payload_len;
    rc = nb_rtps_dds_message_serialize(
        &message, serialized, payload_len + 8u, &serialized_len);
    if (rc != NB_OK) {
        free(serialized);
        return 0;
    }

    ok = send_raw_sample(publisher,
                         &publication->endpoint_guid,
                         serialized,
                         serialized_len,
                         payload,
                         payload_len,
                         state);
    free(serialized);
    return ok;
}

static void test_fragmented_data(void)
{
    static const size_t sizes[] = {4096u, 60000u};
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *publisher;
    nb_rtps_participant_t *subscriber;
    nb_rtps_endpoint_info_t publication;
    nb_rtps_endpoint_info_t subscription;
    test_state_t publisher_state;
    test_state_t subscriber_state;
    frag_state_t frag_state;
    uint16_t publisher_port;
    uint16_t subscriber_port;
    size_t s;

    memset(&publisher_state, 0, sizeof(publisher_state));
    memset(&subscriber_state, 0, sizeof(subscriber_state));
    memset(&frag_state, 0, sizeof(frag_state));

    fill_config(&config, "frag-publisher");
    publisher = nb_rtps_participant_create(&config);
    CHECK(publisher != NULL);
    if (!publisher) {
        return;
    }
    publisher_port = nb_rtps_participant_spdp_port(publisher);
    fill_config(&config, "frag-subscriber");
    subscriber = nb_rtps_participant_create(&config);
    CHECK(subscriber != NULL);
    if (!subscriber) {
        nb_rtps_participant_destroy(publisher);
        return;
    }
    subscriber_port = nb_rtps_participant_spdp_port(subscriber);
    CHECK(nb_rtps_participant_add_peer(publisher, "127.0.0.1", subscriber_port) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(subscriber, "127.0.0.1", publisher_port) == NB_OK);

    publisher_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    subscriber_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    frag_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(publisher_state.lock && subscriber_state.lock && frag_state.lock);
    if (!publisher_state.lock || !subscriber_state.lock || !frag_state.lock) {
        goto cleanup;
    }
    nb_rtps_participant_set_callback(publisher, on_participant, &publisher_state);
    nb_rtps_participant_set_callback(subscriber, on_participant, &subscriber_state);
    nb_rtps_participant_set_endpoint_callback(publisher, on_endpoint, &publisher_state);
    nb_rtps_participant_set_endpoint_callback(subscriber, on_endpoint, &subscriber_state);
    nb_rtps_participant_set_data_callback(subscriber, on_frag_data, &frag_state);
    CHECK(nb_rtps_participant_start(publisher) == NB_OK);
    CHECK(nb_rtps_participant_start(subscriber) == NB_OK);
    CHECK(wait_count(&publisher_state, 0, 1, 5000));
    CHECK(wait_count(&subscriber_state, 0, 1, 5000));

    memset(&publication, 0, sizeof(publication));
    (void)snprintf(publication.topic_name,
                   sizeof(publication.topic_name),
                   "%s",
                   NB_RTPS_DDS_MESSAGE_TOPIC);
    (void)snprintf(publication.type_name,
                   sizeof(publication.type_name),
                   "%s",
                   NB_RTPS_DDS_MESSAGE_TYPE);
    publication.reliability = NB_RTPS_RELIABILITY_RELIABLE;
    publication.history_kind = NB_RTPS_HISTORY_KEEP_LAST;
    publication.history_depth = 4;
    CHECK(nb_rtps_participant_add_endpoint(
              publisher, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);

    memset(&subscription, 0, sizeof(subscription));
    (void)snprintf(subscription.topic_name,
                   sizeof(subscription.topic_name),
                   "%s",
                   NB_RTPS_DDS_MESSAGE_TOPIC);
    (void)snprintf(subscription.type_name,
                   sizeof(subscription.type_name),
                   "%s",
                   NB_RTPS_DDS_MESSAGE_TYPE);
    subscription.reliability = NB_RTPS_RELIABILITY_RELIABLE;
    subscription.history_kind = NB_RTPS_HISTORY_KEEP_LAST;
    subscription.history_depth = 4;
    CHECK(nb_rtps_participant_add_endpoint(
              subscriber, NB_RTPS_ENDPOINT_SUBSCRIPTION, &subscription) == NB_OK);
    CHECK(wait_endpoint_count(&subscriber_state, 0, 1, 5000));
    CHECK(wait_endpoint_count(&publisher_state, 0, 1, 5000));

    for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        size_t len = sizes[s];
        uint8_t *payload = (uint8_t *)malloc(len);
        size_t i;

        CHECK(payload != NULL);
        if (!payload) {
            continue;
        }
        for (i = 0; i < len; i++) {
            payload[i] = (uint8_t)((i * 31u + 7u) & 0xffu);
        }
        CHECK(send_frag_sample(publisher, &publication, payload, len, &frag_state));
        free(payload);
    }

cleanup:
    if (publisher) {
        nb_rtps_participant_stop(publisher);
    }
    if (subscriber) {
        nb_rtps_participant_stop(subscriber);
    }
    if (publisher) {
        nb_rtps_participant_destroy(publisher);
    }
    if (subscriber) {
        nb_rtps_participant_destroy(subscriber);
    }
    if (publisher_state.lock) {
        ntk_mutex_destroy(publisher_state.lock);
    }
    if (subscriber_state.lock) {
        ntk_mutex_destroy(subscriber_state.lock);
    }
    if (frag_state.lock) {
        ntk_mutex_destroy(frag_state.lock);
    }
}

/* Open a UDP socket on the loopback interface and report the port it got. */
static int open_raw_socket(ntk_socket **out, uint16_t *port)
{
    ntk_socket *sock = ntk_socket_create();
    char host[64];

    *out = NULL;
    *port = 0;
    if (!sock) {
        return 0;
    }
    if (ntk_socket_bind_udp(sock, "127.0.0.1", 0, 0) != NTK_OK ||
        ntk_socket_get_local(sock, host, sizeof(host), port) != NTK_OK || *port == 0) {
        ntk_socket_destroy(sock);
        return 0;
    }
    *out = sock;
    return 1;
}

/*
 * Reader-side fragment repair: a sample that arrives with a hole must be
 * repaired by asking for the missing fragment instead of waiting for a
 * whole-sample retransmit. The raw socket stands in for the remote writer.
 */
static void test_frag_nack_repair(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *subscriber;
    nb_rtps_participant_info_t info;
    ntk_socket *raw = NULL;
    uint16_t raw_port = 0;
    uint32_t unicast_port;
    uint8_t payload[2500];
    uint8_t datagram[2048];
    uint8_t received[2048];
    char from_host[64];
    uint16_t from_port = 0;
    nb_rtps_header_t header;
    nb_rtps_data_frag_t fragment;
    nb_cdr_writer_t writer;
    int got_nack = 0;
    int waited;
    size_t i;

    fill_config(&config, "frag-repair-reader");
    subscriber = nb_rtps_participant_create(&config);
    CHECK(subscriber != NULL);
    if (!subscriber) {
        return;
    }
    CHECK(nb_rtps_participant_start(subscriber) == NB_OK);
    CHECK(nb_rtps_participant_local_info(subscriber, &info) == NB_OK);
    unicast_port = info.metatraffic_unicast.port;
    CHECK(unicast_port != 0);
    CHECK(open_raw_socket(&raw, &raw_port));
    if (!raw || unicast_port == 0) {
        nb_rtps_participant_stop(subscriber);
        nb_rtps_participant_destroy(subscriber);
        return;
    }

    for (i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)((i * 13u + 5u) & 0xffu);
    }
    memset(&header, 0, sizeof(header));
    header.version_major = NB_RTPS_PROTOCOL_VERSION_MAJOR;
    header.version_minor = NB_RTPS_PROTOCOL_VERSION_MINOR;
    header.vendor_id[0] = NB_RTPS_VENDOR_ID_0;
    header.vendor_id[1] = NB_RTPS_VENDOR_ID_1;
    for (i = 0; i < NB_RTPS_GUID_PREFIX_SIZE; i++) {
        header.guid_prefix.bytes[i] = (uint8_t)(0xa0u + i);
    }
    memset(&fragment, 0, sizeof(fragment));
    fragment.reader_id = nb_rtps_eid_unknown;
    fragment.writer_id.bytes[0] = 0x04;
    fragment.writer_id.bytes[1] = 0x02;
    fragment.writer_id.bytes[2] = 0x03;
    fragment.writer_id.bytes[3] = 0x07;
    fragment.writer_sn.high = 0;
    fragment.writer_sn.low = 1;
    fragment.fragments_in_submessage = 1;
    fragment.fragment_size = 1024;
    fragment.sample_size = (uint32_t)sizeof(payload);

    /* Deliver fragments 1 and 3 of three; fragment 2 is "lost". */
    fragment.fragment_starting_num = 1;
    fragment.payload = payload;
    fragment.payload_len = 1024;
    CHECK(nb_rtps_message_init(&writer, datagram, sizeof(datagram), &header) == NB_OK);
    CHECK(nb_rtps_data_frag(&writer, &fragment) == NB_OK);
    CHECK(ntk_socket_sendto(
              raw, datagram, nb_cdr_writer_len(&writer), "127.0.0.1", (uint16_t)unicast_port) > 0);

    fragment.fragment_starting_num = 3;
    fragment.payload = payload + 2048;
    fragment.payload_len = sizeof(payload) - 2048u;
    CHECK(nb_rtps_message_init(&writer, datagram, sizeof(datagram), &header) == NB_OK);
    CHECK(nb_rtps_data_frag(&writer, &fragment) == NB_OK);
    CHECK(ntk_socket_sendto(
              raw, datagram, nb_cdr_writer_len(&writer), "127.0.0.1", (uint16_t)unicast_port) > 0);

    for (waited = 0; waited < 2000 && !got_nack; waited += 20) {
        ntk_ssize_t n = ntk_socket_recvfrom(
            raw, received, sizeof(received), from_host, sizeof(from_host), &from_port);

        if (n > 0) {
            nb_rtps_header_t parsed_header;
            nb_rtps_submessage_view_t view;
            size_t offset = 0;

            if (nb_rtps_message_begin(received, (size_t)n, &parsed_header, &offset) == NB_OK) {
                while (nb_rtps_message_next(received, (size_t)n, &offset, &view) == 1) {
                    if (view.id != NB_RTPS_SUBMSG_NACK_FRAG) {
                        continue;
                    }
                    {
                        nb_rtps_nack_frag_t nack;

                        if (nb_rtps_nack_frag_decode(view.body, view.body_len, &nack) == NB_OK) {
                            CHECK(nack.writer_sn.low == 1);
                            CHECK(nack.bitmap_base == 2);
                            CHECK(nack.num_bits == 2);
                            /* Bit 0 is the missing fragment 2; fragment 3 is
                             * already present and must not be requested. */
                            CHECK(nack.bitmap[0] == 0x80000000u);
                            got_nack = 1;
                        }
                    }
                }
            }
        }
        ntk_sleep_ms(20);
    }
    CHECK(got_nack);

    ntk_socket_destroy(raw);
    nb_rtps_participant_stop(subscriber);
    nb_rtps_participant_destroy(subscriber);
}

/*
 * Writer-side fragment repair: after a NACK_FRAG only the requested fragments
 * are resent, followed by a HEARTBEAT_FRAG that restates the fragment count.
 */
static void test_frag_nack_repair_writer(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *publisher;
    nb_rtps_participant_info_t info;
    nb_rtps_endpoint_info_t publication;
    ntk_socket *raw = NULL;
    uint16_t raw_port = 0;
    uint32_t unicast_port;
    uint8_t payload[2500];
    uint8_t datagram[2048];
    uint8_t received[2048];
    char from_host[64];
    uint16_t from_port = 0;
    nb_rtps_header_t header;
    nb_rtps_nack_frag_t nack;
    nb_cdr_writer_t writer;
    int got_frag2 = 0;
    int got_frag3 = 0;
    int got_heartbeat = 0;
    int waited;
    size_t i;

    fill_config(&config, "frag-repair-writer");
    publisher = nb_rtps_participant_create(&config);
    CHECK(publisher != NULL);
    if (!publisher) {
        return;
    }
    CHECK(nb_rtps_participant_start(publisher) == NB_OK);
    CHECK(nb_rtps_participant_local_info(publisher, &info) == NB_OK);
    unicast_port = info.metatraffic_unicast.port;

    init_data_endpoint(&publication, 1);
    CHECK(nb_rtps_participant_add_endpoint(
              publisher, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);
    for (i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)((i * 17u + 9u) & 0xffu);
    }
    /* No reader has been discovered, so this only fills the writer history;
     * the repair below is what actually triggers a transmission. */
    (void)nb_rtps_participant_publish(
        publisher, &publication.endpoint_guid, payload, sizeof(payload));

    CHECK(open_raw_socket(&raw, &raw_port));
    if (!raw || unicast_port == 0) {
        ntk_socket_destroy(raw);
        nb_rtps_participant_stop(publisher);
        nb_rtps_participant_destroy(publisher);
        return;
    }
    memset(&header, 0, sizeof(header));
    header.version_major = NB_RTPS_PROTOCOL_VERSION_MAJOR;
    header.version_minor = NB_RTPS_PROTOCOL_VERSION_MINOR;
    header.vendor_id[0] = NB_RTPS_VENDOR_ID_0;
    header.vendor_id[1] = NB_RTPS_VENDOR_ID_1;
    for (i = 0; i < NB_RTPS_GUID_PREFIX_SIZE; i++) {
        header.guid_prefix.bytes[i] = (uint8_t)(0x50u + i);
    }
    memset(&nack, 0, sizeof(nack));
    nack.reader_id = nb_rtps_eid_unknown;
    nack.writer_id = publication.endpoint_guid.entity;
    nack.writer_sn.high = 0;
    nack.writer_sn.low = 1;
    nack.bitmap_base = 2;
    nack.num_bits = 2;
    nack.bitmap[0] = 0xc0000000u; /* fragments 2 and 3 */

    CHECK(nb_rtps_message_init(&writer, datagram, sizeof(datagram), &header) == NB_OK);
    CHECK(nb_rtps_nack_frag(&writer, &nack) == NB_OK);
    CHECK(ntk_socket_sendto(
              raw, datagram, nb_cdr_writer_len(&writer), "127.0.0.1", (uint16_t)unicast_port) > 0);

    for (waited = 0; waited < 2000 && !(got_frag2 && got_frag3 && got_heartbeat); waited += 20) {
        ntk_ssize_t n = ntk_socket_recvfrom(
            raw, received, sizeof(received), from_host, sizeof(from_host), &from_port);

        if (n > 0) {
            nb_rtps_header_t parsed_header;
            nb_rtps_submessage_view_t view;
            size_t offset = 0;

            if (nb_rtps_message_begin(received, (size_t)n, &parsed_header, &offset) != NB_OK) {
                continue;
            }
            while (nb_rtps_message_next(received, (size_t)n, &offset, &view) == 1) {
                if (view.id == NB_RTPS_SUBMSG_DATA_FRAG) {
                    nb_rtps_data_frag_t parsed;

                    if (nb_rtps_data_frag_decode(
                            view.flags, view.body, view.body_len, &parsed) != NB_OK) {
                        continue;
                    }
                    CHECK(parsed.writer_sn.low == 1);
                    if (parsed.fragment_starting_num == 2) {
                        CHECK(parsed.payload_len == NB_RTPS_FRAGMENT_SIZE);
                        CHECK(memcmp(parsed.payload,
                                     payload + NB_RTPS_FRAGMENT_SIZE,
                                     NB_RTPS_FRAGMENT_SIZE) == 0);
                        got_frag2 = 1;
                    } else if (parsed.fragment_starting_num == 3) {
                        CHECK(parsed.payload_len ==
                              sizeof(payload) - 2u * NB_RTPS_FRAGMENT_SIZE);
                        CHECK(memcmp(parsed.payload,
                                     payload + 2u * NB_RTPS_FRAGMENT_SIZE,
                                     parsed.payload_len) == 0);
                        got_frag3 = 1;
                    }
                } else if (view.id == NB_RTPS_SUBMSG_HEARTBEAT_FRAG) {
                    nb_rtps_heartbeat_frag_t parsed;

                    if (nb_rtps_heartbeat_frag_decode(
                            view.body, view.body_len, &parsed) == NB_OK) {
                        CHECK(parsed.writer_sn.low == 1);
                        CHECK(parsed.last_fragment_num == 3);
                        got_heartbeat = 1;
                    }
                }
            }
        }
        ntk_sleep_ms(20);
    }
    CHECK(got_frag2);
    CHECK(got_frag3);
    CHECK(got_heartbeat);

    ntk_socket_destroy(raw);
    nb_rtps_participant_stop(publisher);
    nb_rtps_participant_destroy(publisher);
}

/*
 * LIFESPAN: a sample that outlived its writer's lifespan must not be
 * retransmitted. Asking for it returns a GAP, so the reader stops waiting
 * instead of looping on an ACKNACK the writer can never satisfy.
 */
static void test_lifespan_expiry(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *publisher;
    nb_rtps_participant_info_t info;
    nb_rtps_endpoint_info_t publication;
    ntk_socket *raw = NULL;
    uint16_t raw_port = 0;
    uint32_t unicast_port;
    uint8_t payload[64];
    uint8_t datagram[256];
    uint8_t received[2048];
    char from_host[64];
    uint16_t from_port = 0;
    nb_rtps_header_t header;
    nb_rtps_acknack_t acknack;
    nb_cdr_writer_t writer;
    int got_gap = 0;
    int got_data = 0;
    int waited;
    size_t i;

    fill_config(&config, "lifespan-writer");
    publisher = nb_rtps_participant_create(&config);
    CHECK(publisher != NULL);
    if (!publisher) {
        return;
    }
    CHECK(nb_rtps_participant_start(publisher) == NB_OK);
    CHECK(nb_rtps_participant_local_info(publisher, &info) == NB_OK);
    unicast_port = info.metatraffic_unicast.port;

    init_data_endpoint(&publication, 1);
    publication.lifespan_ms = 100;
    CHECK(nb_rtps_participant_add_endpoint(
              publisher, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);
    for (i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 7u + 1u);
    }
    (void)nb_rtps_participant_publish(
        publisher, &publication.endpoint_guid, payload, sizeof(payload));

    CHECK(open_raw_socket(&raw, &raw_port));
    if (!raw || unicast_port == 0) {
        ntk_socket_destroy(raw);
        nb_rtps_participant_stop(publisher);
        nb_rtps_participant_destroy(publisher);
        return;
    }
    /* Let the sample age past its 100 ms lifespan. */
    ntk_sleep_ms(250);

    memset(&header, 0, sizeof(header));
    header.version_major = NB_RTPS_PROTOCOL_VERSION_MAJOR;
    header.version_minor = NB_RTPS_PROTOCOL_VERSION_MINOR;
    header.vendor_id[0] = NB_RTPS_VENDOR_ID_0;
    header.vendor_id[1] = NB_RTPS_VENDOR_ID_1;
    for (i = 0; i < NB_RTPS_GUID_PREFIX_SIZE; i++) {
        header.guid_prefix.bytes[i] = (uint8_t)(0x30u + i);
    }
    memset(&acknack, 0, sizeof(acknack));
    acknack.reader_id = nb_rtps_eid_unknown;
    acknack.writer_id = publication.endpoint_guid.entity;
    acknack.base.high = 0;
    acknack.base.low = 1;
    acknack.num_bits = 1;
    acknack.bitmap[0] = 0x80000000u;
    acknack.count = 1;

    CHECK(nb_rtps_message_init(&writer, datagram, sizeof(datagram), &header) == NB_OK);
    CHECK(nb_rtps_acknack(&writer, &acknack) == NB_OK);
    CHECK(ntk_socket_sendto(
              raw, datagram, nb_cdr_writer_len(&writer), "127.0.0.1", (uint16_t)unicast_port) > 0);

    for (waited = 0; waited < 1000 && !got_gap; waited += 20) {
        ntk_ssize_t n = ntk_socket_recvfrom(
            raw, received, sizeof(received), from_host, sizeof(from_host), &from_port);

        if (n > 0) {
            nb_rtps_header_t parsed_header;
            nb_rtps_submessage_view_t view;
            size_t offset = 0;

            if (nb_rtps_message_begin(received, (size_t)n, &parsed_header, &offset) != NB_OK) {
                continue;
            }
            while (nb_rtps_message_next(received, (size_t)n, &offset, &view) == 1) {
                if (view.id == NB_RTPS_SUBMSG_GAP) {
                    nb_rtps_gap_t gap;

                    if (nb_rtps_gap_decode(view.body, view.body_len, &gap) == NB_OK &&
                        nb_rtps_entity_id_equal(&gap.writer_id,
                                                &publication.endpoint_guid.entity)) {
                        /* A single-sequence-number gap leaves the set empty. */
                        CHECK(gap.gap_start.low == 1);
                        CHECK(gap.num_bits == 0 || gap.base.low >= 2);
                        got_gap = 1;
                    }
                } else if (view.id == NB_RTPS_SUBMSG_DATA ||
                           view.id == NB_RTPS_SUBMSG_DATA_FRAG) {
                    got_data = 1;
                }
            }
        }
        ntk_sleep_ms(20);
    }
    CHECK(got_gap);
    CHECK(!got_data);

    ntk_socket_destroy(raw);
    nb_rtps_participant_stop(publisher);
    nb_rtps_participant_destroy(publisher);
}

typedef struct status_state {
    ntk_mutex *lock;
    int offered;
    int requested;
    int liveliness;
    int last_alive;
    uint32_t alive_count;
    uint32_t not_alive_count;
    nb_rtps_guid_t last_writer;
    nb_rtps_guid_t last_endpoint;
    uint32_t last_count;
    /* Match events, per kind, with the latest match status. */
    int pub_matched;
    int sub_matched;
    int incompatible;
    uint32_t match_count;
    uint32_t match_total;
    nb_rtps_guid_t match_peer;
    int has_match_peer;
    char reason[32];
} status_state_t;

static void on_status(void *user, const nb_rtps_status_event_t *event)
{
    status_state_t *state = (status_state_t *)user;

    if (!state || !event) {
        return;
    }
    ntk_mutex_lock(state->lock);
    if (event->kind == NB_RTPS_STATUS_OFFERED_DEADLINE_MISSED) {
        state->offered++;
    } else if (event->kind == NB_RTPS_STATUS_REQUESTED_DEADLINE_MISSED) {
        state->requested++;
    } else if (event->kind == NB_RTPS_STATUS_LIVELINESS_CHANGED) {
        state->liveliness++;
        state->last_alive = event->alive;
        state->alive_count = event->alive_count;
        state->not_alive_count = event->not_alive_count;
        state->last_writer = event->writer;
    } else if (event->kind == NB_RTPS_STATUS_PUBLICATION_MATCHED ||
               event->kind == NB_RTPS_STATUS_SUBSCRIPTION_MATCHED) {
        if (event->kind == NB_RTPS_STATUS_PUBLICATION_MATCHED) {
            state->pub_matched++;
        } else {
            state->sub_matched++;
        }
        state->match_count = event->count;
        state->match_total = event->total_count;
        state->match_peer = event->writer;
        state->has_match_peer = event->has_peer;
    } else if (event->kind == NB_RTPS_STATUS_QOS_INCOMPATIBLE) {
        state->incompatible++;
        state->match_peer = event->writer;
        state->has_match_peer = event->has_peer;
        (void)snprintf(state->reason, sizeof(state->reason), "%s", event->reason);
    }
    state->last_endpoint = event->endpoint;
    state->last_count = event->count;
    ntk_mutex_unlock(state->lock);
}

static int status_count(status_state_t *state, int offered)
{
    int count;

    ntk_mutex_lock(state->lock);
    count = offered ? state->offered : state->requested;
    ntk_mutex_unlock(state->lock);
    return count;
}

static int liveliness_count(status_state_t *state)
{
    int count;

    ntk_mutex_lock(state->lock);
    count = state->liveliness;
    ntk_mutex_unlock(state->lock);
    return count;
}

static void status_snapshot(status_state_t *state, status_state_t *out)
{
    ntk_mutex_lock(state->lock);
    *out = *state;
    ntk_mutex_unlock(state->lock);
}

/*
 * LIVELINESS_CHANGED: a writer that announces a finite automatic lease has to
 * keep asserting it with ParticipantMessageData. While the publisher runs, the
 * subscription sees no change; once the publisher stops, the writer goes
 * not-alive and the event reports it with the LivelinessChangedStatus counts.
 */
static void test_liveliness_lease(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *publisher;
    nb_rtps_participant_t *subscriber;
    nb_rtps_endpoint_info_t publication;
    nb_rtps_endpoint_info_t subscription;
    test_state_t publisher_state;
    test_state_t subscriber_state;
    status_state_t status;
    uint16_t publisher_port;
    uint16_t subscriber_port;
    int waited;

    memset(&publisher_state, 0, sizeof(publisher_state));
    memset(&subscriber_state, 0, sizeof(subscriber_state));
    memset(&status, 0, sizeof(status));

    fill_config(&config, "liveliness-publisher");
    publisher = nb_rtps_participant_create(&config);
    CHECK(publisher != NULL);
    if (!publisher) {
        return;
    }
    publisher_port = nb_rtps_participant_spdp_port(publisher);
    fill_config(&config, "liveliness-subscriber");
    subscriber = nb_rtps_participant_create(&config);
    CHECK(subscriber != NULL);
    if (!subscriber) {
        nb_rtps_participant_destroy(publisher);
        return;
    }
    subscriber_port = nb_rtps_participant_spdp_port(subscriber);
    CHECK(nb_rtps_participant_add_peer(publisher, "127.0.0.1", subscriber_port) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(subscriber, "127.0.0.1", publisher_port) == NB_OK);

    publisher_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    subscriber_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    status.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(publisher_state.lock && subscriber_state.lock && status.lock);
    if (!publisher_state.lock || !subscriber_state.lock || !status.lock) {
        goto cleanup;
    }
    nb_rtps_participant_set_callback(publisher, on_participant, &publisher_state);
    nb_rtps_participant_set_callback(subscriber, on_participant, &subscriber_state);
    nb_rtps_participant_set_endpoint_callback(publisher, on_endpoint, &publisher_state);
    nb_rtps_participant_set_endpoint_callback(subscriber, on_endpoint, &subscriber_state);
    CHECK(nb_rtps_participant_set_status_callback(subscriber, on_status, &status) == NB_OK);

    /* Both sides announce and request the same finite automatic lease. */
    init_data_endpoint(&publication, 0);
    publication.liveliness_kind = NB_RTPS_LIVELINESS_AUTOMATIC;
    publication.liveliness_lease_ms = 300;
    CHECK(nb_rtps_participant_add_endpoint(
              publisher, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);
    init_data_endpoint(&subscription, 0);
    subscription.liveliness_kind = NB_RTPS_LIVELINESS_AUTOMATIC;
    subscription.liveliness_lease_ms = 300;
    CHECK(nb_rtps_participant_add_endpoint(
              subscriber, NB_RTPS_ENDPOINT_SUBSCRIPTION, &subscription) == NB_OK);

    CHECK(nb_rtps_participant_start(publisher) == NB_OK);
    CHECK(nb_rtps_participant_start(subscriber) == NB_OK);
    CHECK(wait_endpoint_count(&publisher_state, 0, 1, 5000));
    CHECK(wait_endpoint_count(&subscriber_state, 0, 1, 5000));

    /* Three lease periods with the publisher running: the assertions keep the
     * writer alive, so no status event may arrive. */
    ntk_sleep_ms(900);
    CHECK(liveliness_count(&status) == 0);

    /* The publisher stops asserting. Its participant lease is longer than the
     * liveliness lease, so the writer is declared not-alive while the
     * participant is still known. */
    nb_rtps_participant_stop(publisher);
    for (waited = 0; waited < 2000 && liveliness_count(&status) == 0; waited += 20) {
        ntk_sleep_ms(20);
    }
    CHECK(liveliness_count(&status) > 0);
    ntk_mutex_lock(status.lock);
    CHECK(status.last_alive == 0);
    CHECK(status.not_alive_count == 1);
    CHECK(status.alive_count == 0);
    CHECK(nb_rtps_guid_equal(&status.last_endpoint, &subscription.endpoint_guid));
    CHECK(nb_rtps_guid_equal(&status.last_writer, &publication.endpoint_guid));
    ntk_mutex_unlock(status.lock);

cleanup:
    nb_rtps_participant_stop(publisher);
    nb_rtps_participant_stop(subscriber);
    nb_rtps_participant_destroy(publisher);
    nb_rtps_participant_destroy(subscriber);
    if (publisher_state.lock) {
        ntk_mutex_destroy(publisher_state.lock);
    }
    if (subscriber_state.lock) {
        ntk_mutex_destroy(subscriber_state.lock);
    }
    if (status.lock) {
        ntk_mutex_destroy(status.lock);
    }
}

/*
 * MANUAL_BY_TOPIC liveliness: writing asserts the writer (DDS 2.2.2.4.2.10),
 * carried as a HEARTBEAT with the L flag. The writer stays alive while it
 * writes, turns not-alive once it stops for longer than the lease even though
 * the participant keeps running, and a further write asserts it again.
 */
static void test_manual_liveliness(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *publisher;
    nb_rtps_participant_t *subscriber;
    nb_rtps_endpoint_info_t publication;
    nb_rtps_endpoint_info_t subscription;
    test_state_t publisher_state;
    test_state_t subscriber_state;
    status_state_t status;
    const uint8_t payload[4] = {0, 0, 0, 1};
    uint16_t publisher_port;
    uint16_t subscriber_port;
    int waited;
    int i;
    int before;

    memset(&publisher_state, 0, sizeof(publisher_state));
    memset(&subscriber_state, 0, sizeof(subscriber_state));
    memset(&status, 0, sizeof(status));

    fill_config(&config, "manual-liveliness-publisher");
    publisher = nb_rtps_participant_create(&config);
    CHECK(publisher != NULL);
    if (!publisher) {
        return;
    }
    publisher_port = nb_rtps_participant_spdp_port(publisher);
    fill_config(&config, "manual-liveliness-subscriber");
    subscriber = nb_rtps_participant_create(&config);
    CHECK(subscriber != NULL);
    if (!subscriber) {
        nb_rtps_participant_destroy(publisher);
        return;
    }
    subscriber_port = nb_rtps_participant_spdp_port(subscriber);
    CHECK(nb_rtps_participant_add_peer(publisher, "127.0.0.1", subscriber_port) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(subscriber, "127.0.0.1", publisher_port) == NB_OK);

    publisher_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    subscriber_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    status.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(publisher_state.lock && subscriber_state.lock && status.lock);
    if (!publisher_state.lock || !subscriber_state.lock || !status.lock) {
        goto cleanup;
    }
    nb_rtps_participant_set_callback(publisher, on_participant, &publisher_state);
    nb_rtps_participant_set_callback(subscriber, on_participant, &subscriber_state);
    nb_rtps_participant_set_endpoint_callback(publisher, on_endpoint, &publisher_state);
    nb_rtps_participant_set_endpoint_callback(subscriber, on_endpoint, &subscriber_state);
    CHECK(nb_rtps_participant_set_status_callback(subscriber, on_status, &status) == NB_OK);

    /* Best-effort on purpose: no heartbeats are sent for these pairs, so the
     * reader's lease can only be renewed by the data itself. */
    init_data_endpoint(&publication, 0);
    publication.liveliness_kind = NB_RTPS_LIVELINESS_MANUAL_BY_TOPIC;
    publication.liveliness_lease_ms = 400;
    CHECK(nb_rtps_participant_add_endpoint(
              publisher, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);
    init_data_endpoint(&subscription, 0);
    subscription.liveliness_kind = NB_RTPS_LIVELINESS_MANUAL_BY_TOPIC;
    subscription.liveliness_lease_ms = 400;
    CHECK(nb_rtps_participant_add_endpoint(
              subscriber, NB_RTPS_ENDPOINT_SUBSCRIPTION, &subscription) == NB_OK);

    CHECK(nb_rtps_participant_start(publisher) == NB_OK);
    CHECK(nb_rtps_participant_start(subscriber) == NB_OK);
    CHECK(wait_endpoint_count(&publisher_state, 0, 1, 5000));
    CHECK(wait_endpoint_count(&subscriber_state, 0, 1, 5000));

    /* Two lease periods of writes: every write asserts the writer, so no
     * liveliness change may be reported. */
    for (i = 0; i < 8; i++) {
        CHECK(nb_rtps_participant_publish(
                  publisher, &publication.endpoint_guid, payload, sizeof(payload)) == NB_OK);
        ntk_sleep_ms(100);
    }
    CHECK(liveliness_count(&status) == 0);

    /* Writes stop while the participant keeps running: the writer has to be
     * declared not-alive, which only the manual assertion rules can do (an
     * automatic lease would have been renewed by ParticipantMessageData). */
    for (waited = 0; waited < 3000 && liveliness_count(&status) == 0; waited += 20) {
        ntk_sleep_ms(20);
    }
    CHECK(liveliness_count(&status) > 0);
    ntk_mutex_lock(status.lock);
    CHECK(status.last_alive == 0);
    CHECK(status.not_alive_count == 1);
    ntk_mutex_unlock(status.lock);

    /* A single write asserts the writer again. */
    before = liveliness_count(&status);
    CHECK(nb_rtps_participant_publish(
              publisher, &publication.endpoint_guid, payload, sizeof(payload)) == NB_OK);
    for (waited = 0; waited < 3000; waited += 20) {
        int alive;

        /* liveliness_count() takes the lock itself, so read the flag the same
         * way instead of nesting a second acquisition (the mutex is not
         * recursive outside Windows). */
        ntk_mutex_lock(status.lock);
        alive = status.last_alive;
        ntk_mutex_unlock(status.lock);
        if (liveliness_count(&status) > before && alive == 1) {
            break;
        }
        ntk_sleep_ms(20);
    }
    CHECK(liveliness_count(&status) > before);
    {
        int alive;

        ntk_mutex_lock(status.lock);
        alive = status.last_alive;
        ntk_mutex_unlock(status.lock);
        CHECK(alive == 1);
    }

cleanup:
    nb_rtps_participant_stop(publisher);
    nb_rtps_participant_stop(subscriber);
    nb_rtps_participant_destroy(publisher);
    nb_rtps_participant_destroy(subscriber);
    if (publisher_state.lock) {
        ntk_mutex_destroy(publisher_state.lock);
    }
    if (subscriber_state.lock) {
        ntk_mutex_destroy(subscriber_state.lock);
    }
    if (status.lock) {
        ntk_mutex_destroy(status.lock);
    }
}

/*
 * Match status: an endpoint appears, matches, and goes away, while a same-topic
 * endpoint with a stricter deadline cannot match. This is the RTPS view of
 * PublicationMatchedStatus/SubscriptionMatchedStatus: `count` follows the
 * current number of matches (a drop means a peer left), `total_count` only
 * grows, and both sides report the event.
 */
static void test_match_status(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *publisher;
    nb_rtps_participant_t *subscriber;
    nb_rtps_endpoint_info_t publication;
    nb_rtps_endpoint_info_t subscription;
    nb_rtps_endpoint_info_t stricter;
    nb_rtps_endpoint_info_t group_reader;
    test_state_t publisher_state;
    test_state_t subscriber_state;
    status_state_t pub_status;
    status_state_t sub_status;
    status_state_t snap;
    uint16_t publisher_port;
    uint16_t subscriber_port;
    int waited;

    memset(&publisher_state, 0, sizeof(publisher_state));
    memset(&subscriber_state, 0, sizeof(subscriber_state));
    memset(&pub_status, 0, sizeof(pub_status));
    memset(&sub_status, 0, sizeof(sub_status));
    memset(&snap, 0, sizeof(snap));

    fill_config(&config, "match-publisher");
    publisher = nb_rtps_participant_create(&config);
    CHECK(publisher != NULL);
    if (!publisher) {
        return;
    }
    publisher_port = nb_rtps_participant_spdp_port(publisher);
    fill_config(&config, "match-subscriber");
    subscriber = nb_rtps_participant_create(&config);
    CHECK(subscriber != NULL);
    if (!subscriber) {
        nb_rtps_participant_destroy(publisher);
        return;
    }
    subscriber_port = nb_rtps_participant_spdp_port(subscriber);
    CHECK(nb_rtps_participant_add_peer(publisher, "127.0.0.1", subscriber_port) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(subscriber, "127.0.0.1", publisher_port) == NB_OK);

    publisher_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    subscriber_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    pub_status.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    sub_status.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(publisher_state.lock && subscriber_state.lock &&
          pub_status.lock && sub_status.lock);
    if (!publisher_state.lock || !subscriber_state.lock ||
        !pub_status.lock || !sub_status.lock) {
        goto cleanup;
    }
    nb_rtps_participant_set_callback(publisher, on_participant, &publisher_state);
    nb_rtps_participant_set_callback(subscriber, on_participant, &subscriber_state);
    nb_rtps_participant_set_endpoint_callback(publisher, on_endpoint, &publisher_state);
    nb_rtps_participant_set_endpoint_callback(subscriber, on_endpoint, &subscriber_state);
    CHECK(nb_rtps_participant_set_status_callback(publisher, on_status, &pub_status) == NB_OK);
    CHECK(nb_rtps_participant_set_status_callback(subscriber, on_status, &sub_status) == NB_OK);

    init_data_endpoint(&publication, 0);
    CHECK(nb_rtps_participant_add_endpoint(
              publisher, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);

    CHECK(nb_rtps_participant_start(publisher) == NB_OK);
    CHECK(nb_rtps_participant_start(subscriber) == NB_OK);
    /* The publication is discovered first, so the subscription is created
     * against a peer that is already known. */
    CHECK(wait_endpoint_count(&subscriber_state, 0, 1, 5000));

    init_data_endpoint(&subscription, 0);
    CHECK(nb_rtps_participant_add_endpoint(
              subscriber, NB_RTPS_ENDPOINT_SUBSCRIPTION, &subscription) == NB_OK);
    for (waited = 0; waited < 3000; waited += 20) {
        status_snapshot(&pub_status, &snap);
        if (snap.pub_matched > 0) {
            break;
        }
        ntk_sleep_ms(20);
    }
    status_snapshot(&pub_status, &snap);
    CHECK(snap.pub_matched > 0);
    CHECK(snap.match_count == 1);
    CHECK(snap.match_total >= 1);
    CHECK(snap.has_match_peer);
    CHECK(nb_rtps_guid_equal(&snap.match_peer, &subscription.endpoint_guid));

    /* The subscription was created after the publication was already known, so
     * it reports the match without waiting for another SEDP round. */
    status_snapshot(&sub_status, &snap);
    CHECK(snap.sub_matched > 0);
    CHECK(snap.match_count == 1);
    CHECK(snap.has_match_peer);
    CHECK(nb_rtps_guid_equal(&snap.match_peer, &publication.endpoint_guid));

    /* Same topic and type, but it asks for a tighter deadline than the
     * publication offers: incompatible, so it never counts as a match. */
    init_data_endpoint(&stricter, 0);
    stricter.deadline_ms = 100;
    CHECK(nb_rtps_participant_add_endpoint(
              subscriber, NB_RTPS_ENDPOINT_SUBSCRIPTION, &stricter) == NB_OK);
    for (waited = 0; waited < 3000; waited += 20) {
        status_snapshot(&pub_status, &snap);
        if (snap.incompatible > 0) {
            break;
        }
        ntk_sleep_ms(20);
    }
    status_snapshot(&pub_status, &snap);
    CHECK(snap.incompatible > 0);
    CHECK(strcmp(snap.reason, "deadline") == 0);
    CHECK(snap.has_match_peer);
    CHECK(nb_rtps_guid_equal(&snap.match_peer, &stricter.endpoint_guid));
    CHECK(snap.match_count == 1);

    status_snapshot(&sub_status, &snap);
    CHECK(snap.incompatible > 0);
    CHECK(strcmp(snap.reason, "deadline") == 0);

    /* Same topic again, but this reader wants GROUP presentation while the
     * writer only offers INSTANCE: incompatible on the access scope. */
    init_data_endpoint(&group_reader, 0);
    group_reader.presentation_scope = NB_RTPS_PRESENTATION_GROUP;
    CHECK(nb_rtps_participant_add_endpoint(
              subscriber, NB_RTPS_ENDPOINT_SUBSCRIPTION, &group_reader) == NB_OK);
    for (waited = 0; waited < 3000; waited += 20) {
        status_snapshot(&pub_status, &snap);
        if (strcmp(snap.reason, "presentation scope") == 0) {
            break;
        }
        ntk_sleep_ms(20);
    }
    status_snapshot(&pub_status, &snap);
    CHECK(strcmp(snap.reason, "presentation scope") == 0);
    CHECK(snap.match_count == 1);

    /* Removing the compatible subscription is a match count drop, not a new
     * match: the publication now has no matched reader. */
    CHECK(nb_rtps_participant_remove_endpoint(
              subscriber, &subscription.endpoint_guid) == NB_OK);
    for (waited = 0; waited < 3000; waited += 20) {
        status_snapshot(&pub_status, &snap);
        if (snap.pub_matched > 1) {
            break;
        }
        ntk_sleep_ms(20);
    }
    status_snapshot(&pub_status, &snap);
    CHECK(snap.pub_matched > 1);
    CHECK(snap.match_count == 0);

cleanup:
    nb_rtps_participant_stop(publisher);
    nb_rtps_participant_stop(subscriber);
    nb_rtps_participant_destroy(publisher);
    nb_rtps_participant_destroy(subscriber);
    if (publisher_state.lock) {
        ntk_mutex_destroy(publisher_state.lock);
    }
    if (subscriber_state.lock) {
        ntk_mutex_destroy(subscriber_state.lock);
    }
    if (pub_status.lock) {
        ntk_mutex_destroy(pub_status.lock);
    }
    if (sub_status.lock) {
        ntk_mutex_destroy(sub_status.lock);
    }
}

/*
 * OFFERED_DEADLINE_MISSED: a publication that does not write within its
 * deadline reports it, once per period, with a count that keeps advancing.
 */
static void test_offered_deadline_missed(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *participant;
    nb_rtps_endpoint_info_t publication;
    status_state_t status;
    int waited;

    memset(&status, 0, sizeof(status));
    status.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(status.lock != NULL);
    fill_config(&config, "deadline-offered");
    participant = nb_rtps_participant_create(&config);
    CHECK(participant != NULL);
    if (!participant || !status.lock) {
        if (participant) {
            nb_rtps_participant_destroy(participant);
        }
        if (status.lock) {
            ntk_mutex_destroy(status.lock);
        }
        return;
    }
    CHECK(nb_rtps_participant_set_status_callback(participant, on_status, &status) == NB_OK);
    init_data_endpoint(&publication, 1);
    publication.deadline_ms = 100;
    CHECK(nb_rtps_participant_add_endpoint(
              participant, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);
    CHECK(nb_rtps_participant_start(participant) == NB_OK);

    for (waited = 0; waited < 1500 && status_count(&status, 1) == 0; waited += 20) {
        ntk_sleep_ms(20);
    }
    CHECK(status_count(&status, 1) > 0);
    CHECK(status_count(&status, 0) == 0);
    CHECK(nb_rtps_guid_equal(&status.last_endpoint, &publication.endpoint_guid));
    CHECK(status.last_count > 0);

    nb_rtps_participant_stop(participant);
    nb_rtps_participant_destroy(participant);
    ntk_mutex_destroy(status.lock);
}

/*
 * REQUESTED_DEADLINE_MISSED: once a writer is matched, a subscription that
 * receives nothing within its deadline reports the miss, even if the writer
 * never wrote a single sample.
 */
static void test_requested_deadline_missed(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *publisher;
    nb_rtps_participant_t *subscriber;
    nb_rtps_endpoint_info_t publication;
    nb_rtps_endpoint_info_t subscription;
    test_state_t publisher_state;
    test_state_t subscriber_state;
    status_state_t status;
    uint16_t publisher_port;
    uint16_t subscriber_port;
    int waited;

    memset(&publisher_state, 0, sizeof(publisher_state));
    memset(&subscriber_state, 0, sizeof(subscriber_state));
    memset(&status, 0, sizeof(status));

    fill_config(&config, "deadline-requested-publisher");
    publisher = nb_rtps_participant_create(&config);
    CHECK(publisher != NULL);
    if (!publisher) {
        return;
    }
    publisher_port = nb_rtps_participant_spdp_port(publisher);
    fill_config(&config, "deadline-requested-subscriber");
    subscriber = nb_rtps_participant_create(&config);
    CHECK(subscriber != NULL);
    if (!subscriber) {
        nb_rtps_participant_destroy(publisher);
        return;
    }
    subscriber_port = nb_rtps_participant_spdp_port(subscriber);
    CHECK(nb_rtps_participant_add_peer(publisher, "127.0.0.1", subscriber_port) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(subscriber, "127.0.0.1", publisher_port) == NB_OK);

    publisher_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    subscriber_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    status.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(publisher_state.lock && subscriber_state.lock && status.lock);
    if (!publisher_state.lock || !subscriber_state.lock || !status.lock) {
        goto cleanup;
    }
    nb_rtps_participant_set_callback(publisher, on_participant, &publisher_state);
    nb_rtps_participant_set_callback(subscriber, on_participant, &subscriber_state);
    nb_rtps_participant_set_endpoint_callback(publisher, on_endpoint, &publisher_state);
    nb_rtps_participant_set_endpoint_callback(subscriber, on_endpoint, &subscriber_state);
    CHECK(nb_rtps_participant_set_status_callback(subscriber, on_status, &status) == NB_OK);

    /* Both endpoints exist before discovery, so the pair is armed at match time
     * rather than at the first sample. */
    init_data_endpoint(&publication, 1);
    publication.deadline_ms = 100;
    CHECK(nb_rtps_participant_add_endpoint(
              publisher, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);
    init_data_endpoint(&subscription, 1);
    subscription.deadline_ms = 100;
    CHECK(nb_rtps_participant_add_endpoint(
              subscriber, NB_RTPS_ENDPOINT_SUBSCRIPTION, &subscription) == NB_OK);

    CHECK(nb_rtps_participant_start(publisher) == NB_OK);
    CHECK(nb_rtps_participant_start(subscriber) == NB_OK);
    CHECK(wait_endpoint_count(&publisher_state, 0, 1, 5000));
    CHECK(wait_endpoint_count(&subscriber_state, 0, 1, 5000));

    /* The writer never writes; the reader must still notice. */
    for (waited = 0; waited < 2000 && status_count(&status, 0) == 0; waited += 20) {
        ntk_sleep_ms(20);
    }
    CHECK(status_count(&status, 0) > 0);
    CHECK(nb_rtps_guid_equal(&status.last_endpoint, &subscription.endpoint_guid));

cleanup:
    nb_rtps_participant_stop(publisher);
    nb_rtps_participant_stop(subscriber);
    nb_rtps_participant_destroy(publisher);
    nb_rtps_participant_destroy(subscriber);
    if (publisher_state.lock) {
        ntk_mutex_destroy(publisher_state.lock);
    }
    if (subscriber_state.lock) {
        ntk_mutex_destroy(subscriber_state.lock);
    }
    if (status.lock) {
        ntk_mutex_destroy(status.lock);
    }
}

typedef struct count_state {
    ntk_mutex *lock;
    int count;
    size_t len;
} count_state_t;

static void on_count_data(void *user,
                          const nb_rtps_guid_t *reader_guid,
                          const nb_rtps_guid_t *writer_guid,
                          const nb_rtps_dds_message_t *message)
{
    count_state_t *state = (count_state_t *)user;

    (void)reader_guid;
    (void)writer_guid;
    if (!state || !message) {
        return;
    }
    ntk_mutex_lock(state->lock);
    state->count++;
    state->len = message->payload_len;
    ntk_mutex_unlock(state->lock);
}

static int count_delivered(count_state_t *state)
{
    int count;

    ntk_mutex_lock(state->lock);
    count = state->count;
    ntk_mutex_unlock(state->lock);
    return count;
}

/*
 * A remote endpoint may advertise one unicast locator per interface. The first
 * one can be unreachable from the sender, so a sample has to go to all of them;
 * this is what Cyclone does for a proxy reader with several addresses. The
 * unreachable locator here is in the documentation range, so the packet is
 * dropped instead of leaving the host.
 */
static void test_multi_locator_delivery(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *publisher;
    nb_rtps_participant_t *subscriber;
    nb_rtps_participant_info_t subscriber_info;
    nb_rtps_endpoint_info_t publication;
    nb_rtps_endpoint_info_t subscription;
    nb_rtps_dds_message_t message;
    test_state_t publisher_state;
    test_state_t subscriber_state;
    count_state_t count_state;
    uint8_t payload[64];
    uint16_t publisher_port;
    uint16_t subscriber_port;
    uint16_t data_port;
    int waited;
    size_t i;

    memset(&publisher_state, 0, sizeof(publisher_state));
    memset(&subscriber_state, 0, sizeof(subscriber_state));
    memset(&count_state, 0, sizeof(count_state));

    fill_config(&config, "multi-locator-publisher");
    publisher = nb_rtps_participant_create(&config);
    CHECK(publisher != NULL);
    if (!publisher) {
        return;
    }
    publisher_port = nb_rtps_participant_spdp_port(publisher);
    fill_config(&config, "multi-locator-subscriber");
    subscriber = nb_rtps_participant_create(&config);
    CHECK(subscriber != NULL);
    if (!subscriber) {
        nb_rtps_participant_destroy(publisher);
        return;
    }
    subscriber_port = nb_rtps_participant_spdp_port(subscriber);
    CHECK(nb_rtps_participant_add_peer(publisher, "127.0.0.1", subscriber_port) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(subscriber, "127.0.0.1", publisher_port) == NB_OK);

    publisher_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    subscriber_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    count_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(publisher_state.lock && subscriber_state.lock && count_state.lock);
    if (!publisher_state.lock || !subscriber_state.lock || !count_state.lock) {
        goto cleanup;
    }
    nb_rtps_participant_set_callback(publisher, on_participant, &publisher_state);
    nb_rtps_participant_set_callback(subscriber, on_participant, &subscriber_state);
    nb_rtps_participant_set_endpoint_callback(publisher, on_endpoint, &publisher_state);
    nb_rtps_participant_set_endpoint_callback(subscriber, on_endpoint, &subscriber_state);
    nb_rtps_participant_set_data_callback(subscriber, on_count_data, &count_state);
    CHECK(nb_rtps_participant_start(publisher) == NB_OK);
    CHECK(nb_rtps_participant_start(subscriber) == NB_OK);
    CHECK(wait_count(&publisher_state, 0, 1, 5000));
    CHECK(wait_count(&subscriber_state, 0, 1, 5000));
    CHECK(nb_rtps_participant_local_info(subscriber, &subscriber_info) == NB_OK);
    data_port = (uint16_t)subscriber_info.metatraffic_unicast.port;
    CHECK(data_port != 0);

    /* First locator is unreachable, second is the subscriber's data socket. */
    memset(&publication, 0, sizeof(publication));
    init_data_endpoint(&publication, 1);
    nb_rtps_locator_ipv4(&publication.unicast[0], 192, 0, 2, 1, data_port);
    nb_rtps_locator_ipv4(&publication.unicast[1], 127, 0, 0, 1, data_port);
    publication.unicast_count = 2;
    CHECK(nb_rtps_participant_add_endpoint(
              publisher, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);

    memset(&subscription, 0, sizeof(subscription));
    init_data_endpoint(&subscription, 1);
    CHECK(nb_rtps_participant_add_endpoint(
              subscriber, NB_RTPS_ENDPOINT_SUBSCRIPTION, &subscription) == NB_OK);
    CHECK(wait_endpoint_count(&subscriber_state, 0, 1, 5000));
    CHECK(wait_endpoint_count(&publisher_state, 0, 1, 5000));

    for (i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 11u + 3u);
    }
    message.payload = payload;
    message.payload_len = sizeof(payload);
    CHECK(nb_rtps_participant_publish_dds_message(
              publisher, &publication.endpoint_guid, &message) == NB_OK);
    for (waited = 0; waited < 2000 && count_delivered(&count_state) == 0; waited += 20) {
        ntk_sleep_ms(20);
    }
    CHECK(count_delivered(&count_state) == 1);
    CHECK(count_state.len == sizeof(payload));

cleanup:
    if (publisher) {
        nb_rtps_participant_stop(publisher);
    }
    if (subscriber) {
        nb_rtps_participant_stop(subscriber);
    }
    if (publisher) {
        nb_rtps_participant_destroy(publisher);
    }
    if (subscriber) {
        nb_rtps_participant_destroy(subscriber);
    }
    if (publisher_state.lock) {
        ntk_mutex_destroy(publisher_state.lock);
    }
    if (subscriber_state.lock) {
        ntk_mutex_destroy(subscriber_state.lock);
    }
    if (count_state.lock) {
        ntk_mutex_destroy(count_state.lock);
    }
}

/*
 * Generic application types: a subscription whose type name is not
 * "dds::Message" receives the raw CDR body of the sample, so an application can
 * plug in any codec (Micro-CDR generated code included) without a registry.
 */
static void test_generic_type_delivery(void)
{
    static const size_t body_sizes[] = {64u, 3000u};
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *publisher;
    nb_rtps_participant_t *subscriber;
    nb_rtps_endpoint_info_t publication;
    nb_rtps_endpoint_info_t subscription;
    test_state_t publisher_state;
    test_state_t subscriber_state;
    frag_state_t frag_state;
    uint16_t publisher_port;
    uint16_t subscriber_port;
    size_t s;

    memset(&publisher_state, 0, sizeof(publisher_state));
    memset(&subscriber_state, 0, sizeof(subscriber_state));
    memset(&frag_state, 0, sizeof(frag_state));

    fill_config(&config, "generic-publisher");
    publisher = nb_rtps_participant_create(&config);
    CHECK(publisher != NULL);
    if (!publisher) {
        return;
    }
    publisher_port = nb_rtps_participant_spdp_port(publisher);
    fill_config(&config, "generic-subscriber");
    subscriber = nb_rtps_participant_create(&config);
    CHECK(subscriber != NULL);
    if (!subscriber) {
        nb_rtps_participant_destroy(publisher);
        return;
    }
    subscriber_port = nb_rtps_participant_spdp_port(subscriber);
    CHECK(nb_rtps_participant_add_peer(publisher, "127.0.0.1", subscriber_port) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(subscriber, "127.0.0.1", publisher_port) == NB_OK);

    publisher_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    subscriber_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    frag_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(publisher_state.lock && subscriber_state.lock && frag_state.lock);
    if (!publisher_state.lock || !subscriber_state.lock || !frag_state.lock) {
        goto cleanup;
    }
    nb_rtps_participant_set_callback(publisher, on_participant, &publisher_state);
    nb_rtps_participant_set_callback(subscriber, on_participant, &subscriber_state);
    nb_rtps_participant_set_endpoint_callback(publisher, on_endpoint, &publisher_state);
    nb_rtps_participant_set_endpoint_callback(subscriber, on_endpoint, &subscriber_state);
    nb_rtps_participant_set_data_callback(subscriber, on_frag_data, &frag_state);
    CHECK(nb_rtps_participant_start(publisher) == NB_OK);
    CHECK(nb_rtps_participant_start(subscriber) == NB_OK);
    CHECK(wait_count(&publisher_state, 0, 1, 5000));
    CHECK(wait_count(&subscriber_state, 0, 1, 5000));

    memset(&publication, 0, sizeof(publication));
    (void)snprintf(publication.topic_name, sizeof(publication.topic_name), "test/Blob");
    (void)snprintf(publication.type_name, sizeof(publication.type_name), "test::Blob");
    publication.reliability = NB_RTPS_RELIABILITY_RELIABLE;
    publication.history_kind = NB_RTPS_HISTORY_KEEP_LAST;
    publication.history_depth = 4;
    CHECK(nb_rtps_participant_add_endpoint(
              publisher, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);

    memset(&subscription, 0, sizeof(subscription));
    (void)snprintf(subscription.topic_name, sizeof(subscription.topic_name), "test/Blob");
    (void)snprintf(subscription.type_name, sizeof(subscription.type_name), "test::Blob");
    subscription.reliability = NB_RTPS_RELIABILITY_RELIABLE;
    subscription.history_kind = NB_RTPS_HISTORY_KEEP_LAST;
    subscription.history_depth = 4;
    CHECK(nb_rtps_participant_add_endpoint(
              subscriber, NB_RTPS_ENDPOINT_SUBSCRIPTION, &subscription) == NB_OK);
    CHECK(wait_endpoint_count(&subscriber_state, 0, 1, 5000));
    CHECK(wait_endpoint_count(&publisher_state, 0, 1, 5000));

    for (s = 0; s < sizeof(body_sizes) / sizeof(body_sizes[0]); s++) {
        size_t body_len = body_sizes[s];
        uint8_t *sample = (uint8_t *)malloc(body_len + NB_RTPS_CDR_HEADER_SIZE);
        size_t i;

        CHECK(sample != NULL);
        if (!sample) {
            continue;
        }
        sample[0] = 0x00;
        sample[1] = 0x01; /* CDR_LE */
        sample[2] = 0x00;
        sample[3] = 0x00;
        for (i = 0; i < body_len; i++) {
            sample[NB_RTPS_CDR_HEADER_SIZE + i] = (uint8_t)((i * 17u + 5u) & 0xffu);
        }
        CHECK(send_raw_sample(publisher,
                              &publication.endpoint_guid,
                              sample,
                              body_len + NB_RTPS_CDR_HEADER_SIZE,
                              sample + NB_RTPS_CDR_HEADER_SIZE,
                              body_len,
                              &frag_state));
        free(sample);
    }

cleanup:
    if (publisher) {
        nb_rtps_participant_stop(publisher);
    }
    if (subscriber) {
        nb_rtps_participant_stop(subscriber);
    }
    if (publisher) {
        nb_rtps_participant_destroy(publisher);
    }
    if (subscriber) {
        nb_rtps_participant_destroy(subscriber);
    }
    if (publisher_state.lock) {
        ntk_mutex_destroy(publisher_state.lock);
    }
    if (subscriber_state.lock) {
        ntk_mutex_destroy(subscriber_state.lock);
    }
    if (frag_state.lock) {
        ntk_mutex_destroy(frag_state.lock);
    }
}

/*
 * Partition matching follows DDS rules: names are comma-separated lists and
 * '*' is a wildcard, so "alpha,team-*" matches "team-blue" but not "other".
 */
static void test_partition_matching(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *publisher = NULL;
    nb_rtps_participant_t *matched = NULL;
    nb_rtps_participant_t *unmatched = NULL;
    nb_rtps_endpoint_info_t publication;
    nb_rtps_endpoint_info_t subscription;
    test_state_t publisher_state;
    test_state_t matched_state;
    test_state_t unmatched_state;
    frag_state_t matched_data;
    frag_state_t unmatched_data;
    nb_rtps_dds_message_t message;
    uint8_t payload[3] = {0x21, 0x22, 0x23};
    uint8_t serialized[16];
    size_t serialized_len = 0;
    uint16_t publisher_port;
    uint16_t matched_port;
    int i;

    memset(&publisher_state, 0, sizeof(publisher_state));
    memset(&matched_state, 0, sizeof(matched_state));
    memset(&unmatched_state, 0, sizeof(unmatched_state));
    memset(&matched_data, 0, sizeof(matched_data));
    memset(&unmatched_data, 0, sizeof(unmatched_data));

    fill_config(&config, "part-publisher");
    publisher = nb_rtps_participant_create(&config);
    CHECK(publisher != NULL);
    if (!publisher) {
        return;
    }
    publisher_port = nb_rtps_participant_spdp_port(publisher);
    fill_config(&config, "part-matched");
    matched = nb_rtps_participant_create(&config);
    fill_config(&config, "part-unmatched");
    unmatched = nb_rtps_participant_create(&config);
    CHECK(matched != NULL && unmatched != NULL);
    if (!matched || !unmatched) {
        goto cleanup;
    }
    matched_port = nb_rtps_participant_spdp_port(matched);
    CHECK(nb_rtps_participant_add_peer(publisher, "127.0.0.1", matched_port) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(matched, "127.0.0.1", publisher_port) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(
              publisher,
              "127.0.0.1",
              nb_rtps_participant_spdp_port(unmatched)) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(unmatched, "127.0.0.1", publisher_port) == NB_OK);

    publisher_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    matched_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    unmatched_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    matched_data.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    unmatched_data.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(publisher_state.lock && matched_state.lock && unmatched_state.lock);
    CHECK(matched_data.lock && unmatched_data.lock);
    if (!publisher_state.lock || !matched_state.lock || !unmatched_state.lock ||
        !matched_data.lock || !unmatched_data.lock) {
        goto cleanup;
    }
    nb_rtps_participant_set_callback(publisher, on_participant, &publisher_state);
    nb_rtps_participant_set_callback(matched, on_participant, &matched_state);
    nb_rtps_participant_set_callback(unmatched, on_participant, &unmatched_state);
    nb_rtps_participant_set_endpoint_callback(publisher, on_endpoint, &publisher_state);
    nb_rtps_participant_set_data_callback(matched, on_frag_data, &matched_data);
    nb_rtps_participant_set_data_callback(unmatched, on_frag_data, &unmatched_data);
    CHECK(nb_rtps_participant_start(publisher) == NB_OK);
    CHECK(nb_rtps_participant_start(matched) == NB_OK);
    CHECK(nb_rtps_participant_start(unmatched) == NB_OK);
    CHECK(wait_count(&publisher_state, 0, 2, 5000));
    CHECK(wait_count(&matched_state, 0, 1, 5000));
    CHECK(wait_count(&unmatched_state, 0, 1, 5000));

    init_data_endpoint(&publication, 0);
    (void)snprintf(publication.partition, sizeof(publication.partition), "alpha,team-*");
    CHECK(nb_rtps_participant_add_endpoint(
              publisher, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);

    init_data_endpoint(&subscription, 0);
    (void)snprintf(subscription.partition, sizeof(subscription.partition), "team-blue");
    CHECK(nb_rtps_participant_add_endpoint(
              matched, NB_RTPS_ENDPOINT_SUBSCRIPTION, &subscription) == NB_OK);
    (void)snprintf(subscription.partition, sizeof(subscription.partition), "other");
    CHECK(nb_rtps_participant_add_endpoint(
              unmatched, NB_RTPS_ENDPOINT_SUBSCRIPTION, &subscription) == NB_OK);
    CHECK(wait_endpoint_count(&publisher_state, 0, 2, 5000));

    message.payload = payload;
    message.payload_len = sizeof(payload);
    CHECK(nb_rtps_dds_message_serialize(
              &message, serialized, sizeof(serialized), &serialized_len) == NB_OK);
    for (i = 0; i < 60 && !frag_data_received(&matched_data); i++) {
        (void)nb_rtps_participant_publish(
            publisher, &publication.endpoint_guid, serialized, serialized_len);
        ntk_sleep_ms(50);
    }
    CHECK(frag_data_received(&matched_data));
    ntk_sleep_ms(500);
    CHECK(!frag_data_received(&unmatched_data));
    if (frag_data_received(&unmatched_data)) {
        nb_log_error("a subscription in partition \"other\" received a sample");
    }

cleanup:
    if (publisher) {
        nb_rtps_participant_stop(publisher);
    }
    if (matched) {
        nb_rtps_participant_stop(matched);
    }
    if (unmatched) {
        nb_rtps_participant_stop(unmatched);
    }
    if (publisher) {
        nb_rtps_participant_destroy(publisher);
    }
    if (matched) {
        nb_rtps_participant_destroy(matched);
    }
    if (unmatched) {
        nb_rtps_participant_destroy(unmatched);
    }
    if (publisher_state.lock) {
        ntk_mutex_destroy(publisher_state.lock);
    }
    if (matched_state.lock) {
        ntk_mutex_destroy(matched_state.lock);
    }
    if (unmatched_state.lock) {
        ntk_mutex_destroy(unmatched_state.lock);
    }
    if (matched_data.lock) {
        ntk_mutex_destroy(matched_data.lock);
    }
    if (unmatched_data.lock) {
        ntk_mutex_destroy(unmatched_data.lock);
    }
}

static void test_multicast(void)
{
    const char *enabled = getenv("NB_TEST_RTPS_MULTICAST");
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *a;
    nb_rtps_participant_t *b;
    test_state_t state_a;
    test_state_t state_b;

    if (!enabled || strcmp(enabled, "1") != 0) {
        nb_log_info("multicast test skipped; set NB_TEST_RTPS_MULTICAST=1");
        return;
    }
    memset(&state_a, 0, sizeof(state_a));
    memset(&state_b, 0, sizeof(state_b));
    fill_config(&config, "mcast-a");
    config.bind_host = "0.0.0.0";
    config.multicast_group = "239.255.0.1";
    config.spdp_port = 47123;
    config.lease_ms = 3000;
    a = nb_rtps_participant_create(&config);
    CHECK(a != NULL);
    if (!a) {
        return;
    }
    config.name = "mcast-b";
    b = nb_rtps_participant_create(&config);
    CHECK(b != NULL);
    if (!b) {
        nb_rtps_participant_destroy(a);
        return;
    }
    state_a.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    state_b.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(state_a.lock != NULL);
    CHECK(state_b.lock != NULL);
    if (!state_a.lock || !state_b.lock) {
        nb_rtps_participant_destroy(a);
        nb_rtps_participant_destroy(b);
        if (state_a.lock) {
            ntk_mutex_destroy(state_a.lock);
        }
        if (state_b.lock) {
            ntk_mutex_destroy(state_b.lock);
        }
        return;
    }
    nb_rtps_participant_set_callback(a, on_participant, &state_a);
    nb_rtps_participant_set_callback(b, on_participant, &state_b);
    CHECK(nb_rtps_participant_start(a) == NB_OK);
    CHECK(nb_rtps_participant_start(b) == NB_OK);
    CHECK(wait_count(&state_a, 0, 1, 5000));
    CHECK(wait_count(&state_b, 0, 1, 5000));
    nb_rtps_participant_stop(a);
    nb_rtps_participant_stop(b);
    nb_rtps_participant_destroy(a);
    nb_rtps_participant_destroy(b);
    ntk_mutex_destroy(state_a.lock);
    ntk_mutex_destroy(state_b.lock);
}

/*
 * A reliable publication that sent samples before the subscriber existed must
 * replay them once the endpoints match: the writer HEARTBEATs its history and
 * the reader ACKNACKs the missing samples.
 */
static void test_reliable_late_joiner(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *publisher;
    nb_rtps_participant_t *subscriber;
    nb_rtps_endpoint_info_t publication;
    nb_rtps_endpoint_info_t subscription;
    nb_rtps_participant_info_t publisher_info;
    nb_rtps_dds_message_t message;
    test_state_t publisher_state;
    test_state_t subscriber_state;
    reliable_data_state_t data_state;
    static const uint8_t expected[] = {1, 2, 3, 4};
    uint16_t publisher_port;
    uint16_t subscriber_port;
    int i;
    int sent;

    memset(&publisher_state, 0, sizeof(publisher_state));
    memset(&subscriber_state, 0, sizeof(subscriber_state));
    memset(&data_state, 0, sizeof(data_state));

    fill_config(&config, "rel-publisher");
    config.data_heartbeat_ms = 100;
    config.data_history_depth = 8;
    publisher = nb_rtps_participant_create(&config);
    CHECK(publisher != NULL);
    if (!publisher) {
        return;
    }
    publisher_port = nb_rtps_participant_spdp_port(publisher);
    fill_config(&config, "rel-subscriber");
    config.data_heartbeat_ms = 100;
    config.data_history_depth = 8;
    subscriber = nb_rtps_participant_create(&config);
    CHECK(subscriber != NULL);
    if (!subscriber) {
        nb_rtps_participant_destroy(publisher);
        return;
    }
    subscriber_port = nb_rtps_participant_spdp_port(subscriber);
    CHECK(nb_rtps_participant_add_peer(publisher, "127.0.0.1", subscriber_port) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(subscriber, "127.0.0.1", publisher_port) == NB_OK);

    publisher_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    subscriber_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    data_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(publisher_state.lock != NULL);
    CHECK(subscriber_state.lock != NULL);
    CHECK(data_state.lock != NULL);
    if (!publisher_state.lock || !subscriber_state.lock || !data_state.lock) {
        nb_rtps_participant_destroy(publisher);
        nb_rtps_participant_destroy(subscriber);
        if (publisher_state.lock) {
            ntk_mutex_destroy(publisher_state.lock);
        }
        if (subscriber_state.lock) {
            ntk_mutex_destroy(subscriber_state.lock);
        }
        if (data_state.lock) {
            ntk_mutex_destroy(data_state.lock);
        }
        return;
    }
    nb_rtps_participant_set_callback(publisher, on_participant, &publisher_state);
    nb_rtps_participant_set_callback(subscriber, on_participant, &subscriber_state);
    nb_rtps_participant_set_endpoint_callback(publisher, on_endpoint, &publisher_state);
    nb_rtps_participant_set_endpoint_callback(subscriber, on_endpoint, &subscriber_state);
    nb_rtps_participant_set_data_callback(subscriber, on_reliable_data, &data_state);
    CHECK(nb_rtps_participant_start(publisher) == NB_OK);
    CHECK(nb_rtps_participant_start(subscriber) == NB_OK);
    CHECK(wait_count(&publisher_state, 0, 1, 5000));
    CHECK(wait_count(&subscriber_state, 0, 1, 5000));

    memset(&publication, 0, sizeof(publication));
    (void)snprintf(publication.topic_name,
                   sizeof(publication.topic_name),
                   "%s",
                   NB_RTPS_DDS_MESSAGE_TOPIC);
    (void)snprintf(publication.type_name,
                   sizeof(publication.type_name),
                   "%s",
                   NB_RTPS_DDS_MESSAGE_TYPE);
    publication.reliability = NB_RTPS_RELIABILITY_RELIABLE;
    publication.durability = NB_RTPS_DURABILITY_TRANSIENT_LOCAL;
    publication.history_kind = NB_RTPS_HISTORY_KEEP_LAST;
    publication.history_depth = 4;
    CHECK(nb_rtps_participant_add_endpoint(
              publisher, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);

    /* No matching subscription yet: the samples stay in the writer history
     * and the TRANSIENT_LOCAL durability entitles the late joiner to them. */
    for (i = 0; i < 3; i++) {
        message.payload = &expected[i];
        message.payload_len = 1;
        (void)nb_rtps_participant_publish_dds_message(
            publisher, &publication.endpoint_guid, &message);
    }

    memset(&subscription, 0, sizeof(subscription));
    (void)snprintf(subscription.topic_name,
                   sizeof(subscription.topic_name),
                   "%s",
                   NB_RTPS_DDS_MESSAGE_TOPIC);
    (void)snprintf(subscription.type_name,
                   sizeof(subscription.type_name),
                   "%s",
                   NB_RTPS_DDS_MESSAGE_TYPE);
    subscription.reliability = NB_RTPS_RELIABILITY_RELIABLE;
    subscription.durability = NB_RTPS_DURABILITY_TRANSIENT_LOCAL;
    subscription.history_kind = NB_RTPS_HISTORY_KEEP_LAST;
    subscription.history_depth = 4;
    CHECK(nb_rtps_participant_add_endpoint(
              subscriber, NB_RTPS_ENDPOINT_SUBSCRIPTION, &subscription) == NB_OK);
    CHECK(wait_endpoint_count(&subscriber_state, 0, 1, 5000));
    CHECK(wait_endpoint_count(&publisher_state, 0, 1, 5000));

    CHECK(wait_reliable_data(&data_state, 3, 5000));

    /* Steady state: a new sample arrives once, while the replayed history is
     * not delivered again. */
    message.payload = &expected[3];
    message.payload_len = 1;
    sent = 0;
    for (i = 0; i < 100 && !sent; i++) {
        if (nb_rtps_participant_publish_dds_message(
                publisher, &publication.endpoint_guid, &message) == NB_OK) {
            sent = 1;
        } else {
            ntk_sleep_ms(20);
        }
    }
    CHECK(sent);
    CHECK(wait_reliable_data(&data_state, 4, 5000));
    ntk_sleep_ms(300); /* allow any duplicate retransmit to show up */
    CHECK(reliable_data_count(&data_state) == 4);

    CHECK(nb_rtps_participant_local_info(publisher, &publisher_info) == NB_OK);
    for (i = 0; i < 4; i++) {
        CHECK(data_state.payload_lens[i] == 1);
        CHECK(data_state.payloads[i][0] == expected[i]);
        CHECK(nb_rtps_guid_equal(&data_state.reader_guids[i],
                                 &subscription.endpoint_guid));
        CHECK(nb_rtps_guid_prefix_equal(&data_state.writer_guids[i].prefix,
                                        &publisher_info.guid.prefix));
        CHECK(nb_rtps_entity_id_equal(&data_state.writer_guids[i].entity,
                                      &publication.endpoint_guid.entity));
    }

    nb_rtps_participant_stop(publisher);
    nb_rtps_participant_stop(subscriber);
    nb_rtps_participant_destroy(publisher);
    nb_rtps_participant_destroy(subscriber);
    ntk_mutex_destroy(publisher_state.lock);
    ntk_mutex_destroy(subscriber_state.lock);
    ntk_mutex_destroy(data_state.lock);
}

/*
 * Late-joiner durability. A TRANSIENT_LOCAL publication keeps its history and
 * replays it when a matching reader appears later, even when the pair is
 * best effort. A VOLATILE publication keeps nothing for a reader that was not
 * matched when the sample was written.
 */
static void run_late_joiner_case(const char *prefix,
                                 uint32_t durability,
                                 uint32_t history_kind,
                                 int sample_count,
                                 int expect_count)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *publisher = NULL;
    nb_rtps_participant_t *subscriber = NULL;
    nb_rtps_endpoint_info_t publication;
    nb_rtps_endpoint_info_t subscription;
    nb_rtps_dds_message_t message;
    test_state_t publisher_state;
    test_state_t subscriber_state;
    reliable_data_state_t data_state;
    uint8_t samples[8];
    char name[64];
    uint16_t publisher_port;
    uint16_t subscriber_port;
    int i;

    memset(&publisher_state, 0, sizeof(publisher_state));
    memset(&subscriber_state, 0, sizeof(subscriber_state));
    memset(&data_state, 0, sizeof(data_state));
    for (i = 0; i < sample_count && i < (int)sizeof(samples); i++) {
        samples[i] = (uint8_t)(0xa1 + i);
    }

    (void)snprintf(name, sizeof(name), "%s-publisher", prefix);
    fill_config(&config, name);
    publisher = nb_rtps_participant_create(&config);
    CHECK(publisher != NULL);
    if (!publisher) {
        return;
    }
    publisher_port = nb_rtps_participant_spdp_port(publisher);
    (void)snprintf(name, sizeof(name), "%s-subscriber", prefix);
    fill_config(&config, name);
    subscriber = nb_rtps_participant_create(&config);
    CHECK(subscriber != NULL);
    if (!subscriber) {
        nb_rtps_participant_destroy(publisher);
        return;
    }
    subscriber_port = nb_rtps_participant_spdp_port(subscriber);
    CHECK(nb_rtps_participant_add_peer(publisher, "127.0.0.1", subscriber_port) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(subscriber, "127.0.0.1", publisher_port) == NB_OK);

    publisher_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    subscriber_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    data_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(publisher_state.lock && subscriber_state.lock && data_state.lock);
    if (!publisher_state.lock || !subscriber_state.lock || !data_state.lock) {
        goto cleanup;
    }
    nb_rtps_participant_set_callback(publisher, on_participant, &publisher_state);
    nb_rtps_participant_set_callback(subscriber, on_participant, &subscriber_state);
    nb_rtps_participant_set_endpoint_callback(publisher, on_endpoint, &publisher_state);
    nb_rtps_participant_set_endpoint_callback(subscriber, on_endpoint, &subscriber_state);
    nb_rtps_participant_set_data_callback(subscriber, on_reliable_data, &data_state);
    CHECK(nb_rtps_participant_start(publisher) == NB_OK);
    CHECK(nb_rtps_participant_start(subscriber) == NB_OK);
    CHECK(wait_count(&publisher_state, 0, 1, 5000));
    CHECK(wait_count(&subscriber_state, 0, 1, 5000));

    init_data_endpoint(&publication, 0);
    publication.durability = durability;
    publication.history_kind = history_kind;
    /* A KEEP_ALL publication must not be limited by the configured depth. */
    publication.history_depth = 2;
    CHECK(nb_rtps_participant_add_endpoint(
              publisher, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);

    /* Written before any reader matches. */
    for (i = 0; i < sample_count; i++) {
        message.payload = &samples[i];
        message.payload_len = 1;
        (void)nb_rtps_participant_publish_dds_message(
            publisher, &publication.endpoint_guid, &message);
    }
    ntk_sleep_ms(100);

    init_data_endpoint(&subscription, 0);
    subscription.durability = durability;
    subscription.history_kind = history_kind;
    subscription.history_depth = 2;
    CHECK(nb_rtps_participant_add_endpoint(
              subscriber, NB_RTPS_ENDPOINT_SUBSCRIPTION, &subscription) == NB_OK);
    CHECK(wait_endpoint_count(&subscriber_state, 0, 1, 5000));
    CHECK(wait_endpoint_count(&publisher_state, 0, 1, 5000));

    if (expect_count > 0) {
        CHECK(wait_reliable_data(&data_state, expect_count, 5000));
        ntk_sleep_ms(300);
        CHECK(reliable_data_count(&data_state) == expect_count);
        for (i = 0; i < expect_count; i++) {
            CHECK(data_state.payload_lens[i] == 1);
            CHECK(data_state.payloads[i][0] == samples[i]);
        }
    } else {
        ntk_sleep_ms(1200);
        CHECK(reliable_data_count(&data_state) == 0);
    }

cleanup:
    if (publisher) {
        nb_rtps_participant_stop(publisher);
    }
    if (subscriber) {
        nb_rtps_participant_stop(subscriber);
    }
    if (publisher) {
        nb_rtps_participant_destroy(publisher);
    }
    if (subscriber) {
        nb_rtps_participant_destroy(subscriber);
    }
    if (publisher_state.lock) {
        ntk_mutex_destroy(publisher_state.lock);
    }
    if (subscriber_state.lock) {
        ntk_mutex_destroy(subscriber_state.lock);
    }
    if (data_state.lock) {
        ntk_mutex_destroy(data_state.lock);
    }
}

static void test_late_joiner_durability(void)
{
    run_late_joiner_case(
        "tl", NB_RTPS_DURABILITY_TRANSIENT_LOCAL, NB_RTPS_HISTORY_KEEP_LAST, 2, 2);
    run_late_joiner_case(
        "vol", NB_RTPS_DURABILITY_VOLATILE, NB_RTPS_HISTORY_KEEP_LAST, 2, 0);
    /* KEEP_ALL retains more than the configured depth of two. */
    run_late_joiner_case(
        "ka", NB_RTPS_DURABILITY_TRANSIENT_LOCAL, NB_RTPS_HISTORY_KEEP_ALL, 5, 5);
}

/*
 * Reader-side GAP handling: once the writer declares sequence numbers as
 * permanently unavailable, samples that reuse those numbers must be dropped
 * instead of delivered late.
 */
static void test_reliable_gap(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *publisher;
    nb_rtps_participant_t *subscriber;
    nb_rtps_endpoint_info_t publication;
    nb_rtps_endpoint_info_t subscription;
    nb_rtps_participant_info_t publisher_info;
    nb_rtps_dds_message_t message;
    test_state_t publisher_state;
    test_state_t subscriber_state;
    reliable_data_state_t data_state;
    static const uint8_t expected[] = {1, 2, 3, 4};
    uint16_t publisher_port;
    uint16_t subscriber_port;
    uint16_t data_port;
    int i;

    memset(&publisher_state, 0, sizeof(publisher_state));
    memset(&subscriber_state, 0, sizeof(subscriber_state));
    memset(&data_state, 0, sizeof(data_state));

    fill_config(&config, "gap-publisher");
    config.data_heartbeat_ms = 100;
    publisher = nb_rtps_participant_create(&config);
    CHECK(publisher != NULL);
    if (!publisher) {
        return;
    }
    publisher_port = nb_rtps_participant_spdp_port(publisher);
    fill_config(&config, "gap-subscriber");
    config.data_heartbeat_ms = 100;
    subscriber = nb_rtps_participant_create(&config);
    CHECK(subscriber != NULL);
    if (!subscriber) {
        nb_rtps_participant_destroy(publisher);
        return;
    }
    subscriber_port = nb_rtps_participant_spdp_port(subscriber);
    CHECK(nb_rtps_participant_add_peer(publisher, "127.0.0.1", subscriber_port) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(subscriber, "127.0.0.1", publisher_port) == NB_OK);

    publisher_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    subscriber_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    data_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(publisher_state.lock != NULL);
    CHECK(subscriber_state.lock != NULL);
    CHECK(data_state.lock != NULL);
    if (!publisher_state.lock || !subscriber_state.lock || !data_state.lock) {
        nb_rtps_participant_destroy(publisher);
        nb_rtps_participant_destroy(subscriber);
        if (publisher_state.lock) {
            ntk_mutex_destroy(publisher_state.lock);
        }
        if (subscriber_state.lock) {
            ntk_mutex_destroy(subscriber_state.lock);
        }
        if (data_state.lock) {
            ntk_mutex_destroy(data_state.lock);
        }
        return;
    }
    nb_rtps_participant_set_callback(publisher, on_participant, &publisher_state);
    nb_rtps_participant_set_callback(subscriber, on_participant, &subscriber_state);
    nb_rtps_participant_set_endpoint_callback(publisher, on_endpoint, &publisher_state);
    nb_rtps_participant_set_endpoint_callback(subscriber, on_endpoint, &subscriber_state);
    nb_rtps_participant_set_data_callback(subscriber, on_reliable_data, &data_state);
    CHECK(nb_rtps_participant_start(publisher) == NB_OK);
    CHECK(nb_rtps_participant_start(subscriber) == NB_OK);
    CHECK(wait_count(&publisher_state, 0, 1, 5000));
    CHECK(wait_count(&subscriber_state, 0, 1, 5000));

    memset(&publication, 0, sizeof(publication));
    (void)snprintf(publication.topic_name,
                   sizeof(publication.topic_name),
                   "%s",
                   NB_RTPS_DDS_MESSAGE_TOPIC);
    (void)snprintf(publication.type_name,
                   sizeof(publication.type_name),
                   "%s",
                   NB_RTPS_DDS_MESSAGE_TYPE);
    publication.reliability = NB_RTPS_RELIABILITY_RELIABLE;
    publication.durability = NB_RTPS_DURABILITY_VOLATILE;
    publication.history_kind = NB_RTPS_HISTORY_KEEP_LAST;
    publication.history_depth = 8;
    CHECK(nb_rtps_participant_add_endpoint(
              publisher, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);

    memset(&subscription, 0, sizeof(subscription));
    (void)snprintf(subscription.topic_name,
                   sizeof(subscription.topic_name),
                   "%s",
                   NB_RTPS_DDS_MESSAGE_TOPIC);
    (void)snprintf(subscription.type_name,
                   sizeof(subscription.type_name),
                   "%s",
                   NB_RTPS_DDS_MESSAGE_TYPE);
    subscription.reliability = NB_RTPS_RELIABILITY_RELIABLE;
    subscription.durability = NB_RTPS_DURABILITY_VOLATILE;
    subscription.history_kind = NB_RTPS_HISTORY_KEEP_LAST;
    subscription.history_depth = 8;
    CHECK(nb_rtps_participant_add_endpoint(
              subscriber, NB_RTPS_ENDPOINT_SUBSCRIPTION, &subscription) == NB_OK);
    CHECK(wait_endpoint_count(&subscriber_state, 0, 1, 5000));
    CHECK(wait_endpoint_count(&publisher_state, 0, 1, 5000));

    message.payload = &expected[0];
    message.payload_len = 1;
    CHECK(nb_rtps_participant_publish_dds_message(
              publisher, &publication.endpoint_guid, &message) == NB_OK);
    CHECK(wait_reliable_data(&data_state, 1, 5000));

    CHECK(subscription.unicast_count > 0);
    data_port = (uint16_t)subscription.unicast[0].port;
    CHECK(data_port != 0);
    CHECK(nb_rtps_participant_local_info(publisher, &publisher_info) == NB_OK);

    /* Announce that sequence numbers 2 and 3 will never be delivered. */
    CHECK(send_gap_datagram(&publisher_info.guid.prefix,
                            &publication.endpoint_guid,
                            &subscription.endpoint_guid,
                            2,
                            3,
                            1,
                            0x80000000u,
                            data_port) == NB_OK);
    ntk_sleep_ms(200);

    /* The next two samples reuse the gapped sequence numbers and must be
     * dropped; the sample after that is delivered normally. */
    for (i = 1; i < 4; i++) {
        message.payload = &expected[i];
        message.payload_len = 1;
        CHECK(nb_rtps_participant_publish_dds_message(
                  publisher, &publication.endpoint_guid, &message) == NB_OK);
        ntk_sleep_ms(50);
    }
    CHECK(wait_reliable_data(&data_state, 2, 5000));
    ntk_sleep_ms(400);
    CHECK(reliable_data_count(&data_state) == 2);
    CHECK(data_state.payloads[0][0] == expected[0]);
    CHECK(data_state.payloads[1][0] == expected[3]);
    CHECK(data_state.payload_lens[0] == 1);
    CHECK(data_state.payload_lens[1] == 1);

    nb_rtps_participant_stop(publisher);
    nb_rtps_participant_stop(subscriber);
    nb_rtps_participant_destroy(publisher);
    nb_rtps_participant_destroy(subscriber);
    ntk_mutex_destroy(publisher_state.lock);
    ntk_mutex_destroy(subscriber_state.lock);
    ntk_mutex_destroy(data_state.lock);
}

/*
 * The reader window table is capped by max_remote_endpoints. With more active
 * (writer, reader) pairs than that, the extra pairs run without a window:
 * delivery must still work, memory stays bounded, and the degraded pairs must
 * not trigger a retransmission storm for the pairs that do have a window.
 */
static void test_reliable_reader_capacity(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *publisher;
    nb_rtps_participant_t *subscriber;
    nb_rtps_endpoint_info_t publications[2];
    nb_rtps_endpoint_info_t subscriptions[3];
    nb_rtps_dds_message_t message;
    test_state_t publisher_state;
    test_state_t subscriber_state;
    reliable_data_state_t data_state;
    static const uint8_t payloads[2] = {0xa1, 0xb2};
    uint16_t publisher_port;
    uint16_t subscriber_port;
    int round;
    size_t i;

    memset(&publisher_state, 0, sizeof(publisher_state));
    memset(&subscriber_state, 0, sizeof(subscriber_state));
    memset(&data_state, 0, sizeof(data_state));

    fill_config(&config, "evict-publisher");
    config.data_heartbeat_ms = 100;
    config.max_remote_endpoints = 4;
    publisher = nb_rtps_participant_create(&config);
    CHECK(publisher != NULL);
    if (!publisher) {
        return;
    }
    publisher_port = nb_rtps_participant_spdp_port(publisher);
    fill_config(&config, "evict-subscriber");
    config.data_heartbeat_ms = 100;
    config.max_remote_endpoints = 4;
    subscriber = nb_rtps_participant_create(&config);
    CHECK(subscriber != NULL);
    if (!subscriber) {
        nb_rtps_participant_destroy(publisher);
        return;
    }
    subscriber_port = nb_rtps_participant_spdp_port(subscriber);
    CHECK(nb_rtps_participant_add_peer(publisher, "127.0.0.1", subscriber_port) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(subscriber, "127.0.0.1", publisher_port) == NB_OK);

    publisher_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    subscriber_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    data_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(publisher_state.lock != NULL);
    CHECK(subscriber_state.lock != NULL);
    CHECK(data_state.lock != NULL);
    if (!publisher_state.lock || !subscriber_state.lock || !data_state.lock) {
        nb_rtps_participant_destroy(publisher);
        nb_rtps_participant_destroy(subscriber);
        if (publisher_state.lock) {
            ntk_mutex_destroy(publisher_state.lock);
        }
        if (subscriber_state.lock) {
            ntk_mutex_destroy(subscriber_state.lock);
        }
        if (data_state.lock) {
            ntk_mutex_destroy(data_state.lock);
        }
        return;
    }
    nb_rtps_participant_set_callback(publisher, on_participant, &publisher_state);
    nb_rtps_participant_set_callback(subscriber, on_participant, &subscriber_state);
    nb_rtps_participant_set_endpoint_callback(publisher, on_endpoint, &publisher_state);
    nb_rtps_participant_set_endpoint_callback(subscriber, on_endpoint, &subscriber_state);
    nb_rtps_participant_set_data_callback(subscriber, on_reliable_data, &data_state);
    CHECK(nb_rtps_participant_start(publisher) == NB_OK);
    CHECK(nb_rtps_participant_start(subscriber) == NB_OK);
    CHECK(wait_count(&publisher_state, 0, 1, 5000));
    CHECK(wait_count(&subscriber_state, 0, 1, 5000));

    /* Two writers and three readers make six pairs against four slots. */
    for (i = 0; i < sizeof(publications) / sizeof(publications[0]); i++) {
        init_data_endpoint(&publications[i], 1);
        CHECK(nb_rtps_participant_add_endpoint(
                  publisher, NB_RTPS_ENDPOINT_PUBLICATION, &publications[i]) == NB_OK);
    }
    for (i = 0; i < sizeof(subscriptions) / sizeof(subscriptions[0]); i++) {
        init_data_endpoint(&subscriptions[i], 1);
        CHECK(nb_rtps_participant_add_endpoint(
                  subscriber, NB_RTPS_ENDPOINT_SUBSCRIPTION, &subscriptions[i]) == NB_OK);
    }
    CHECK(wait_endpoint_count(&subscriber_state, 0, 2, 5000));
    CHECK(wait_endpoint_count(&publisher_state, 0, 3, 5000));

    /* Every reader must still receive every writer at least once. */
    for (round = 0; round < 4; round++) {
        for (i = 0; i < sizeof(publications) / sizeof(publications[0]); i++) {
            message.payload = &payloads[i];
            message.payload_len = 1;
            CHECK(nb_rtps_participant_publish_dds_message(
                      publisher, &publications[i].endpoint_guid, &message) == NB_OK);
        }
        ntk_sleep_ms(50);
    }
    CHECK(wait_reliable_data(&data_state, 6, 5000));
    ntk_sleep_ms(300);
    /* A pair without a window must not turn into a retransmission storm. */
    CHECK(reliable_data_count(&data_state) <= 64);

    nb_rtps_participant_stop(publisher);
    nb_rtps_participant_stop(subscriber);
    nb_rtps_participant_destroy(publisher);
    nb_rtps_participant_destroy(subscriber);
    ntk_mutex_destroy(publisher_state.lock);
    ntk_mutex_destroy(subscriber_state.lock);
    ntk_mutex_destroy(data_state.lock);
}

/*
 * EXCLUSIVE ownership: a subscription takes data from the writer with the
 * highest ownership strength only, and moves to the next strongest writer when
 * the owner is removed instead of going silent.
 */
static void test_exclusive_ownership(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *strong = NULL;
    nb_rtps_participant_t *weak = NULL;
    nb_rtps_participant_t *reader = NULL;
    nb_rtps_endpoint_info_t strong_publication;
    nb_rtps_endpoint_info_t weak_publication;
    nb_rtps_endpoint_info_t subscription;
    test_state_t strong_state;
    test_state_t weak_state;
    test_state_t reader_state;
    reliable_data_state_t data_state;
    nb_rtps_dds_message_t message;
    uint8_t strong_byte = 0xa5;
    uint8_t weak_byte = 0x5a;
    uint16_t strong_port;
    uint16_t weak_port;
    uint16_t reader_port;
    int count_before;
    int waited;
    int round;
    int all_strong = 1;
    size_t i;

    memset(&strong_state, 0, sizeof(strong_state));
    memset(&weak_state, 0, sizeof(weak_state));
    memset(&reader_state, 0, sizeof(reader_state));
    memset(&data_state, 0, sizeof(data_state));

    fill_config(&config, "ownership-strong");
    strong = nb_rtps_participant_create(&config);
    fill_config(&config, "ownership-weak");
    weak = nb_rtps_participant_create(&config);
    fill_config(&config, "ownership-reader");
    reader = nb_rtps_participant_create(&config);
    CHECK(strong != NULL && weak != NULL && reader != NULL);
    if (!strong || !weak || !reader) {
        goto cleanup;
    }
    strong_port = nb_rtps_participant_spdp_port(strong);
    weak_port = nb_rtps_participant_spdp_port(weak);
    reader_port = nb_rtps_participant_spdp_port(reader);
    CHECK(nb_rtps_participant_add_peer(strong, "127.0.0.1", reader_port) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(weak, "127.0.0.1", reader_port) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(reader, "127.0.0.1", strong_port) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(reader, "127.0.0.1", weak_port) == NB_OK);

    strong_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    weak_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    reader_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    data_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(strong_state.lock && weak_state.lock && reader_state.lock && data_state.lock);
    if (!strong_state.lock || !weak_state.lock || !reader_state.lock || !data_state.lock) {
        goto cleanup;
    }
    nb_rtps_participant_set_endpoint_callback(strong, on_endpoint, &strong_state);
    nb_rtps_participant_set_endpoint_callback(weak, on_endpoint, &weak_state);
    nb_rtps_participant_set_endpoint_callback(reader, on_endpoint, &reader_state);
    nb_rtps_participant_set_data_callback(reader, on_reliable_data, &data_state);

    /* Both writers offer EXCLUSIVE ownership with different strengths; the
     * subscription requests the same policy. Matching requires equal ownership,
     * so a mismatch here would show up as a missing endpoint below. */
    init_data_endpoint(&strong_publication, 0);
    strong_publication.ownership = NB_RTPS_OWNERSHIP_EXCLUSIVE;
    strong_publication.ownership_strength = 10;
    init_data_endpoint(&weak_publication, 0);
    weak_publication.ownership = NB_RTPS_OWNERSHIP_EXCLUSIVE;
    weak_publication.ownership_strength = 1;
    init_data_endpoint(&subscription, 0);
    subscription.ownership = NB_RTPS_OWNERSHIP_EXCLUSIVE;

    CHECK(nb_rtps_participant_add_endpoint(
              strong, NB_RTPS_ENDPOINT_PUBLICATION, &strong_publication) == NB_OK);
    CHECK(nb_rtps_participant_add_endpoint(
              weak, NB_RTPS_ENDPOINT_PUBLICATION, &weak_publication) == NB_OK);
    CHECK(nb_rtps_participant_add_endpoint(
              reader, NB_RTPS_ENDPOINT_SUBSCRIPTION, &subscription) == NB_OK);

    CHECK(nb_rtps_participant_start(strong) == NB_OK);
    CHECK(nb_rtps_participant_start(weak) == NB_OK);
    CHECK(nb_rtps_participant_start(reader) == NB_OK);
    CHECK(wait_endpoint_count(&strong_state, 0, 1, 5000));
    CHECK(wait_endpoint_count(&weak_state, 0, 1, 5000));
    CHECK(wait_endpoint_count(&reader_state, 0, 2, 5000));

    /* Both writers publish; only the strong writer's samples may arrive. */
    for (round = 0; round < 5; round++) {
        message.payload = &strong_byte;
        message.payload_len = 1;
        (void)nb_rtps_participant_publish_dds_message(
            strong, &strong_publication.endpoint_guid, &message);
        message.payload = &weak_byte;
        (void)nb_rtps_participant_publish_dds_message(
            weak, &weak_publication.endpoint_guid, &message);
        ntk_sleep_ms(20);
    }
    CHECK(wait_reliable_data(&data_state, 1, 3000));
    ntk_sleep_ms(200);
    ntk_mutex_lock(data_state.lock);
    for (i = 0; i < (size_t)data_state.count &&
                i < sizeof(data_state.writer_guids) / sizeof(data_state.writer_guids[0]);
         i++) {
        if (!nb_rtps_guid_equal(&data_state.writer_guids[i],
                                &strong_publication.endpoint_guid)) {
            all_strong = 0;
        }
    }
    ntk_mutex_unlock(data_state.lock);
    CHECK(all_strong);

    /* Withdraw the owner: ownership must move to the only remaining writer. */
    count_before = reliable_data_count(&data_state);
    CHECK(nb_rtps_participant_remove_endpoint(strong, &strong_publication.endpoint_guid) ==
          NB_OK);
    for (waited = 0; waited < 5000; waited += 20) {
        message.payload = &weak_byte;
        message.payload_len = 1;
        (void)nb_rtps_participant_publish_dds_message(
            weak, &weak_publication.endpoint_guid, &message);
        if (reliable_data_count(&data_state) > count_before) {
            break;
        }
        ntk_sleep_ms(20);
    }
    CHECK(reliable_data_count(&data_state) > count_before);
    ntk_mutex_lock(data_state.lock);
    CHECK(nb_rtps_guid_equal(&data_state.writer_guids[data_state.count - 1],
                             &weak_publication.endpoint_guid));
    ntk_mutex_unlock(data_state.lock);

cleanup:
    if (strong) {
        nb_rtps_participant_stop(strong);
        nb_rtps_participant_destroy(strong);
    }
    if (weak) {
        nb_rtps_participant_stop(weak);
        nb_rtps_participant_destroy(weak);
    }
    if (reader) {
        nb_rtps_participant_stop(reader);
        nb_rtps_participant_destroy(reader);
    }
    if (strong_state.lock) {
        ntk_mutex_destroy(strong_state.lock);
    }
    if (weak_state.lock) {
        ntk_mutex_destroy(weak_state.lock);
    }
    if (reader_state.lock) {
        ntk_mutex_destroy(reader_state.lock);
    }
    if (data_state.lock) {
        ntk_mutex_destroy(data_state.lock);
    }
}

/*
 * BY_SOURCE_TIMESTAMP: a reader discards samples whose source timestamp is not
 * newer than the last sample it delivered, while a BY_RECEPTION_TIMESTAMP reader
 * on the same topic takes everything. The same four samples go to both, in
 * arrival order 3, 1, 2, 4.
 */
static void test_destination_order_by_source(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *publisher = NULL;
    nb_rtps_participant_t *subscriber = NULL;
    nb_rtps_endpoint_info_t publication;
    nb_rtps_endpoint_info_t ordered;
    nb_rtps_endpoint_info_t unordered;
    test_state_t publisher_state;
    test_state_t subscriber_state;
    reliable_data_state_t data_state;
    nb_rtps_dds_message_t message;
    uint8_t serialized[16];
    size_t serialized_len = 0;
    uint16_t publisher_port;
    uint16_t subscriber_port;
    uint8_t payloads[4] = {0x03, 0x01, 0x02, 0x04};
    int32_t stamps[4] = {300, 100, 200, 400};
    int ordered_count = 0;
    int unordered_count = 0;
    int saw_3 = 0;
    int saw_4 = 0;
    int i;

    memset(&publisher_state, 0, sizeof(publisher_state));
    memset(&subscriber_state, 0, sizeof(subscriber_state));
    memset(&data_state, 0, sizeof(data_state));

    fill_config(&config, "order-publisher");
    publisher = nb_rtps_participant_create(&config);
    fill_config(&config, "order-subscriber");
    subscriber = nb_rtps_participant_create(&config);
    CHECK(publisher != NULL && subscriber != NULL);
    if (!publisher || !subscriber) {
        goto cleanup;
    }
    publisher_port = nb_rtps_participant_spdp_port(publisher);
    subscriber_port = nb_rtps_participant_spdp_port(subscriber);
    CHECK(nb_rtps_participant_add_peer(publisher, "127.0.0.1", subscriber_port) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(subscriber, "127.0.0.1", publisher_port) == NB_OK);

    publisher_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    subscriber_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    data_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(publisher_state.lock && subscriber_state.lock && data_state.lock);
    if (!publisher_state.lock || !subscriber_state.lock || !data_state.lock) {
        goto cleanup;
    }
    nb_rtps_participant_set_endpoint_callback(publisher, on_endpoint, &publisher_state);
    nb_rtps_participant_set_endpoint_callback(subscriber, on_endpoint, &subscriber_state);
    nb_rtps_participant_set_data_callback(subscriber, on_reliable_data, &data_state);

    init_data_endpoint(&publication, 0);
    publication.destination_order = NB_RTPS_DESTINATION_ORDER_BY_SOURCE;
    init_data_endpoint(&ordered, 0);
    ordered.destination_order = NB_RTPS_DESTINATION_ORDER_BY_SOURCE;
    init_data_endpoint(&unordered, 0);

    CHECK(nb_rtps_participant_add_endpoint(
              publisher, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);
    CHECK(nb_rtps_participant_add_endpoint(
              subscriber, NB_RTPS_ENDPOINT_SUBSCRIPTION, &ordered) == NB_OK);
    CHECK(nb_rtps_participant_add_endpoint(
              subscriber, NB_RTPS_ENDPOINT_SUBSCRIPTION, &unordered) == NB_OK);

    CHECK(nb_rtps_participant_start(publisher) == NB_OK);
    CHECK(nb_rtps_participant_start(subscriber) == NB_OK);
    CHECK(wait_endpoint_count(&publisher_state, 0, 2, 5000));
    CHECK(wait_endpoint_count(&subscriber_state, 0, 1, 5000));

    for (i = 0; i < 4; i++) {
        nb_rtps_time_t ts;

        ts.seconds = stamps[i];
        ts.fraction = 0;
        message.payload = &payloads[i];
        message.payload_len = 1;
        serialized_len = 0;
        CHECK(nb_rtps_dds_message_serialize(
                  &message, serialized, sizeof(serialized), &serialized_len) == NB_OK);
        CHECK(nb_rtps_participant_publish_at(publisher,
                                             &publication.endpoint_guid,
                                             serialized,
                                             serialized_len,
                                             &ts) == NB_OK);
        ntk_sleep_ms(30);
    }

    /* Four samples reach the unordered reader, two the ordered one. */
    CHECK(wait_reliable_data(&data_state, 6, 5000));
    ntk_sleep_ms(200);

    ntk_mutex_lock(data_state.lock);
    for (i = 0; i < data_state.count &&
                i < (int)(sizeof(data_state.payloads) / sizeof(data_state.payloads[0]));
         i++) {
        if (nb_rtps_guid_equal(&data_state.reader_guids[i], &ordered.endpoint_guid)) {
            ordered_count++;
            if (data_state.payload_lens[i] == 1 && data_state.payloads[i][0] == 0x03) {
                saw_3 = 1;
            }
            if (data_state.payload_lens[i] == 1 && data_state.payloads[i][0] == 0x04) {
                saw_4 = 1;
            }
        } else if (nb_rtps_guid_equal(&data_state.reader_guids[i],
                                      &unordered.endpoint_guid)) {
            unordered_count++;
        }
    }
    ntk_mutex_unlock(data_state.lock);

    CHECK(ordered_count == 2);
    CHECK(unordered_count == 4);
    CHECK(saw_3);
    CHECK(saw_4);
    /* The two out-of-order samples never reach the ordered reader. */
    CHECK(!(ordered_count == 4));

cleanup:
    if (publisher) {
        nb_rtps_participant_stop(publisher);
        nb_rtps_participant_destroy(publisher);
    }
    if (subscriber) {
        nb_rtps_participant_stop(subscriber);
        nb_rtps_participant_destroy(subscriber);
    }
    if (publisher_state.lock) {
        ntk_mutex_destroy(publisher_state.lock);
    }
    if (subscriber_state.lock) {
        ntk_mutex_destroy(subscriber_state.lock);
    }
    if (data_state.lock) {
        ntk_mutex_destroy(data_state.lock);
    }
}

/*
 * TIME_BASED_FILTER: a subscription that requests a minimum separation drops
 * samples stamped closer than that to the last one it delivered, while a
 * subscription on the same topic without the policy still sees all of them.
 * The four samples carry source timestamps 0, 50, 120 and 230 ms, so a 100 ms
 * separation keeps 0, 120 and 230 and drops 50.
 */
static void test_time_based_filter(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *publisher = NULL;
    nb_rtps_participant_t *subscriber = NULL;
    nb_rtps_endpoint_info_t publication;
    nb_rtps_endpoint_info_t filtered;
    nb_rtps_endpoint_info_t plain;
    test_state_t publisher_state;
    test_state_t subscriber_state;
    reliable_data_state_t data_state;
    nb_rtps_dds_message_t message;
    uint8_t serialized[16];
    size_t serialized_len = 0;
    uint16_t publisher_port;
    uint16_t subscriber_port;
    uint8_t payloads[4] = {0x01, 0x02, 0x03, 0x04};
    /* A non-zero base keeps every sample on the wire with an INFO_TS; a zero
     * timestamp means "not set" and is never filtered. */
    int32_t seconds[4] = {1, 1, 1, 1};
    uint32_t fractions[4] = {0u, 0x0ccccccdu, 0x1eb851ebu, 0x3ae147aeu};
    int filtered_count = 0;
    int plain_count = 0;
    int filtered_saw_2 = 0;
    int plain_saw_2 = 0;
    int64_t filtered_ts[4];
    int filtered_ts_count = 0;
    int i;

    memset(&publisher_state, 0, sizeof(publisher_state));
    memset(&subscriber_state, 0, sizeof(subscriber_state));
    memset(&data_state, 0, sizeof(data_state));

    fill_config(&config, "filter-publisher");
    publisher = nb_rtps_participant_create(&config);
    fill_config(&config, "filter-subscriber");
    subscriber = nb_rtps_participant_create(&config);
    CHECK(publisher != NULL && subscriber != NULL);
    if (!publisher || !subscriber) {
        goto cleanup;
    }
    publisher_port = nb_rtps_participant_spdp_port(publisher);
    subscriber_port = nb_rtps_participant_spdp_port(subscriber);
    CHECK(nb_rtps_participant_add_peer(publisher, "127.0.0.1", subscriber_port) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(subscriber, "127.0.0.1", publisher_port) == NB_OK);

    publisher_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    subscriber_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    data_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(publisher_state.lock && subscriber_state.lock && data_state.lock);
    if (!publisher_state.lock || !subscriber_state.lock || !data_state.lock) {
        goto cleanup;
    }
    nb_rtps_participant_set_endpoint_callback(publisher, on_endpoint, &publisher_state);
    nb_rtps_participant_set_endpoint_callback(subscriber, on_endpoint, &subscriber_state);
    nb_rtps_participant_set_data_callback(subscriber, on_reliable_data, &data_state);

    init_data_endpoint(&publication, 0);
    init_data_endpoint(&filtered, 0);
    filtered.min_separation_ms = 100;
    init_data_endpoint(&plain, 0);

    CHECK(nb_rtps_participant_add_endpoint(
              publisher, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);
    CHECK(nb_rtps_participant_add_endpoint(
              subscriber, NB_RTPS_ENDPOINT_SUBSCRIPTION, &filtered) == NB_OK);
    CHECK(nb_rtps_participant_add_endpoint(
              subscriber, NB_RTPS_ENDPOINT_SUBSCRIPTION, &plain) == NB_OK);

    CHECK(nb_rtps_participant_start(publisher) == NB_OK);
    CHECK(nb_rtps_participant_start(subscriber) == NB_OK);
    CHECK(wait_endpoint_count(&publisher_state, 0, 2, 5000));
    CHECK(wait_endpoint_count(&subscriber_state, 0, 1, 5000));

    for (i = 0; i < 4; i++) {
        nb_rtps_time_t ts;

        ts.seconds = seconds[i];
        ts.fraction = fractions[i];
        message.payload = &payloads[i];
        message.payload_len = 1;
        serialized_len = 0;
        CHECK(nb_rtps_dds_message_serialize(
                  &message, serialized, sizeof(serialized), &serialized_len) == NB_OK);
        CHECK(nb_rtps_participant_publish_at(publisher,
                                             &publication.endpoint_guid,
                                             serialized,
                                             serialized_len,
                                             &ts) == NB_OK);
        ntk_sleep_ms(30);
    }

    /* Four samples reach the plain reader, three the filtered one. */
    CHECK(wait_reliable_data(&data_state, 7, 5000));
    ntk_sleep_ms(200);

    ntk_mutex_lock(data_state.lock);
    for (i = 0; i < data_state.count &&
                i < (int)(sizeof(data_state.payloads) / sizeof(data_state.payloads[0]));
         i++) {
        int is_filtered =
            nb_rtps_guid_equal(&data_state.reader_guids[i], &filtered.endpoint_guid);
        int is_plain = nb_rtps_guid_equal(&data_state.reader_guids[i], &plain.endpoint_guid);

        if (!is_filtered && !is_plain) {
            continue;
        }
        if (is_filtered) {
            filtered_count++;
            if (filtered_ts_count < 4) {
                filtered_ts[filtered_ts_count++] = data_state.source_ts[i];
            }
            if (data_state.payload_lens[i] == 1 && data_state.payloads[i][0] == 0x02) {
                filtered_saw_2 = 1;
            }
        } else {
            plain_count++;
            if (data_state.payload_lens[i] == 1 && data_state.payloads[i][0] == 0x02) {
                plain_saw_2 = 1;
            }
        }
    }
    ntk_mutex_unlock(data_state.lock);

    CHECK(plain_count == 4);
    CHECK(plain_saw_2);
    CHECK(filtered_count == 3);
    /* The sample 50 ms after the previous accepted one is the filtered one. */
    CHECK(!filtered_saw_2);
    /* The surviving samples keep the writer's source timestamps. */
    CHECK(filtered_ts_count == 3);
    CHECK(filtered_ts[0] == 1000000000ll);
    /* The wire carries seconds plus a 2^-32 fraction, so the round trip lands
     * within a nanosecond of 1.12 s and 1.23 s; compare at millisecond
     * granularity, which is what the policy is specified in. */
    CHECK(filtered_ts[1] / 1000000ll == 1119ll);
    CHECK(filtered_ts[2] / 1000000ll == 1229ll);

cleanup:
    if (publisher) {
        nb_rtps_participant_stop(publisher);
        nb_rtps_participant_destroy(publisher);
    }
    if (subscriber) {
        nb_rtps_participant_stop(subscriber);
        nb_rtps_participant_destroy(subscriber);
    }
    if (publisher_state.lock) {
        ntk_mutex_destroy(publisher_state.lock);
    }
    if (subscriber_state.lock) {
        ntk_mutex_destroy(subscriber_state.lock);
    }
    if (data_state.lock) {
        ntk_mutex_destroy(data_state.lock);
    }
}

/*
 * A dispose carries no application payload but is delivered as a sample that
 * is not alive, carrying the instance handle from its inline QoS. Its sequence
 * number is also consumed: otherwise a reliable writer keeps retransmitting a
 * sample this reader never acknowledges.
 */
static void test_dispose_is_consumed(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *publisher = NULL;
    nb_rtps_participant_t *subscriber = NULL;
    nb_rtps_endpoint_info_t publication;
    nb_rtps_endpoint_info_t subscription;
    nb_rtps_participant_info_t publisher_info;
    test_state_t publisher_state;
    test_state_t subscriber_state;
    reliable_data_state_t data_state;
    nb_rtps_dds_message_t message;
    uint8_t byte = 0x11;
    uint16_t publisher_port;
    uint16_t subscriber_port;
    uint16_t data_port;

    memset(&publisher_state, 0, sizeof(publisher_state));
    memset(&subscriber_state, 0, sizeof(subscriber_state));
    memset(&data_state, 0, sizeof(data_state));

    fill_config(&config, "dispose-publisher");
    publisher = nb_rtps_participant_create(&config);
    fill_config(&config, "dispose-subscriber");
    subscriber = nb_rtps_participant_create(&config);
    CHECK(publisher != NULL && subscriber != NULL);
    if (!publisher || !subscriber) {
        goto cleanup;
    }
    publisher_port = nb_rtps_participant_spdp_port(publisher);
    subscriber_port = nb_rtps_participant_spdp_port(subscriber);
    CHECK(nb_rtps_participant_add_peer(publisher, "127.0.0.1", subscriber_port) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(subscriber, "127.0.0.1", publisher_port) == NB_OK);

    publisher_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    subscriber_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    data_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(publisher_state.lock && subscriber_state.lock && data_state.lock);
    if (!publisher_state.lock || !subscriber_state.lock || !data_state.lock) {
        goto cleanup;
    }
    nb_rtps_participant_set_endpoint_callback(publisher, on_endpoint, &publisher_state);
    nb_rtps_participant_set_endpoint_callback(subscriber, on_endpoint, &subscriber_state);
    nb_rtps_participant_set_data_callback(subscriber, on_reliable_data, &data_state);

    /* Reliable on both sides: only a reliable writer retransmits unacknowledged
     * samples, which is the defect this guards against. */
    init_data_endpoint(&publication, 1);
    init_data_endpoint(&subscription, 1);
    CHECK(nb_rtps_participant_add_endpoint(
              publisher, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);
    CHECK(nb_rtps_participant_add_endpoint(
              subscriber, NB_RTPS_ENDPOINT_SUBSCRIPTION, &subscription) == NB_OK);

    CHECK(nb_rtps_participant_start(publisher) == NB_OK);
    CHECK(nb_rtps_participant_start(subscriber) == NB_OK);
    CHECK(wait_endpoint_count(&publisher_state, 0, 1, 5000));
    CHECK(wait_endpoint_count(&subscriber_state, 0, 1, 5000));
    CHECK(subscription.unicast_count > 0);
    data_port = (uint16_t)subscription.unicast[0].port;
    CHECK(data_port != 0);
    CHECK(nb_rtps_participant_local_info(publisher, &publisher_info) == NB_OK);

    /* Sample 1 is real data and must still be delivered. */
    message.payload = &byte;
    message.payload_len = 1;
    CHECK(nb_rtps_participant_publish_dds_message(
              publisher, &publication.endpoint_guid, &message) == NB_OK);
    CHECK(wait_reliable_data(&data_state, 1, 5000));

    /* Sample 2 arrives as a dispose: delivered as not-alive and consumed. */
    CHECK(send_dispose_datagram(&publisher_info.guid.prefix,
                                &publication.endpoint_guid,
                                &subscription.endpoint_guid,
                                2,
                                data_port) == NB_OK);
    for (int waited = 0;
         waited < 2000 &&
         !nb_rtps_participant_reader_consumed(subscriber,
                                              &publication.endpoint_guid,
                                              &subscription.endpoint_guid,
                                              2);
         waited += 20) {
        ntk_sleep_ms(20);
    }
    CHECK(nb_rtps_participant_reader_consumed(subscriber,
                                              &publication.endpoint_guid,
                                              &subscription.endpoint_guid,
                                              2));
    CHECK(reliable_data_count(&data_state) == 2);
    ntk_mutex_lock(data_state.lock);
    CHECK(data_state.states[0] == NB_SAMPLE_STATE_VALID);
    CHECK(data_state.payload_lens[0] == 1);
    CHECK(data_state.states[1] == NB_SAMPLE_STATE_DISPOSED);
    CHECK(data_state.payload_lens[1] == 0);
    CHECK(data_state.has_handle[1] == 1);
    CHECK(memcmp(data_state.handles[1], g_dispose_handle, sizeof(g_dispose_handle)) == 0);
    ntk_mutex_unlock(data_state.lock);

cleanup:
    if (publisher) {
        nb_rtps_participant_stop(publisher);
        nb_rtps_participant_destroy(publisher);
    }
    if (subscriber) {
        nb_rtps_participant_stop(subscriber);
        nb_rtps_participant_destroy(subscriber);
    }
    if (publisher_state.lock) {
        ntk_mutex_destroy(publisher_state.lock);
    }
    if (subscriber_state.lock) {
        ntk_mutex_destroy(subscriber_state.lock);
    }
    if (data_state.lock) {
        ntk_mutex_destroy(data_state.lock);
    }
}

/*
 * Instance state as DDS reports it: a live sample registers the instance (NEW
 * view, generation rank 0), a dispose turns it not-alive, and the next live
 * sample re-registers it with a new generation rank and a NEW view again.
 */
static void test_instance_states(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *publisher = NULL;
    nb_rtps_participant_t *subscriber = NULL;
    nb_rtps_endpoint_info_t publication;
    nb_rtps_endpoint_info_t subscription;
    nb_rtps_participant_info_t publisher_info;
    test_state_t publisher_state;
    test_state_t subscriber_state;
    reliable_data_state_t data_state;
    uint16_t publisher_port;
    uint16_t subscriber_port;
    uint16_t data_port;

    memset(&publisher_state, 0, sizeof(publisher_state));
    memset(&subscriber_state, 0, sizeof(subscriber_state));
    memset(&data_state, 0, sizeof(data_state));

    fill_config(&config, "instance-publisher");
    publisher = nb_rtps_participant_create(&config);
    fill_config(&config, "instance-subscriber");
    subscriber = nb_rtps_participant_create(&config);
    CHECK(publisher != NULL && subscriber != NULL);
    if (!publisher || !subscriber) {
        goto cleanup;
    }
    publisher_port = nb_rtps_participant_spdp_port(publisher);
    subscriber_port = nb_rtps_participant_spdp_port(subscriber);
    CHECK(nb_rtps_participant_add_peer(publisher, "127.0.0.1", subscriber_port) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(subscriber, "127.0.0.1", publisher_port) == NB_OK);

    publisher_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    subscriber_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    data_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(publisher_state.lock && subscriber_state.lock && data_state.lock);
    if (!publisher_state.lock || !subscriber_state.lock || !data_state.lock) {
        goto cleanup;
    }
    nb_rtps_participant_set_endpoint_callback(publisher, on_endpoint, &publisher_state);
    nb_rtps_participant_set_endpoint_callback(subscriber, on_endpoint, &subscriber_state);
    nb_rtps_participant_set_data_callback(subscriber, on_reliable_data, &data_state);

    init_data_endpoint(&publication, 1);
    init_data_endpoint(&subscription, 1);
    CHECK(nb_rtps_participant_add_endpoint(
              publisher, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);
    CHECK(nb_rtps_participant_add_endpoint(
              subscriber, NB_RTPS_ENDPOINT_SUBSCRIPTION, &subscription) == NB_OK);

    CHECK(nb_rtps_participant_start(publisher) == NB_OK);
    CHECK(nb_rtps_participant_start(subscriber) == NB_OK);
    CHECK(wait_endpoint_count(&publisher_state, 0, 1, 5000));
    CHECK(wait_endpoint_count(&subscriber_state, 0, 1, 5000));
    CHECK(subscription.unicast_count > 0);
    data_port = (uint16_t)subscription.unicast[0].port;
    CHECK(data_port != 0);
    CHECK(nb_rtps_participant_local_info(publisher, &publisher_info) == NB_OK);

    CHECK(send_instance_datagram(&publisher_info.guid.prefix,
                                 &publication.endpoint_guid,
                                 &subscription.endpoint_guid,
                                 1,
                                 g_dispose_handle,
                                 0,
                                 data_port) == NB_OK);
    CHECK(send_instance_datagram(&publisher_info.guid.prefix,
                                 &publication.endpoint_guid,
                                 &subscription.endpoint_guid,
                                 2,
                                 g_dispose_handle,
                                 1,
                                 data_port) == NB_OK);
    CHECK(send_instance_datagram(&publisher_info.guid.prefix,
                                 &publication.endpoint_guid,
                                 &subscription.endpoint_guid,
                                 3,
                                 g_dispose_handle,
                                 0,
                                 data_port) == NB_OK);
    CHECK(wait_reliable_data(&data_state, 3, 5000));

    ntk_mutex_lock(data_state.lock);
    CHECK(data_state.count == 3);
    /* First sample of the instance: alive, new view, rank 0. */
    CHECK(data_state.states[0] == NB_SAMPLE_STATE_VALID);
    CHECK(data_state.instance_states[0] == NB_INSTANCE_STATE_ALIVE);
    CHECK(data_state.view_states[0] == NB_VIEW_STATE_NEW);
    CHECK(data_state.generation_ranks[0] == 0);
    CHECK(data_state.has_handle[0] == 1);
    CHECK(data_state.reception_ts[0] > 0);
    /* The dispose leaves it not-alive and does not bump the rank. */
    CHECK(data_state.states[1] == NB_SAMPLE_STATE_DISPOSED);
    CHECK(data_state.instance_states[1] == NB_INSTANCE_STATE_NOT_ALIVE_DISPOSED);
    CHECK(data_state.view_states[1] == NB_VIEW_STATE_NOT_NEW);
    CHECK(data_state.generation_ranks[1] == 0);
    /* Writing again re-registers the instance with a new generation rank. */
    CHECK(data_state.states[2] == NB_SAMPLE_STATE_VALID);
    CHECK(data_state.instance_states[2] == NB_INSTANCE_STATE_ALIVE);
    CHECK(data_state.view_states[2] == NB_VIEW_STATE_NEW);
    CHECK(data_state.generation_ranks[2] == 1);
    ntk_mutex_unlock(data_state.lock);

cleanup:
    if (publisher) {
        nb_rtps_participant_stop(publisher);
        nb_rtps_participant_destroy(publisher);
    }
    if (subscriber) {
        nb_rtps_participant_stop(subscriber);
        nb_rtps_participant_destroy(subscriber);
    }
    if (publisher_state.lock) {
        ntk_mutex_destroy(publisher_state.lock);
    }
    if (subscriber_state.lock) {
        ntk_mutex_destroy(subscriber_state.lock);
    }
    if (data_state.lock) {
        ntk_mutex_destroy(data_state.lock);
    }
}

/*
 * Writer side of the keyed lifecycle: the writer publishes live samples with an
 * instance handle, disposes and unregisters instances, and disposes whatever is
 * still alive when it is deleted (autodispose). The subscriber must see the
 * matching instance states in the order the writer produced them.
 */
static void test_keyed_writer_lifecycle(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *publisher = NULL;
    nb_rtps_participant_t *subscriber = NULL;
    nb_rtps_endpoint_info_t publication;
    nb_rtps_endpoint_info_t subscription;
    test_state_t publisher_state;
    test_state_t subscriber_state;
    reliable_data_state_t data_state;
    uint16_t publisher_port;
    uint16_t subscriber_port;
    uint8_t live[9] = {0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x42};
    uint8_t key[8] = {0x00, 0x01, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00};
    uint8_t handle_b[NB_INSTANCE_HANDLE_SIZE];
    uint8_t handle_c[NB_INSTANCE_HANDLE_SIZE];
    size_t i;

    memset(&publisher_state, 0, sizeof(publisher_state));
    memset(&subscriber_state, 0, sizeof(subscriber_state));
    memset(&data_state, 0, sizeof(data_state));
    for (i = 0; i < NB_INSTANCE_HANDLE_SIZE; i++) {
        handle_b[i] = (uint8_t)(0xb0 + i);
        handle_c[i] = (uint8_t)(0xc0 + i);
    }

    fill_config(&config, "writer-publisher");
    publisher = nb_rtps_participant_create(&config);
    fill_config(&config, "writer-subscriber");
    subscriber = nb_rtps_participant_create(&config);
    CHECK(publisher != NULL && subscriber != NULL);
    if (!publisher || !subscriber) {
        goto cleanup;
    }
    publisher_port = nb_rtps_participant_spdp_port(publisher);
    subscriber_port = nb_rtps_participant_spdp_port(subscriber);
    CHECK(nb_rtps_participant_add_peer(publisher, "127.0.0.1", subscriber_port) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(subscriber, "127.0.0.1", publisher_port) == NB_OK);

    publisher_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    subscriber_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    data_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(publisher_state.lock && subscriber_state.lock && data_state.lock);
    if (!publisher_state.lock || !subscriber_state.lock || !data_state.lock) {
        goto cleanup;
    }
    nb_rtps_participant_set_endpoint_callback(publisher, on_endpoint, &publisher_state);
    nb_rtps_participant_set_endpoint_callback(subscriber, on_endpoint, &subscriber_state);
    nb_rtps_participant_set_data_callback(subscriber, on_reliable_data, &data_state);

    init_data_endpoint(&publication, 1);
    init_data_endpoint(&subscription, 1);
    publication.autodispose = 1;
    CHECK(nb_rtps_participant_add_endpoint(
              publisher, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);
    CHECK(nb_rtps_participant_add_endpoint(
              subscriber, NB_RTPS_ENDPOINT_SUBSCRIPTION, &subscription) == NB_OK);

    CHECK(nb_rtps_participant_start(publisher) == NB_OK);
    CHECK(nb_rtps_participant_start(subscriber) == NB_OK);
    CHECK(wait_endpoint_count(&publisher_state, 0, 1, 5000));
    CHECK(wait_endpoint_count(&subscriber_state, 0, 1, 5000));

    /* live A, dispose A, live B, unregister B, live C */
    CHECK(nb_rtps_participant_publish_instance(publisher,
                                               &publication.endpoint_guid,
                                               g_dispose_handle,
                                               0,
                                               live,
                                               sizeof(live)) == NB_OK);
    CHECK(nb_rtps_participant_publish_instance(publisher,
                                               &publication.endpoint_guid,
                                               g_dispose_handle,
                                               1,
                                               key,
                                               sizeof(key)) == NB_OK);
    CHECK(nb_rtps_participant_publish_instance(publisher,
                                               &publication.endpoint_guid,
                                               handle_b,
                                               0,
                                               live,
                                               sizeof(live)) == NB_OK);
    CHECK(nb_rtps_participant_publish_instance(publisher,
                                               &publication.endpoint_guid,
                                               handle_b,
                                               2,
                                               key,
                                               sizeof(key)) == NB_OK);
    CHECK(nb_rtps_participant_publish_instance(publisher,
                                               &publication.endpoint_guid,
                                               handle_c,
                                               0,
                                               live,
                                               sizeof(live)) == NB_OK);
    CHECK(wait_reliable_data(&data_state, 5, 5000));

    ntk_mutex_lock(data_state.lock);
    CHECK(data_state.count == 5);
    CHECK(data_state.states[0] == NB_SAMPLE_STATE_VALID);
    CHECK(data_state.instance_states[0] == NB_INSTANCE_STATE_ALIVE);
    CHECK(data_state.view_states[0] == NB_VIEW_STATE_NEW);
    CHECK(data_state.has_handle[0] == 1);
    CHECK(memcmp(data_state.handles[0], g_dispose_handle, NB_INSTANCE_HANDLE_SIZE) == 0);
    CHECK(data_state.states[1] == NB_SAMPLE_STATE_DISPOSED);
    CHECK(data_state.instance_states[1] == NB_INSTANCE_STATE_NOT_ALIVE_DISPOSED);
    CHECK(data_state.view_states[1] == NB_VIEW_STATE_NOT_NEW);
    CHECK(data_state.generation_ranks[1] == 0);
    CHECK(data_state.states[2] == NB_SAMPLE_STATE_VALID);
    CHECK(data_state.instance_states[2] == NB_INSTANCE_STATE_ALIVE);
    CHECK(data_state.view_states[2] == NB_VIEW_STATE_NEW);
    CHECK(data_state.generation_ranks[2] == 0);
    CHECK(data_state.states[3] == NB_SAMPLE_STATE_NO_WRITERS);
    CHECK(data_state.instance_states[3] == NB_INSTANCE_STATE_NOT_ALIVE_NO_WRITERS);
    CHECK(data_state.states[4] == NB_SAMPLE_STATE_VALID);
    CHECK(data_state.instance_states[4] == NB_INSTANCE_STATE_ALIVE);
    ntk_mutex_unlock(data_state.lock);

    /* Deleting the writer disposes the instance that is still alive. */
    CHECK(nb_rtps_participant_remove_endpoint(publisher, &publication.endpoint_guid) ==
          NB_OK);
    CHECK(wait_reliable_data(&data_state, 6, 5000));
    ntk_mutex_lock(data_state.lock);
    CHECK(data_state.count == 6);
    CHECK(data_state.states[5] == NB_SAMPLE_STATE_DISPOSED);
    CHECK(data_state.instance_states[5] == NB_INSTANCE_STATE_NOT_ALIVE_DISPOSED);
    CHECK(memcmp(data_state.handles[5], handle_c, NB_INSTANCE_HANDLE_SIZE) == 0);
    ntk_mutex_unlock(data_state.lock);

cleanup:
    if (publisher) {
        nb_rtps_participant_stop(publisher);
        nb_rtps_participant_destroy(publisher);
    }
    if (subscriber) {
        nb_rtps_participant_stop(subscriber);
        nb_rtps_participant_destroy(subscriber);
    }
    if (publisher_state.lock) {
        ntk_mutex_destroy(publisher_state.lock);
    }
    if (subscriber_state.lock) {
        ntk_mutex_destroy(subscriber_state.lock);
    }
    if (data_state.lock) {
        ntk_mutex_destroy(data_state.lock);
    }
}

/*
 * RTPS over TCP. The two participants discover each other over UDP (SPDP has
 * to work before a stream can be dialled), then everything else - SEDP, user
 * data, heartbeats and acknowledgements - moves to the stream. The test proves
 * that by watching the connection counters: the sample only arrives if the
 * publisher wrote it to the TCP link.
 */
static void test_tcp_transport(void)
{
    static const uint8_t expected[] = {0x54, 0x43, 0x50, 0x2d, 0x31};
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *publisher;
    nb_rtps_participant_t *subscriber;
    nb_rtps_endpoint_info_t publication;
    nb_rtps_endpoint_info_t subscription;
    nb_rtps_dds_message_t message;
    nb_rtps_tcp_stats_t before;
    nb_rtps_tcp_stats_t after;
    nb_rtps_tcp_stats_t subscriber_stats;
    test_state_t publisher_state;
    test_state_t subscriber_state;
    test_data_state_t data_state;
    uint16_t publisher_port;
    uint16_t subscriber_port;
    int waited;
    int received = 0;

    memset(&publisher_state, 0, sizeof(publisher_state));
    memset(&subscriber_state, 0, sizeof(subscriber_state));
    memset(&data_state, 0, sizeof(data_state));
    memset(&before, 0, sizeof(before));
    memset(&after, 0, sizeof(after));
    memset(&subscriber_stats, 0, sizeof(subscriber_stats));
    fill_config(&config, "tcp-publisher");
    config.enable_tcp = 1;
    config.tcp_port = 0;
    publisher = nb_rtps_participant_create(&config);
    CHECK(publisher != NULL);
    if (!publisher) {
        return;
    }
    CHECK(nb_rtps_participant_tcp_enabled(publisher));
    CHECK(nb_rtps_participant_tcp_port(publisher) != 0);
    publisher_port = nb_rtps_participant_spdp_port(publisher);

    fill_config(&config, "tcp-subscriber");
    config.enable_tcp = 1;
    config.tcp_port = 0;
    subscriber = nb_rtps_participant_create(&config);
    CHECK(subscriber != NULL);
    if (!subscriber) {
        nb_rtps_participant_destroy(publisher);
        return;
    }
    CHECK(nb_rtps_participant_tcp_port(subscriber) != 0);
    subscriber_port = nb_rtps_participant_spdp_port(subscriber);
    CHECK(nb_rtps_participant_add_peer(publisher, "127.0.0.1", subscriber_port) == NB_OK);
    CHECK(nb_rtps_participant_add_peer(subscriber, "127.0.0.1", publisher_port) == NB_OK);

    publisher_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    subscriber_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    data_state.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(publisher_state.lock && subscriber_state.lock && data_state.lock);
    if (!publisher_state.lock || !subscriber_state.lock || !data_state.lock) {
        nb_rtps_participant_destroy(publisher);
        nb_rtps_participant_destroy(subscriber);
        return;
    }
    nb_rtps_participant_set_callback(publisher, on_participant, &publisher_state);
    nb_rtps_participant_set_callback(subscriber, on_participant, &subscriber_state);
    nb_rtps_participant_set_endpoint_callback(publisher, on_endpoint, &publisher_state);
    nb_rtps_participant_set_endpoint_callback(subscriber, on_endpoint, &subscriber_state);
    nb_rtps_participant_set_data_callback(subscriber, on_data, &data_state);
    CHECK(nb_rtps_participant_start(publisher) == NB_OK);
    CHECK(nb_rtps_participant_start(subscriber) == NB_OK);
    CHECK(wait_count(&publisher_state, 0, 1, 5000));
    CHECK(wait_count(&subscriber_state, 0, 1, 5000));

    /* The link comes up on its own once both sides have discovered each
     * other's TCP locator. */
    for (waited = 0; waited < 5000; waited += 20) {
        if (nb_rtps_participant_tcp_links(publisher) > 0 &&
            nb_rtps_participant_tcp_links(subscriber) > 0) {
            break;
        }
        ntk_sleep_ms(20);
    }
    CHECK(nb_rtps_participant_tcp_links(publisher) == 1);
    CHECK(nb_rtps_participant_tcp_links(subscriber) == 1);

    init_data_endpoint(&publication, 1);
    CHECK(nb_rtps_participant_add_endpoint(
              publisher, NB_RTPS_ENDPOINT_PUBLICATION, &publication) == NB_OK);
    init_data_endpoint(&subscription, 1);
    CHECK(nb_rtps_participant_add_endpoint(
              subscriber, NB_RTPS_ENDPOINT_SUBSCRIPTION, &subscription) == NB_OK);
    CHECK(wait_endpoint_count(&subscriber_state, 0, 1, 5000));
    CHECK(wait_endpoint_count(&publisher_state, 0, 1, 5000));
    /* Let SEDP settle on the stream so the publisher's remote has a TCP key
     * before the sample is written. */
    ntk_sleep_ms(200);
    nb_rtps_participant_tcp_stats(publisher, &before);

    message.payload = expected;
    message.payload_len = sizeof(expected);
    for (waited = 0; waited < 5000 && !received; waited += 50) {
        if (nb_rtps_participant_publish_dds_message(
                publisher, &publication.endpoint_guid, &message) == NB_OK) {
            received = data_received(&data_state);
        }
        if (!received) {
            ntk_sleep_ms(50);
        }
    }
    CHECK(received);
    CHECK(data_state.payload_len == sizeof(expected));
    CHECK(memcmp(data_state.payload, expected, sizeof(expected)) == 0);

    /* The DATA left over the stream: the publisher's TCP byte counter grew
     * while no new UDP peer was involved. */
    nb_rtps_participant_tcp_stats(publisher, &after);
    CHECK(after.bytes_sent > before.bytes_sent);
    nb_rtps_participant_tcp_stats(subscriber, &subscriber_stats);
    CHECK(subscriber_stats.bytes_received > 0);
    CHECK(subscriber_stats.messages_received > 0);

    nb_rtps_participant_stop(publisher);
    nb_rtps_participant_stop(subscriber);
    nb_rtps_participant_destroy(publisher);
    nb_rtps_participant_destroy(subscriber);
    ntk_mutex_destroy(publisher_state.lock);
    ntk_mutex_destroy(subscriber_state.lock);
    ntk_mutex_destroy(data_state.lock);
}

static void test_standard_ports(void)
{
    nb_rtps_participant_config_t config;
    nb_rtps_participant_t *participant;
    nb_rtps_participant_info_t info;

    memset(&config, 0, sizeof(config));
    config.domain_id = 200;
    config.bind_host = "0.0.0.0";
    config.advertise_host = "127.0.0.1";
    config.multicast_group = "239.255.0.1";
    config.standard_ports = 1;
    config.participant_id = 5;
    config.announce_interval_ms = 100;
    config.lease_ms = 1200;
    config.name = "standard-ports";
    config.max_remote_participants = 8;

    participant = nb_rtps_participant_create(&config);
    CHECK(participant != NULL);
    if (!participant) {
        return;
    }
    CHECK(nb_rtps_participant_start(participant) == NB_OK);
    CHECK(nb_rtps_participant_local_info(participant, &info) == NB_OK);
    /* PB=7400, DG=250, PG=2, d0=0, d1=10, d3=11:
     *   SPDP multicast = 7400 + 250*200 + 0       = 57400
     *   metatraffic    = 7400 + 250*200 + 10 + 10 = 57420
     *   user unicast   = 7400 + 250*200 + 11 + 10 = 57421
     */
    CHECK(nb_rtps_participant_spdp_port(participant) == 57400);
    CHECK(info.metatraffic_unicast.port == 57420);
    CHECK(info.default_unicast.port == 57421);
    nb_rtps_participant_stop(participant);
    nb_rtps_participant_destroy(participant);
}

int main(void)
{
    test_create_destroy();
    test_unicast_peers();
    test_sedp_endpoints();
    test_metrics_counters();
    test_tcp_transport();
    test_discovery_views();
    test_data_roundtrip();
    test_fragmented_data();
    test_frag_nack_repair();
    test_frag_nack_repair_writer();
    test_multi_locator_delivery();
    test_lifespan_expiry();
    test_offered_deadline_missed();
    test_requested_deadline_missed();
    test_liveliness_lease();
    test_manual_liveliness();
    test_match_status();
    test_exclusive_ownership();
    test_destination_order_by_source();
    test_time_based_filter();
    test_dispose_is_consumed();
    test_instance_states();
    test_keyed_writer_lifecycle();
    test_generic_type_delivery();
    test_partition_matching();
    test_reliable_late_joiner();
    test_late_joiner_durability();
    test_reliable_gap();
    test_reliable_reader_capacity();
    test_multicast();
    test_standard_ports();
    if (g_fail) {
        nb_log_error("rtps participant tests failed");
        return 1;
    }
    nb_log_info("rtps participant tests passed");
    return 0;
}
