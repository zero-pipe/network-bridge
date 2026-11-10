/* DDS backend: in-tree RTPS engine adapter. */
#include <nb/dds/backend.h>

#include <nb/base/error.h>

#include <rtps/rtps.h>

#include <ntk.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NB_DDS_RTPS_WIRE_TYPE "dds::Message"

typedef struct nb_dds_rtps_participant {
    rtps_participant_t *participant;
    ntk_mutex *lock;
    nb_dds_status_callback_t status_cb;
    void *status_user;
} nb_dds_rtps_participant_t;

typedef struct nb_dds_rtps_reader {
    rtps_reader_t *reader;
    nb_dds_backend_on_sample_fn on_sample;
    void *user;
} nb_dds_rtps_reader_t;

/* ---- QoS mapping ------------------------------------------------------- */

static rtps_reliability_t nb_dds_rtps_reliability(nb_dds_reliability_t value)
{
    return value == NB_DDS_RELIABILITY_RELIABLE ? RTPS_RELIABILITY_RELIABLE
                                                : RTPS_RELIABILITY_BEST_EFFORT;
}

static rtps_durability_t nb_dds_rtps_durability(nb_dds_durability_t value)
{
    return value == NB_DDS_DURABILITY_TRANSIENT_LOCAL ? RTPS_DURABILITY_TRANSIENT_LOCAL
                                                      : RTPS_DURABILITY_VOLATILE;
}

static rtps_history_t nb_dds_rtps_history(nb_dds_history_t value)
{
    return value == NB_DDS_HISTORY_KEEP_ALL ? RTPS_HISTORY_KEEP_ALL : RTPS_HISTORY_KEEP_LAST;
}

static rtps_liveliness_t nb_dds_rtps_liveliness(nb_dds_liveliness_t value)
{
    switch (value) {
    case NB_DDS_LIVELINESS_MANUAL_BY_PARTICIPANT:
        return RTPS_LIVELINESS_MANUAL_BY_PARTICIPANT;
    case NB_DDS_LIVELINESS_MANUAL_BY_TOPIC:
        return RTPS_LIVELINESS_MANUAL_BY_TOPIC;
    default:
        return RTPS_LIVELINESS_AUTOMATIC;
    }
}

