/* DDS backend: Cyclone DDS adapter (topic/writer/reader only). */
#include <nb/dds/backend.h>

#include <nb/base/error.h>

#include <dds/dds.h>

#include "cy_dds_message.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct nb_dds_cyclone_participant {
    dds_entity_t participant;
    dds_entity_t domain;
    int domain_created;
} nb_dds_cyclone_participant_t;

typedef struct nb_dds_cyclone_writer {
    dds_entity_t writer;
} nb_dds_cyclone_writer_t;

typedef struct nb_dds_cyclone_reader {
    dds_entity_t reader;
    dds_listener_t *listener;
    nb_dds_backend_on_sample_fn on_sample;
    void *user;
} nb_dds_cyclone_reader_t;

#define NB_CYCLONE_MAX_SAMPLES 32

static void *nb_cyclone_entity(dds_entity_t entity)
{
    return (void *)(intptr_t)entity;
}

static dds_entity_t nb_cyclone_entity_value(const void *entity)
{
    return (dds_entity_t)(intptr_t)entity;
}

/* ---- QoS mapping ------------------------------------------------------- */

static dds_qos_t *nb_cyclone_make_qos(const nb_dds_qos_t *src)
{
    dds_qos_t *qos = dds_create_qos();

    if (!qos) {
        return NULL;
    }
    /* The DDS module delivers to local readers itself; backends only carry
     * samples to remote participants. */
    dds_qset_ignorelocal(qos, DDS_IGNORELOCAL_PARTICIPANT);
    /* Uniform DDS defaults with the RTPS backend: best_effort / volatile /
     * keep_last depth 1. */
    dds_qset_reliability(qos, DDS_RELIABILITY_BEST_EFFORT, 0);
    dds_qset_durability(qos, DDS_DURABILITY_VOLATILE);
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 1);
    if (!src) {
        return qos;
    }
    if (src->present & NB_DDS_QOS_PRESENT_RELIABILITY) {
        if (src->reliability == NB_DDS_RELIABILITY_RELIABLE) {
            dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_MSECS(100));
        } else {
            dds_qset_reliability(qos, DDS_RELIABILITY_BEST_EFFORT, 0);
        }
    }
    if (src->present & NB_DDS_QOS_PRESENT_DURABILITY) {
        dds_qset_durability(qos,
                            src->durability == NB_DDS_DURABILITY_TRANSIENT_LOCAL
                                ? DDS_DURABILITY_TRANSIENT_LOCAL
                                : DDS_DURABILITY_VOLATILE);
    }
    if (src->present & (NB_DDS_QOS_PRESENT_HISTORY | NB_DDS_QOS_PRESENT_DEPTH)) {
        dds_history_kind_t kind =
            src->history == NB_DDS_HISTORY_KEEP_ALL ? DDS_HISTORY_KEEP_ALL
                                                    : DDS_HISTORY_KEEP_LAST;
        dds_qset_history(qos, kind, (int32_t)(src->depth ? src->depth : 1u));
    }
    if (src->present & NB_DDS_QOS_PRESENT_DEADLINE) {
        dds_qset_deadline(qos, DDS_MSECS(src->deadline_ms));
    }
    if (src->present & NB_DDS_QOS_PRESENT_LIFESPAN) {
        dds_qset_lifespan(qos, DDS_MSECS(src->lifespan_ms));
    }
    if (src->present & NB_DDS_QOS_PRESENT_LATENCY_BUDGET) {
        dds_qset_latency_budget(qos, DDS_MSECS(src->latency_budget_ms));
    }
    if (src->present & NB_DDS_QOS_PRESENT_MIN_SEPARATION) {
        dds_qset_time_based_filter(qos, DDS_MSECS(src->min_separation_ms));
    }
    if (src->present & NB_DDS_QOS_PRESENT_LIVELINESS) {
        dds_liveliness_kind_t kind = DDS_LIVELINESS_AUTOMATIC;
        if (src->liveliness == NB_DDS_LIVELINESS_MANUAL_BY_PARTICIPANT) {
            kind = DDS_LIVELINESS_MANUAL_BY_PARTICIPANT;
        } else if (src->liveliness == NB_DDS_LIVELINESS_MANUAL_BY_TOPIC) {
            kind = DDS_LIVELINESS_MANUAL_BY_TOPIC;
        }
        dds_qset_liveliness(qos, kind, DDS_MSECS(src->lease_duration_ms));
    }
    if (src->present & NB_DDS_QOS_PRESENT_OWNERSHIP) {
        dds_qset_ownership(qos,
                           src->ownership == NB_DDS_OWNERSHIP_EXCLUSIVE
                               ? DDS_OWNERSHIP_EXCLUSIVE
                               : DDS_OWNERSHIP_SHARED);
    }
    if ((src->present & NB_DDS_QOS_PRESENT_PARTITION) && src->partition[0]) {
        dds_qset_partition1(qos, src->partition);
    }
    if (src->present &
        (NB_DDS_QOS_PRESENT_MAX_SAMPLES | NB_DDS_QOS_PRESENT_MAX_INSTANCES |
         NB_DDS_QOS_PRESENT_MAX_SAMPLES_PER_INSTANCE)) {
        dds_qset_resource_limits(qos,
                                 src->max_samples > 0 ? (int32_t)src->max_samples
                                                      : DDS_LENGTH_UNLIMITED,
                                 src->max_instances > 0 ? (int32_t)src->max_instances
                                                        : DDS_LENGTH_UNLIMITED,
                                 src->max_samples_per_instance > 0
                                     ? (int32_t)src->max_samples_per_instance
                                     : DDS_LENGTH_UNLIMITED);
    }
    if (src->present & NB_DDS_QOS_PRESENT_AUTODISPOSE) {
        dds_qset_writer_data_lifecycle(qos, src->autodispose_unregistered_instances);
    }
    if (src->present & NB_DDS_QOS_PRESENT_PRESENTATION) {
        dds_presentation_access_scope_kind_t scope = DDS_PRESENTATION_INSTANCE;
        if (src->presentation_scope == NB_DDS_PRESENTATION_TOPIC) {
            scope = DDS_PRESENTATION_TOPIC;
        } else if (src->presentation_scope == NB_DDS_PRESENTATION_GROUP) {
            scope = DDS_PRESENTATION_GROUP;
        }
        dds_qset_presentation(qos, scope, src->coherent_access, src->ordered_access);
    }
    return qos;
}

