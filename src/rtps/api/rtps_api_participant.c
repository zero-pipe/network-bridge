#include "rtps_api_internal.h"

#include <stdlib.h>
#include <string.h>

/*
 * Participant, engine callback dispatch, status mapping, discovery and
 * metrics. This is the only file that translates between the engine's
 * nb_rtps_* types and the public rtps_* types at the participant level.
 */

static struct rtps_reader *find_reader_locked(struct rtps_participant *participant,
                                              const nb_rtps_guid_t *guid)
{
    struct rtps_reader *reader;

    for (reader = participant->readers; reader; reader = reader->next) {
        if (memcmp(&reader->guid, guid, sizeof(reader->guid)) == 0) {
            return reader;
        }
    }
    return NULL;
}

/* ---- engine -> API callbacks ------------------------------------------- */

static void rtps_api_on_engine_data(void *user,
                                    const nb_rtps_guid_t *reader_guid,
                                    const nb_rtps_guid_t *writer_guid,
                                    const nb_rtps_dds_message_t *message)
{
    struct rtps_participant *participant = (struct rtps_participant *)user;
    struct rtps_reader *reader;
    rtps_data_callback_t callback;
    void *callback_user;
    rtps_reader_history_t *history;
    rtps_sample_t sample;

    (void)writer_guid;
    if (!participant || !reader_guid || !message) {
        return;
    }
    ntk_mutex_lock(participant->lock);
    reader = find_reader_locked(participant, reader_guid);
    if (!reader) {
        ntk_mutex_unlock(participant->lock);
        return;
    }
    reader->refs++;
    callback = reader->callback;
    callback_user = reader->callback_user;
    history = reader->history;
    ntk_mutex_unlock(participant->lock);

    rtps_api_fill_sample(message, &sample);
    if (history) {
        rtps_history_push(history, &sample);
    }
    if (callback) {
        callback(callback_user, reader, &sample, 1);
    }

    ntk_mutex_lock(participant->lock);
    if (reader->refs > 0) {
        reader->refs--;
    }
    rtps_reader_unref_locked(reader);
    ntk_mutex_unlock(participant->lock);
}

static void rtps_api_on_engine_status(void *user,
                                      const nb_rtps_status_event_t *event)
{
    struct rtps_participant *participant = (struct rtps_participant *)user;
    rtps_status_callback_t callback;
    void *callback_user;
    rtps_status_event_t out;

    if (!participant || !event) {
        return;
    }
    memset(&out, 0, sizeof(out));
    out.kind = (rtps_status_kind_t)event->kind;
    memcpy(&out.endpoint, &event->endpoint, sizeof(out.endpoint));
    out.count = event->count;
    out.total_count = event->total_count;
    memcpy(&out.peer, &event->writer, sizeof(out.peer));
    out.has_peer = event->has_peer;
    out.alive = event->alive;
    out.alive_count = event->alive_count;
    out.not_alive_count = event->not_alive_count;
    rtps_api_copy(out.reason, sizeof(out.reason), event->reason);

    ntk_mutex_lock(participant->lock);
    callback = participant->status_cb;
    callback_user = participant->status_user;
    ntk_mutex_unlock(participant->lock);
    if (callback) {
        callback(callback_user, &out);
    }
}

/* ---- lifecycle ---------------------------------------------------------- */

void rtps_participant_config_init(rtps_participant_config_t *config)
{
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->struct_size = (uint32_t)sizeof(*config);
}