static void nb_dds_rtps_make_qos(const nb_dds_qos_t *src, rtps_qos_t *dst)
{
    /* Uniform DDS defaults across backends: best_effort / volatile /
     * keep_last depth 1. */
    rtps_qos_init(dst);
    rtps_qos_set_reliability(dst, RTPS_RELIABILITY_BEST_EFFORT);
    rtps_qos_set_durability(dst, RTPS_DURABILITY_VOLATILE);
    rtps_qos_set_history(dst, RTPS_HISTORY_KEEP_LAST, 1);
    if (!src) {
        return;
    }
    if (src->present & NB_DDS_QOS_PRESENT_RELIABILITY) {
        rtps_qos_set_reliability(dst, nb_dds_rtps_reliability(src->reliability));
    }
    if (src->present & NB_DDS_QOS_PRESENT_DURABILITY) {
        rtps_qos_set_durability(dst, nb_dds_rtps_durability(src->durability));
    }
    if (src->present & (NB_DDS_QOS_PRESENT_HISTORY | NB_DDS_QOS_PRESENT_DEPTH)) {
        rtps_qos_set_history(dst,
                             nb_dds_rtps_history(src->history),
                             src->depth ? src->depth : 1);
    }
    if (src->present & NB_DDS_QOS_PRESENT_DEADLINE) {
        rtps_qos_set_deadline_ms(dst, src->deadline_ms);
    }
    if (src->present & NB_DDS_QOS_PRESENT_LIFESPAN) {
        rtps_qos_set_lifespan_ms(dst, src->lifespan_ms);
    }
    if (src->present & NB_DDS_QOS_PRESENT_LATENCY_BUDGET) {
        rtps_qos_set_latency_budget_ms(dst, src->latency_budget_ms);
    }
    if (src->present & NB_DDS_QOS_PRESENT_MIN_SEPARATION) {
        rtps_qos_set_time_based_filter_ms(dst, src->min_separation_ms);
    }
    if (src->present & NB_DDS_QOS_PRESENT_LIVELINESS) {
        rtps_qos_set_liveliness(dst,
                                nb_dds_rtps_liveliness(src->liveliness),
                                src->lease_duration_ms);
    }
    if (src->present & NB_DDS_QOS_PRESENT_OWNERSHIP) {
        rtps_qos_set_ownership(dst,
                               src->ownership == NB_DDS_OWNERSHIP_EXCLUSIVE
                                   ? RTPS_OWNERSHIP_EXCLUSIVE
                                   : RTPS_OWNERSHIP_SHARED,
                               0);
    }
    if ((src->present & NB_DDS_QOS_PRESENT_PARTITION) && src->partition[0]) {
        rtps_qos_set_partition(dst, src->partition);
    }
    if (src->present &
        (NB_DDS_QOS_PRESENT_MAX_SAMPLES | NB_DDS_QOS_PRESENT_MAX_INSTANCES |
         NB_DDS_QOS_PRESENT_MAX_SAMPLES_PER_INSTANCE)) {
        rtps_qos_set_resource_limits(dst, src->max_samples, src->max_instances,
                                     src->max_samples_per_instance);
    }
    if (src->present & NB_DDS_QOS_PRESENT_AUTODISPOSE) {
        rtps_qos_set_autodispose(dst, src->autodispose_unregistered_instances ? 1 : 0);
    }
    if (src->present & NB_DDS_QOS_PRESENT_PRESENTATION) {
        rtps_qos_set_presentation(dst,
                                  (rtps_presentation_scope_t)src->presentation_scope,
                                  src->coherent_access ? 1 : 0,
                                  src->ordered_access ? 1 : 0);
    }
}

/* ---- Sample/status/discovery translation ------------------------------ */

static void nb_dds_rtps_fill_info(const rtps_sample_info_t *src, nb_dds_sample_info_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    if (!src) {
        dst->state = NB_SAMPLE_STATE_VALID;
        return;
    }
    if (src->valid_data) {
        dst->state = NB_SAMPLE_STATE_VALID;
    } else if (src->instance_state == RTPS_INSTANCE_NOT_ALIVE_DISPOSED) {
        dst->state = NB_SAMPLE_STATE_DISPOSED;
    } else {
        dst->state = NB_SAMPLE_STATE_NO_WRITERS;
    }
    dst->has_instance_handle = src->has_instance_handle != 0;
    memcpy(dst->instance_handle, src->instance_handle.bytes, sizeof(dst->instance_handle));
    dst->source_timestamp_ns = src->source_timestamp_ns;
    dst->reception_timestamp_ns = src->reception_timestamp_ns;
    dst->instance_state = (nb_instance_state_t)src->instance_state;
    dst->view_state =
        src->view_state == RTPS_VIEW_STATE_NEW ? NB_VIEW_STATE_NEW : NB_VIEW_STATE_NOT_NEW;
    dst->generation_rank = (int32_t)src->generation_rank;
}

static void nb_dds_rtps_on_data(void *user,
                                rtps_reader_t *reader,
                                const rtps_sample_t *samples,
                                size_t count)
{
    nb_dds_rtps_reader_t *wrapper = (nb_dds_rtps_reader_t *)user;
    size_t i;

    (void)reader;
    if (!wrapper || !wrapper->on_sample) {
        return;
    }
    for (i = 0; i < count; i++) {
        nb_dds_sample_info_t info;

        nb_dds_rtps_fill_info(&samples[i].info, &info);
        wrapper->on_sample(wrapper->user, samples[i].data, samples[i].size, 0, &info);
    }
}