/* ---- Participant ------------------------------------------------------- */

static int nb_cyclone_participant_create(const nb_dds_participant_options_t *options,
                                         void **out)
{
    nb_dds_cyclone_participant_t *wrapper;
    dds_qos_t *pq;
    const char *xml = options && options->cyclone ? options->cyclone->config_xml : NULL;

    if (!options || !out) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    wrapper = (nb_dds_cyclone_participant_t *)calloc(1, sizeof(*wrapper));
    if (!wrapper) {
        return NB_ERR_NOMEM;
    }
    wrapper->domain = DDS_ENTITY_NIL;
    wrapper->participant = DDS_ENTITY_NIL;
    if (xml && xml[0]) {
        wrapper->domain = dds_create_domain((dds_domainid_t)options->domain, xml);
        if (wrapper->domain < 0) {
            free(wrapper);
            return NB_ERR_IO;
        }
        wrapper->domain_created = 1;
    }
    pq = dds_create_qos();
    if (pq && options->name && options->name[0]) {
        dds_qset_entity_name(pq, options->name);
    }
    wrapper->participant =
        dds_create_participant((dds_domainid_t)options->domain, pq, NULL);
    if (pq) {
        dds_delete_qos(pq);
    }
    if (wrapper->participant < 0) {
        if (wrapper->domain_created) {
            dds_delete(wrapper->domain);
        }
        free(wrapper);
        return NB_ERR_IO;
    }
    *out = wrapper;
    return NB_OK;
}

static void nb_cyclone_participant_destroy(void *participant)
{
    nb_dds_cyclone_participant_t *wrapper = (nb_dds_cyclone_participant_t *)participant;

    if (!wrapper) {
        return;
    }
    if (wrapper->participant >= 0) {
        dds_delete(wrapper->participant);
    }
    if (wrapper->domain_created) {
        dds_delete(wrapper->domain);
    }
    free(wrapper);
}

static int nb_cyclone_participant_start(void *participant)
{
    return participant ? NB_OK : NB_ERR_INVALID;
}

static void nb_cyclone_participant_stop(void *participant)
{
    (void)participant;
}

/* ---- Topic / writer / reader ------------------------------------------ */