int rtps_participant_create(const rtps_participant_config_t *config,
                            rtps_participant_t **out)
{
    nb_rtps_participant_config_t engine_config;
    struct rtps_participant *participant;

    if (!config || !out) {
        return RTPS_ERR_INVALID;
    }
    if (!rtps_api_size_ok(config->struct_size, sizeof(*config)) ||
        config->domain_id > 232u) {
        return RTPS_ERR_INVALID;
    }
    *out = NULL;
    participant = (struct rtps_participant *)calloc(1, sizeof(*participant));
    if (!participant) {
        return RTPS_ERR_NOMEM;
    }
    participant->lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    if (!participant->lock) {
        free(participant);
        return RTPS_ERR_NOMEM;
    }
    rtps_api_copy(participant->name, sizeof(participant->name), config->name);

    memset(&engine_config, 0, sizeof(engine_config));
    engine_config.domain_id = config->domain_id;
    engine_config.bind_host = config->bind_host;
    engine_config.advertise_host = config->advertise_host;
    engine_config.multicast_group = config->multicast_group;
    engine_config.spdp_port = config->spdp_port;
    engine_config.unicast_port = config->unicast_port;
    engine_config.announce_interval_ms = config->announce_interval_ms;
    engine_config.lease_ms = config->lease_ms;
    engine_config.data_heartbeat_ms = config->data_heartbeat_ms;
    engine_config.data_history_depth = config->data_history_depth;
    engine_config.name = config->name;
    engine_config.enable_tcp = config->enable_tcp;
    engine_config.tcp_port = config->tcp_port;
    engine_config.max_remote_participants = config->max_remote_participants;
    engine_config.max_local_endpoints = config->max_local_endpoints;
    engine_config.max_remote_endpoints = config->max_remote_endpoints;
    engine_config.dump_frames = config->dump_frames;
    participant->engine = nb_rtps_participant_create(&engine_config);
    if (!participant->engine) {
        ntk_mutex_destroy(participant->lock);
        free(participant);
        return RTPS_ERR_INVALID;
    }
    nb_rtps_participant_set_data_callback(
        participant->engine, rtps_api_on_engine_data, participant);
    nb_rtps_participant_set_status_callback(
        participant->engine, rtps_api_on_engine_status, participant);
    *out = participant;
    return RTPS_OK;
}

void rtps_participant_destroy(rtps_participant_t *participant)
{
    if (!participant) {
        return;
    }
    if (participant->started) {
        nb_rtps_participant_stop(participant->engine);
        participant->started = 0;
    }
    /* Grouping entities own their endpoints, and topics own the rest; destroy
     * from the leaves up so no list keeps a freed writer/reader. */
    while (participant->publishers) {
        (void)rtps_publisher_destroy(participant->publishers);
    }
    while (participant->subscribers) {
        (void)rtps_subscriber_destroy(participant->subscribers);
    }
    while (participant->topics) {
        (void)rtps_topic_destroy(participant->topics);
    }
    while (participant->waitsets) {
        rtps_waitset_destroy(participant->waitsets);
    }
    nb_rtps_participant_destroy(participant->engine);
    ntk_mutex_destroy(participant->lock);
    free(participant);
}

int rtps_participant_start(rtps_participant_t *participant)
{
    int rc;

    if (!participant) {
        return RTPS_ERR_INVALID;
    }
    if (participant->started) {
        return RTPS_ERR_ALREADY;
    }
    rc = rtps_api_error(nb_rtps_participant_start(participant->engine));
    if (rc == RTPS_OK) {
        participant->started = 1;
    }
    return rc;
}

int rtps_participant_stop(rtps_participant_t *participant)
{
    if (!participant) {
        return RTPS_ERR_INVALID;
    }
    if (!participant->started) {
        return RTPS_OK;
    }
    nb_rtps_participant_stop(participant->engine);
    participant->started = 0;
    return RTPS_OK;
}

int rtps_participant_add_peer(rtps_participant_t *participant,
                              const char *host,
                              uint16_t port)
{
    if (!participant || !host || port == 0 || participant->started) {
        return RTPS_ERR_INVALID;
    }
    return rtps_api_error(
        nb_rtps_participant_add_peer(participant->engine, host, port));
}

int rtps_participant_get_guid(const rtps_participant_t *participant,
                              rtps_guid_t *out)
{
    nb_rtps_participant_info_t info;

    if (!participant || !out) {
        return RTPS_ERR_INVALID;
    }
    if (nb_rtps_participant_local_info(participant->engine, &info) != NB_OK) {
        return RTPS_ERR_INTERNAL;
    }
    memcpy(out, &info.guid, sizeof(*out));
    return RTPS_OK;
}

const char *rtps_participant_name(const rtps_participant_t *participant)
{
    return participant ? participant->name : "";
}

int rtps_participant_set_status_callback(rtps_participant_t *participant,
                                         rtps_status_callback_t callback,
                                         void *user)
{
    if (!participant) {
        return RTPS_ERR_INVALID;
    }
    ntk_mutex_lock(participant->lock);
    participant->status_cb = callback;
    participant->status_user = user;
    ntk_mutex_unlock(participant->lock);
    return RTPS_OK;
}