static void nb_dds_rtps_format_guid(const rtps_guid_t *guid, char *out, size_t cap)
{
    (void)snprintf(out, cap, "%02x%02x%02x%02x-%02x%02x%02x%02x-%02x%02x%02x%02x:%02x%02x%02x%02x",
                   guid->prefix.bytes[0], guid->prefix.bytes[1], guid->prefix.bytes[2],
                   guid->prefix.bytes[3], guid->prefix.bytes[4], guid->prefix.bytes[5],
                   guid->prefix.bytes[6], guid->prefix.bytes[7], guid->prefix.bytes[8],
                   guid->prefix.bytes[9], guid->prefix.bytes[10], guid->prefix.bytes[11],
                   guid->entity.bytes[0], guid->entity.bytes[1], guid->entity.bytes[2],
                   guid->entity.bytes[3]);
}

static void nb_dds_rtps_on_status(void *user, const rtps_status_event_t *event)
{
    nb_dds_rtps_participant_t *participant = (nb_dds_rtps_participant_t *)user;
    nb_dds_status_callback_t callback;
    void *cb_user;
    nb_dds_status_event_t out;

    if (!participant || !event) {
        return;
    }
    ntk_mutex_lock(participant->lock);
    callback = participant->status_cb;
    cb_user = participant->status_user;
    ntk_mutex_unlock(participant->lock);
    if (!callback) {
        return;
    }
    memset(&out, 0, sizeof(out));
    out.kind = (nb_dds_status_kind_t)event->kind;
    nb_dds_rtps_format_guid(&event->endpoint, out.endpoint, sizeof(out.endpoint));
    out.count = event->count;
    out.total_count = event->total_count;
    out.alive = event->alive;
    out.alive_count = event->alive_count;
    out.not_alive_count = event->not_alive_count;
    out.has_peer = event->has_peer != 0;
    if (event->has_peer) {
        memcpy(out.peer_guid, event->peer.prefix.bytes, RTPS_GUID_PREFIX_SIZE);
        memcpy(out.peer_guid + RTPS_GUID_PREFIX_SIZE, event->peer.entity.bytes,
               RTPS_ENTITY_ID_SIZE);
    }
    (void)snprintf(out.reason, sizeof(out.reason), "%s", event->reason);
    callback(cb_user, &out);
}

/* ---- Backend ops ------------------------------------------------------- */

static int nb_dds_rtps_participant_create(const nb_dds_participant_options_t *options,
                                          void **out)
{
    nb_dds_rtps_participant_t *wrapper;
    rtps_participant_config_t config;
    int rc;
    size_t i;

    if (!options || !out) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    wrapper = (nb_dds_rtps_participant_t *)calloc(1, sizeof(*wrapper));
    if (!wrapper) {
        return NB_ERR_NOMEM;
    }
    wrapper->lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    if (!wrapper->lock) {
        free(wrapper);
        return NB_ERR_NOMEM;
    }
    rtps_participant_config_init(&config);
    config.domain_id = options->domain;
    config.name = options->name;
    if (options->rtps) {
        const nb_dds_rtps_options_t *rtps = options->rtps;

        config.bind_host = rtps->bind_host;
        config.advertise_host = rtps->advertise_host;
        config.multicast_group = rtps->multicast_group;
        config.spdp_port = rtps->spdp_port;
        config.unicast_port = rtps->unicast_port;
        config.announce_interval_ms = rtps->announce_interval_ms;
        config.lease_ms = rtps->lease_ms;
        config.data_heartbeat_ms = rtps->data_heartbeat_ms;
        config.data_history_depth = rtps->data_history_depth;
        config.enable_tcp = rtps->enable_tcp;
        config.tcp_port = rtps->tcp_port;
        config.standard_ports = rtps->standard_ports;
        config.participant_id = rtps->participant_id;
    }
    rc = rtps_participant_create(&config, &wrapper->participant);
    if (rc != RTPS_OK) {
        ntk_mutex_destroy(wrapper->lock);
        free(wrapper);
        return NB_ERR_IO;
    }
    if (options->rtps && options->rtps->peers) {
        for (i = 0; i < options->rtps->peer_count; i++) {
            const char *peer = options->rtps->peers[i];
            const char *colon;
            char host[64];
            size_t len;
            unsigned long port;

            if (!peer) {
                continue;
            }
            colon = strrchr(peer, ':');
            if (!colon || colon == peer) {
                continue;
            }
            len = (size_t)(colon - peer);
            if (len >= sizeof(host)) {
                continue;
            }
            memcpy(host, peer, len);
            host[len] = '\0';
            port = strtoul(colon + 1, NULL, 10);
            if (port == 0 || port > 65535) {
                continue;
            }
            (void)rtps_participant_add_peer(wrapper->participant, host, (uint16_t)port);
        }
    }
    rc = rtps_participant_set_status_callback(wrapper->participant, nb_dds_rtps_on_status,
                                              wrapper);
    (void)rc;
    *out = wrapper;
    return NB_OK;
}