static int nb_cyclone_topic_create(void *participant,
                                   const char *wire_name,
                                   const char *type_name,
                                   const nb_dds_qos_t *qos,
                                   void **out)
{
    nb_dds_cyclone_participant_t *wrapper = (nb_dds_cyclone_participant_t *)participant;
    dds_entity_t topic;

    (void)type_name; /* the wire type is dds::Message for both backends */
    (void)qos;       /* endpoint QoS is applied at writer/reader creation */
    if (!wrapper || !wire_name || !out) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    topic = dds_create_topic(wrapper->participant, &dds_Message_desc, wire_name, NULL, NULL);
    if (topic < 0) {
        return NB_ERR_IO;
    }
    *out = nb_cyclone_entity(topic);
    return NB_OK;
}

static void nb_cyclone_topic_destroy(void *participant, void *topic)
{
    (void)participant;
    (void)dds_delete(nb_cyclone_entity_value(topic));
}

static int nb_cyclone_writer_create(void *participant,
                                    void *topic,
                                    const nb_dds_qos_t *qos,
                                    void **out)
{
    nb_dds_cyclone_participant_t *wrapper = (nb_dds_cyclone_participant_t *)participant;
    nb_dds_cyclone_writer_t *writer;
    dds_qos_t *qq;

    if (!wrapper || !topic || !out) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    writer = (nb_dds_cyclone_writer_t *)calloc(1, sizeof(*writer));
    if (!writer) {
        return NB_ERR_NOMEM;
    }
    qq = nb_cyclone_make_qos(qos);
    writer->writer = dds_create_writer(wrapper->participant, nb_cyclone_entity_value(topic), qq,
                                       NULL);
    if (qq) {
        dds_delete_qos(qq);
    }
    if (writer->writer < 0) {
        free(writer);
        return NB_ERR_IO;
    }
    *out = writer;
    return NB_OK;
}

static int nb_cyclone_writer_write(void *writer, const void *data, size_t len)
{
    nb_dds_cyclone_writer_t *wrapper = (nb_dds_cyclone_writer_t *)writer;
    dds_Message message;

    if (!wrapper || !data || len == 0 || len > UINT32_MAX) {
        return NB_ERR_INVALID;
    }
    memset(&message, 0, sizeof(message));
    message.payload._maximum = (uint32_t)len;
    message.payload._length = (uint32_t)len;
    message.payload._buffer = (uint8_t *)data;
    message.payload._release = false;
    if (dds_write(wrapper->writer, &message) < 0) {
        return NB_ERR_IO;
    }
    return NB_OK;
}

static void nb_cyclone_writer_destroy(void *participant, void *writer)
{
    nb_dds_cyclone_writer_t *wrapper = (nb_dds_cyclone_writer_t *)writer;

    (void)participant;
    if (!wrapper) {
        return;
    }
    (void)dds_delete(wrapper->writer);
    free(wrapper);
}

static void nb_cyclone_fill_info(const dds_sample_info_t *src, nb_dds_sample_info_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    if (src->valid_data) {
        dst->state = NB_SAMPLE_STATE_VALID;
    } else if (src->instance_state == DDS_NOT_ALIVE_DISPOSED_INSTANCE_STATE) {
        dst->state = NB_SAMPLE_STATE_DISPOSED;
    } else {
        dst->state = NB_SAMPLE_STATE_NO_WRITERS;
    }
    dst->has_instance_handle = src->instance_handle != 0;
    memcpy(dst->instance_handle, &src->instance_handle,
           sizeof(src->instance_handle) < sizeof(dst->instance_handle)
               ? sizeof(src->instance_handle)
               : sizeof(dst->instance_handle));
    dst->source_timestamp_ns = (int64_t)src->source_timestamp;
    dst->instance_state = (nb_instance_state_t)src->instance_state;
    dst->view_state = src->view_state == DDS_NEW_VIEW_STATE ? NB_VIEW_STATE_NEW
                                                            : NB_VIEW_STATE_NOT_NEW;
    dst->generation_rank = (int32_t)src->generation_rank;
}

