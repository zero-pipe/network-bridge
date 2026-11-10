/* DDS spine: readers, delivery, history conversion. */
#include "dds_internal.h"

#include <nb/base/error.h>

#include <stdlib.h>
#include <string.h>

void nb_dds_sample_release(nb_dds_sample_t *samples, size_t count)
{
    size_t i;

    if (!samples) {
        return;
    }
    for (i = 0; i < count; i++) {
        nb_buf_release(samples[i].owner);
        samples[i].owner = NULL;
        samples[i].data.data = NULL;
        samples[i].data.len = 0;
    }
}

static void nb_dds_backend_sample(void *user,
                                  const void *data,
                                  size_t len,
                                  uint32_t encoding,
                                  const nb_dds_sample_info_t *info)
{
    nb_dds_reader_deliver((nb_dds_reader_t *)user, data, len, encoding, info);
}

void nb_dds_reader_deliver(nb_dds_reader_t *reader,
                           const void *data,
                           size_t len,
                           uint32_t encoding,
                           const nb_dds_sample_info_t *info)
{
    nb_dds_participant_t *participant;
    nb_sample_queue_t *history;
    nb_dds_reader_data_fn on_data;
    void *user;
    int destroy = 0;

    if (!reader) {
        return;
    }
    participant = reader->topic->participant;
    ntk_mutex_lock(participant->lock);
    if (reader->removed) {
        ntk_mutex_unlock(participant->lock);
        return;
    }
    reader->refs++;
    history = reader->history;
    on_data = reader->on_data;
    user = reader->user;
    ntk_mutex_unlock(participant->lock);

    if (history) {
        (void)nb_sample_queue_push(history, data, len, (uint8_t)encoding, info);
    } else if (on_data) {
        nb_dds_sample_t sample;
        memset(&sample, 0, sizeof(sample));
        sample.data.data = data;
        sample.data.len = len;
        sample.data.encoding = (uint8_t)encoding;
        if (info) {
            sample.info = *info;
        }
        on_data(user, &sample);
    }

    ntk_mutex_lock(participant->lock);
    reader->refs--;
    destroy = (reader->removed && reader->refs == 0);
    ntk_mutex_unlock(participant->lock);

    if (destroy) {
        if (reader->history) {
            nb_sample_queue_destroy(reader->history);
            reader->history = NULL;
        }
        participant->ops->reader_destroy(participant->backend, reader->backend);
        free(reader);
    }
}

void nb_dds_reader_unref(nb_dds_reader_t *reader)
{
    nb_dds_participant_t *participant;
    int destroy = 0;

    if (!reader) {
        return;
    }
    participant = reader->topic->participant;
    ntk_mutex_lock(participant->lock);
    if (reader->refs > 0) {
        reader->refs--;
    }
    destroy = (reader->removed && reader->refs == 0);
    ntk_mutex_unlock(participant->lock);
    if (destroy) {
        if (reader->history) {
            nb_sample_queue_destroy(reader->history);
            reader->history = NULL;
        }
        participant->ops->reader_destroy(participant->backend, reader->backend);
        free(reader);
    }
}

size_t nb_dds_reader_snapshot(nb_dds_topic_t *topic, nb_dds_reader_t **out, size_t max)
{
    nb_dds_participant_t *participant;
    nb_dds_reader_t *reader;
    size_t count = 0;

    if (!topic || !out || max == 0) {
        return 0;
    }
    participant = topic->participant;
    ntk_mutex_lock(participant->lock);
    for (reader = topic->readers; reader && count < max; reader = reader->next) {
        if (!reader->removed) {
            reader->refs++;
            out[count++] = reader;
        }
    }
    ntk_mutex_unlock(participant->lock);
    return count;
}

nb_sample_queue_t *nb_dds_reader_history(const nb_dds_reader_t *reader)
{
    return reader ? reader->history : NULL;
}

int nb_dds_reader_create(nb_dds_topic_t *topic,
                         const nb_dds_qos_t *qos,
                         nb_dds_reader_data_fn on_data,
                         void *user,
                         nb_dds_reader_t **out)
{
    nb_dds_reader_t *reader;
    nb_dds_participant_t *participant;
    uint32_t depth = 1;
    int rc;

    if (!topic || !out) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    participant = topic->participant;
    reader = (nb_dds_reader_t *)calloc(1, sizeof(*reader));
    if (!reader) {
        return NB_ERR_NOMEM;
    }
    reader->topic = topic;
    if (qos) {
        reader->qos = *qos;
    } else {
        nb_dds_qos_init(&reader->qos);
    }
    nb_dds_qos_inherit(&reader->qos, &topic->qos);
    rc = nb_dds_qos_validate(&reader->qos);
    if (rc != NB_OK) {
        free(reader);
        return rc;
    }
    reader->on_data = on_data;
    reader->user = user;
    if (!on_data) {
        if ((reader->qos.present & NB_DDS_QOS_PRESENT_DEPTH) && reader->qos.depth > 0) {
            depth = reader->qos.depth;
        }
        reader->history = nb_sample_queue_create(depth);
        if (!reader->history) {
            free(reader);
            return NB_ERR_NOMEM;
        }
    }
    rc = participant->ops->reader_create(participant->backend, topic->backend,
                                         &reader->qos, nb_dds_backend_sample, reader,
                                         &reader->backend);
    if (rc != NB_OK) {
        nb_sample_queue_destroy(reader->history);
        free(reader);
        return rc;
    }
    ntk_mutex_lock(participant->lock);
    topic->reader_count++;
    reader->next = topic->readers;
    topic->readers = reader;
    ntk_mutex_unlock(participant->lock);
    *out = reader;
    return NB_OK;
}

void nb_dds_reader_destroy(nb_dds_reader_t *reader)
{
    nb_dds_participant_t *participant;
    int destroy = 0;

    if (!reader) {
        return;
    }
    participant = reader->topic->participant;
    ntk_mutex_lock(participant->lock);
    if (!reader->removed) {
        nb_dds_reader_t **link = &reader->topic->readers;

        reader->removed = 1;
        while (*link) {
            if (*link == reader) {
                *link = reader->next;
                break;
            }
            link = &(*link)->next;
        }
        if (reader->topic->reader_count > 0) {
            reader->topic->reader_count--;
        }
    }
    if (reader->refs == 0) {
        destroy = 1;
    }
    ntk_mutex_unlock(participant->lock);

    if (destroy) {
        if (reader->history) {
            nb_sample_queue_destroy(reader->history);
            reader->history = NULL;
        }
        participant->ops->reader_destroy(participant->backend, reader->backend);
        free(reader);
    }
}

size_t nb_dds_reader_read(nb_dds_reader_t *reader,
                          const nb_dds_sample_filter_t *filter,
                          nb_dds_sample_t *out,
                          size_t max)
{
    if (!reader || !reader->history || !out || max == 0) {
        return 0;
    }
    return nb_sample_queue_read(reader->history, filter, out, max);
}

size_t nb_dds_reader_take(nb_dds_reader_t *reader,
                          const nb_dds_sample_filter_t *filter,
                          nb_dds_sample_t *out,
                          size_t max)
{
    if (!reader || !reader->history || !out || max == 0) {
        return 0;
    }
    return nb_sample_queue_take(reader->history, filter, out, max);
}

size_t nb_dds_reader_size(const nb_dds_reader_t *reader)
{
    return reader ? nb_sample_queue_size(reader->history) : 0;
}

uint64_t nb_dds_reader_dropped(const nb_dds_reader_t *reader)
{
    return reader ? nb_sample_queue_dropped(reader->history) : 0;
}