static void nb_dds_rtps_participant_destroy(void *participant)
{
    nb_dds_rtps_participant_t *wrapper = (nb_dds_rtps_participant_t *)participant;

    if (!wrapper) {
        return;
    }
    if (wrapper->participant) {
        rtps_participant_destroy(wrapper->participant);
    }
    ntk_mutex_destroy(wrapper->lock);
    free(wrapper);
}

static int nb_dds_rtps_participant_start(void *participant)
{
    nb_dds_rtps_participant_t *wrapper = (nb_dds_rtps_participant_t *)participant;

    return rtps_participant_start(wrapper->participant) == RTPS_OK ? NB_OK : NB_ERR_IO;
}

static void nb_dds_rtps_participant_stop(void *participant)
{
    nb_dds_rtps_participant_t *wrapper = (nb_dds_rtps_participant_t *)participant;

    if (wrapper && wrapper->participant) {
        (void)rtps_participant_stop(wrapper->participant);
    }
}

static int nb_dds_rtps_topic_create(void *participant,
                                    const char *wire_name,
                                    const char *type_name,
                                    const nb_dds_qos_t *qos,
                                    void **out)
{
    nb_dds_rtps_participant_t *wrapper = (nb_dds_rtps_participant_t *)participant;
    rtps_topic_desc_t desc;
    rtps_topic_t *topic = NULL;
    int rc;

    (void)type_name; /* advisory; the wire type both backends share is dds::Message */
    if (!wrapper || !wire_name || !out) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    rtps_topic_desc_init(&desc);
    desc.name = wire_name;
    desc.type_name = NB_DDS_RTPS_WIRE_TYPE;
    nb_dds_rtps_make_qos(qos, &desc.qos);
    rc = rtps_topic_create(wrapper->participant, &desc, &topic);
    if (rc != RTPS_OK) {
        return NB_ERR_IO;
    }
    *out = topic;
    return NB_OK;
}

static void nb_dds_rtps_topic_destroy(void *participant, void *topic)
{
    (void)participant;
    (void)rtps_topic_destroy((rtps_topic_t *)topic);
}

static int nb_dds_rtps_writer_create(void *participant,
                                     void *topic,
                                     const nb_dds_qos_t *qos,
                                     void **out)
{
    rtps_qos_t rtps_qos;
    rtps_writer_t *writer = NULL;
    int rc;

    (void)participant;
    if (!topic || !out) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    nb_dds_rtps_make_qos(qos, &rtps_qos);
    rc = rtps_writer_create((rtps_topic_t *)topic, &rtps_qos, &writer);
    if (rc != RTPS_OK) {
        return NB_ERR_IO;
    }
    *out = writer;
    return NB_OK;
}