static void nb_cyclone_on_data_available(dds_entity_t reader, void *arg)
{
    nb_dds_cyclone_reader_t *wrapper = (nb_dds_cyclone_reader_t *)arg;
    void *samples[NB_CYCLONE_MAX_SAMPLES];
    dds_sample_info_t infos[NB_CYCLONE_MAX_SAMPLES];
    dds_return_t count;
    int i;

    if (!wrapper || !wrapper->on_sample) {
        return;
    }
    memset(samples, 0, sizeof(samples));
    count = dds_take(reader, samples, infos, NB_CYCLONE_MAX_SAMPLES, NB_CYCLONE_MAX_SAMPLES);
    if (count <= 0) {
        return;
    }
    for (i = 0; i < count; i++) {
        const dds_Message *message = (const dds_Message *)samples[i];
        nb_dds_sample_info_t info;

        nb_cyclone_fill_info(&infos[i], &info);
        if (message && infos[i].valid_data && message->payload._buffer) {
            wrapper->on_sample(wrapper->user, message->payload._buffer,
                               (size_t)message->payload._length, 0, &info);
        } else {
            wrapper->on_sample(wrapper->user, NULL, 0, 0, &info);
        }
    }
    (void)dds_return_loan(reader, samples, count);
}

static int nb_cyclone_reader_create(void *participant,
                                    void *topic,
                                    const nb_dds_qos_t *qos,
                                    nb_dds_backend_on_sample_fn on_sample,
                                    void *user,
                                    void **out)
{
    nb_dds_cyclone_participant_t *wrapper = (nb_dds_cyclone_participant_t *)participant;
    nb_dds_cyclone_reader_t *reader;
    dds_qos_t *qq;

    if (!wrapper || !topic || !out) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    reader = (nb_dds_cyclone_reader_t *)calloc(1, sizeof(*reader));
    if (!reader) {
        return NB_ERR_NOMEM;
    }
    reader->on_sample = on_sample;
    reader->user = user;
    qq = nb_cyclone_make_qos(qos);
    if (on_sample) {
        reader->listener = dds_create_listener(reader);
        if (!reader->listener) {
            if (qq) {
                dds_delete_qos(qq);
            }
            free(reader);
            return NB_ERR_NOMEM;
        }
        dds_lset_data_available(reader->listener, nb_cyclone_on_data_available);
    }
    reader->reader = dds_create_reader(wrapper->participant, nb_cyclone_entity_value(topic), qq,
                                       reader->listener);
    if (qq) {
        dds_delete_qos(qq);
    }
    if (reader->reader < 0) {
        if (reader->listener) {
            dds_delete_listener(reader->listener);
        }
        free(reader);
        return NB_ERR_IO;
    }
    *out = reader;
    return NB_OK;
}

static void nb_cyclone_reader_destroy(void *participant, void *reader)
{
    nb_dds_cyclone_reader_t *wrapper = (nb_dds_cyclone_reader_t *)reader;

    (void)participant;
    if (!wrapper) {
        return;
    }
    (void)dds_delete(wrapper->reader);
    if (wrapper->listener) {
        dds_delete_listener(wrapper->listener);
    }
    free(wrapper);
}

static int nb_cyclone_set_status_callback(void *participant,
                                          nb_dds_status_callback_t callback,
                                          void *user)
{
    (void)participant;
    (void)user;
    /* Cyclone status requires per-endpoint status conditions/listeners; report
     * the gap instead of pretending. */
    return callback ? NB_ERR_NOT_SUPPORTED : NB_OK;
}

static size_t nb_cyclone_discovery_read(void *participant,
                                        nb_dds_builtin_topic_t topic,
                                        nb_dds_discovery_sample_t *out,
                                        size_t max)
{
    (void)participant;
    (void)topic;
    (void)out;
    (void)max;
    return 0;
}

const nb_dds_backend_ops_t nb_dds_cyclone_backend_ops = {
    .abi_version = NB_DDS_BACKEND_ABI_VERSION,
    .struct_size = sizeof(nb_dds_backend_ops_t),
    .id = NB_DDS_BACKEND_CYCLONE,
    .name = "cyclone",
    .participant_create = nb_cyclone_participant_create,
    .participant_destroy = nb_cyclone_participant_destroy,
    .participant_start = nb_cyclone_participant_start,
    .participant_stop = nb_cyclone_participant_stop,
    .topic_create = nb_cyclone_topic_create,
    .topic_destroy = nb_cyclone_topic_destroy,
    .writer_create = nb_cyclone_writer_create,
    .writer_write = nb_cyclone_writer_write,
    .writer_destroy = nb_cyclone_writer_destroy,
    .reader_create = nb_cyclone_reader_create,
    .reader_destroy = nb_cyclone_reader_destroy,
    .set_status_callback = nb_cyclone_set_status_callback,
    .discovery_read = nb_cyclone_discovery_read,
    .metrics_read = NULL,
};
