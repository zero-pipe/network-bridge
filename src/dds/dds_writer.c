/* DDS spine: writers. */
#include "dds_internal.h"

#include <nb/base/error.h>

#include <stdlib.h>
#include <string.h>

int nb_dds_writer_create(nb_dds_topic_t *topic,
                         const nb_dds_qos_t *qos,
                         nb_dds_writer_t **out)
{
    nb_dds_writer_t *writer;
    nb_dds_participant_t *participant;
    int rc;

    if (!topic || !out) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    participant = topic->participant;
    writer = (nb_dds_writer_t *)calloc(1, sizeof(*writer));
    if (!writer) {
        return NB_ERR_NOMEM;
    }
    writer->topic = topic;
    if (qos) {
        writer->qos = *qos;
    } else {
        nb_dds_qos_init(&writer->qos);
    }
    nb_dds_qos_inherit(&writer->qos, &topic->qos);
    rc = nb_dds_qos_validate(&writer->qos);
    if (rc != NB_OK) {
        free(writer);
        return rc;
    }
    rc = participant->ops->writer_create(participant->backend, topic->backend,
                                         &writer->qos, &writer->backend);
    if (rc != NB_OK) {
        free(writer);
        return rc;
    }
    ntk_mutex_lock(participant->lock);
    topic->writer_count++;
    ntk_mutex_unlock(participant->lock);
    *out = writer;
    return NB_OK;
}

void nb_dds_writer_destroy(nb_dds_writer_t *writer)
{
    nb_dds_participant_t *participant;

    if (!writer) {
        return;
    }
    participant = writer->topic->participant;
    participant->ops->writer_destroy(participant->backend, writer->backend);
    ntk_mutex_lock(participant->lock);
    if (writer->topic->writer_count > 0) {
        writer->topic->writer_count--;
    }
    ntk_mutex_unlock(participant->lock);
    free(writer);
}

static void nb_dds_writer_deliver_local(nb_dds_writer_t *writer, const void *data, size_t size)
{
    nb_dds_reader_t *readers[64];
    nb_dds_sample_info_t info;
    size_t count;
    size_t i;

    memset(&info, 0, sizeof(info));
    info.state = NB_SAMPLE_STATE_VALID;
    info.instance_state = NB_INSTANCE_STATE_ALIVE;
    info.view_state = NB_VIEW_STATE_NEW;
    count = nb_dds_reader_snapshot(writer->topic, readers,
                                   sizeof(readers) / sizeof(readers[0]));
    for (i = 0; i < count; i++) {
        nb_dds_reader_deliver(readers[i], data, size, 0, &info);
        nb_dds_reader_unref(readers[i]);
    }
}

int nb_dds_writer_write(nb_dds_writer_t *writer, const void *data, size_t size)
{
    int rc;

    if (!writer || !data || size == 0) {
        return NB_ERR_INVALID;
    }
    /* Local readers on the same participant get the sample first; backends are
     * configured to ignore local endpoints, so they only carry it to peers. */
    nb_dds_writer_deliver_local(writer, data, size);
    rc = writer->topic->participant->ops->writer_write(writer->backend, data, size);
    return rc;
}

int nb_dds_writer_write_payload(nb_dds_writer_t *writer, const nb_payload_t *payload)
{
    if (!payload) {
        return NB_ERR_INVALID;
    }
    return nb_dds_writer_write(writer, payload->data, payload->len);
}

int nb_dds_writer_write_owned(nb_dds_writer_t *writer, nb_owned_payload_t *payload)
{
    nb_payload_t view;
    int rc;

    if (!payload) {
        return NB_ERR_INVALID;
    }
    view = nb_owned_payload_view(payload);
    rc = nb_dds_writer_write(writer, view.data, view.len);
    nb_owned_payload_release(payload);
    return rc;
}