static int nb_dds_rtps_writer_write(void *writer, const void *data, size_t len)
{
    uint8_t *body;
    int rc;

    if (!writer || !data || len == 0 || len > (64u * 1024u * 1024u) - 8u) {
        return NB_ERR_INVALID;
    }
    /* dds::Message CDR body: u32 sequence length followed by the bytes.
     * rtps_writer_write prepends the 4-byte XCDR1 encapsulation. */
    body = (uint8_t *)malloc(len + 4u);
    if (!body) {
        return NB_ERR_NOMEM;
    }
    body[0] = (uint8_t)(len & 0xffu);
    body[1] = (uint8_t)((len >> 8) & 0xffu);
    body[2] = (uint8_t)((len >> 16) & 0xffu);
    body[3] = (uint8_t)((len >> 24) & 0xffu);
    memcpy(body + 4, data, len);
    rc = rtps_writer_write((rtps_writer_t *)writer, body, len + 4u);
    free(body);
    if (rc != RTPS_OK) {
        return NB_ERR_IO;
    }
    return NB_OK;
}

static void nb_dds_rtps_writer_destroy(void *participant, void *writer)
{
    (void)participant;
    (void)rtps_writer_destroy((rtps_writer_t *)writer);
}

static int nb_dds_rtps_reader_create(void *participant,
                                     void *topic,
                                     const nb_dds_qos_t *qos,
                                     nb_dds_backend_on_sample_fn on_sample,
                                     void *user,
                                     void **out)
{
    nb_dds_rtps_reader_t *wrapper;
    rtps_qos_t rtps_qos;
    int rc;

    (void)participant;
    if (!topic || !out) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    wrapper = (nb_dds_rtps_reader_t *)calloc(1, sizeof(*wrapper));
    if (!wrapper) {
        return NB_ERR_NOMEM;
    }
    wrapper->on_sample = on_sample;
    wrapper->user = user;
    nb_dds_rtps_make_qos(qos, &rtps_qos);
    rc = rtps_reader_create((rtps_topic_t *)topic, &rtps_qos, &wrapper->reader);
    if (rc != RTPS_OK) {
        free(wrapper);
        return NB_ERR_IO;
    }
    if (on_sample) {
        (void)rtps_reader_set_data_callback(wrapper->reader, nb_dds_rtps_on_data, wrapper);
    }
    *out = wrapper;
    return NB_OK;
}

static void nb_dds_rtps_reader_destroy(void *participant, void *reader)
{
    nb_dds_rtps_reader_t *wrapper = (nb_dds_rtps_reader_t *)reader;

    (void)participant;
    if (!wrapper) {
        return;
    }
    (void)rtps_reader_destroy(wrapper->reader);
    free(wrapper);
}

static int nb_dds_rtps_set_status_callback(void *participant,
                                           nb_dds_status_callback_t callback,
                                           void *user)
{
    nb_dds_rtps_participant_t *wrapper = (nb_dds_rtps_participant_t *)participant;

    if (!wrapper) {
        return NB_ERR_INVALID;
    }
    ntk_mutex_lock(wrapper->lock);
    wrapper->status_cb = callback;
    wrapper->status_user = user;
    ntk_mutex_unlock(wrapper->lock);
    return NB_OK;
}