/* ---- discovery and metrics --------------------------------------------- */

static void copy_discovery(const nb_discovery_sample_t *src,
                           rtps_discovery_sample_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->topic = (rtps_builtin_topic_t)src->topic;
    memcpy(dst->handle, src->handle, sizeof(dst->handle));
    rtps_api_copy(dst->name, sizeof(dst->name), src->name);
    rtps_api_copy(dst->type_name, sizeof(dst->type_name), src->type_name);
    rtps_api_copy(dst->host, sizeof(dst->host), src->host);
    dst->port = src->port;
    dst->is_local = src->is_local;
    dst->is_writer = src->is_writer;
    rtps_api_copy(dst->partition, sizeof(dst->partition), src->partition);
    dst->reliability = src->reliability;
    dst->durability = src->durability;
    dst->history_kind = src->history_kind;
    dst->history_depth = src->history_depth;
    dst->deadline_ms = src->deadline_ms;
    dst->lifespan_ms = src->lifespan_ms;
    dst->liveliness_kind = src->liveliness_kind;
    dst->liveliness_lease_ms = src->liveliness_lease_ms;
    dst->ownership_strength = src->ownership_strength;
    dst->lease_ms = src->lease_ms;
    dst->writer_count = src->writer_count;
    dst->reader_count = src->reader_count;
}

size_t rtps_participant_discovery_read(rtps_participant_t *participant,
                                       rtps_builtin_topic_t topic,
                                       rtps_discovery_sample_t *out,
                                       size_t max)
{
    nb_discovery_sample_t *tmp;
    size_t count;
    size_t i;

    if (!participant || !out || max == 0) {
        return 0;
    }
    tmp = (nb_discovery_sample_t *)calloc(max, sizeof(*tmp));
    if (!tmp) {
        return 0;
    }
    count = nb_rtps_participant_discovery_read(
        participant->engine, (int)topic, tmp, max);
    for (i = 0; i < count; i++) {
        copy_discovery(&tmp[i], &out[i]);
    }
    free(tmp);
    return count;
}

int rtps_participant_metrics(const rtps_participant_t *participant,
                             rtps_metrics_t *out)
{
    nb_metrics_t metrics;

    if (!participant || !out) {
        return RTPS_ERR_INVALID;
    }
    memset(&metrics, 0, sizeof(metrics));
    nb_rtps_participant_metrics(participant->engine, &metrics);
    memset(out, 0, sizeof(*out));
    out->packets_sent = metrics.rtps_packets_sent;
    out->packets_received = metrics.rtps_packets_received;
    out->bytes_sent = metrics.rtps_bytes_sent;
    out->bytes_received = metrics.rtps_bytes_received;
    out->spdp_sent = metrics.rtps_spdp_sent;
    out->spdp_received = metrics.rtps_spdp_received;
    out->sedp_sent = metrics.rtps_sedp_sent;
    out->sedp_received = metrics.rtps_sedp_received;
    out->data_sent = metrics.rtps_data_sent;
    out->data_received = metrics.rtps_data_received;
    out->heartbeats_sent = metrics.rtps_heartbeats_sent;
    out->heartbeats_received = metrics.rtps_heartbeats_received;
    out->acknacks_sent = metrics.rtps_acknacks_sent;
    out->acknacks_received = metrics.rtps_acknacks_received;
    out->gaps_sent = metrics.rtps_gaps_sent;
    out->gaps_received = metrics.rtps_gaps_received;
    out->fragments_sent = metrics.rtps_fragments_sent;
    out->fragments_received = metrics.rtps_fragments_received;
    out->nack_frags_sent = metrics.rtps_nack_frags_sent;
    out->nack_frags_received = metrics.rtps_nack_frags_received;
    out->parse_errors = metrics.rtps_parse_errors;
    out->samples_dropped = metrics.rtps_samples_dropped;
    out->lease_expired = metrics.rtps_lease_expired;
    out->participants = metrics.rtps_participants;
    out->publications = metrics.rtps_publications;
    out->subscriptions = metrics.rtps_subscriptions;
    return RTPS_OK;
}

uint16_t rtps_participant_tcp_port(const rtps_participant_t *participant)
{
    if (!participant) {
        return 0;
    }
    return nb_rtps_participant_tcp_port(participant->engine);
}