static size_t nb_dds_rtps_discovery_read(void *participant,
                                         nb_dds_builtin_topic_t topic,
                                         nb_dds_discovery_sample_t *out,
                                         size_t max)
{
    nb_dds_rtps_participant_t *wrapper = (nb_dds_rtps_participant_t *)participant;
    rtps_discovery_sample_t *tmp;
    size_t count;
    size_t i;

    if (!wrapper || !out || max == 0) {
        return 0;
    }
    tmp = (rtps_discovery_sample_t *)calloc(max, sizeof(*tmp));
    if (!tmp) {
        return 0;
    }
    count = rtps_participant_discovery_read(wrapper->participant, (rtps_builtin_topic_t)topic,
                                            tmp, max);
    for (i = 0; i < count; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].topic = (nb_dds_builtin_topic_t)tmp[i].topic;
        memcpy(out[i].handle, tmp[i].handle, sizeof(out[i].handle));
        (void)snprintf(out[i].name, sizeof(out[i].name), "%s", tmp[i].name);
        (void)snprintf(out[i].type_name, sizeof(out[i].type_name), "%s", tmp[i].type_name);
        (void)snprintf(out[i].host, sizeof(out[i].host), "%s", tmp[i].host);
        out[i].port = tmp[i].port;
        out[i].is_local = tmp[i].is_local;
        out[i].is_writer = tmp[i].is_writer;
        (void)snprintf(out[i].partition, sizeof(out[i].partition), "%s", tmp[i].partition);
        out[i].reliability = tmp[i].reliability;
        out[i].durability = tmp[i].durability;
        out[i].history_kind = tmp[i].history_kind;
        out[i].history_depth = tmp[i].history_depth;
        out[i].deadline_ms = tmp[i].deadline_ms;
        out[i].lifespan_ms = tmp[i].lifespan_ms;
        out[i].liveliness_kind = tmp[i].liveliness_kind;
        out[i].liveliness_lease_ms = tmp[i].liveliness_lease_ms;
        out[i].ownership_strength = tmp[i].ownership_strength;
        out[i].lease_ms = tmp[i].lease_ms;
        out[i].writer_count = tmp[i].writer_count;
        out[i].reader_count = tmp[i].reader_count;
    }
    free(tmp);
    return count;
}

static void nb_dds_rtps_metrics_read(const void *participant, nb_dds_metrics_t *out)
{
    const nb_dds_rtps_participant_t *wrapper = (const nb_dds_rtps_participant_t *)participant;
    rtps_metrics_t metrics;

    if (!wrapper || !out) {
        return;
    }
    if (rtps_participant_metrics(wrapper->participant, &metrics) != RTPS_OK) {
        return;
    }
    out->samples_sent = metrics.data_sent;
    out->samples_received = metrics.data_received;
    out->bytes_sent = metrics.bytes_sent;
    out->bytes_received = metrics.bytes_received;
    out->packets_sent = metrics.packets_sent;
    out->packets_received = metrics.packets_received;
    out->heartbeats_sent = metrics.heartbeats_sent;
    out->heartbeats_received = metrics.heartbeats_received;
    out->nacks_sent = metrics.acknacks_sent;
    out->nacks_received = metrics.acknacks_received;
    out->gaps_sent = metrics.gaps_sent;
    out->gaps_received = metrics.gaps_received;
    out->parse_errors = metrics.parse_errors;
    out->samples_dropped = metrics.samples_dropped;
    out->participants = metrics.participants;
    out->publications = metrics.publications;
    out->subscriptions = metrics.subscriptions;
}

const nb_dds_backend_ops_t nb_dds_rtps_backend_ops = {
    .abi_version = NB_DDS_BACKEND_ABI_VERSION,
    .struct_size = sizeof(nb_dds_backend_ops_t),
    .id = NB_DDS_BACKEND_RTPS,
    .name = "rtps",
    .participant_create = nb_dds_rtps_participant_create,
    .participant_destroy = nb_dds_rtps_participant_destroy,
    .participant_start = nb_dds_rtps_participant_start,
    .participant_stop = nb_dds_rtps_participant_stop,
    .topic_create = nb_dds_rtps_topic_create,
    .topic_destroy = nb_dds_rtps_topic_destroy,
    .writer_create = nb_dds_rtps_writer_create,
    .writer_write = nb_dds_rtps_writer_write,
    .writer_destroy = nb_dds_rtps_writer_destroy,
    .reader_create = nb_dds_rtps_reader_create,
    .reader_destroy = nb_dds_rtps_reader_destroy,
    .set_status_callback = nb_dds_rtps_set_status_callback,
    .discovery_read = nb_dds_rtps_discovery_read,
    .metrics_read = nb_dds_rtps_metrics_read,
};
