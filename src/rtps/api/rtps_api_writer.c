#include "rtps_api_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- writer ------------------------------------------------------------- */

int rtps_writer_create_internal(struct rtps_participant *participant,
                                struct rtps_publisher *publisher,
                                struct rtps_topic *topic,
                                const rtps_qos_t *qos,
                                struct rtps_writer **out)
{
    struct rtps_writer *writer;
    nb_rtps_endpoint_info_t info;
    rtps_qos_t merged;
    rtps_qos_t with_group;
    int rc;

    if (!participant || !topic || !out) {
        return RTPS_ERR_INVALID;
    }
    if (publisher && publisher->participant != participant) {
        return RTPS_ERR_INVALID;
    }
    *out = NULL;
    writer = (struct rtps_writer *)calloc(1, sizeof(*writer));
    if (!writer) {
        return RTPS_ERR_NOMEM;
    }
    /* Endpoint > publisher > topic > DDS default. */
    if (publisher) {
        rtps_api_qos_merge(&topic->qos, &publisher->qos, &with_group);
    } else {
        with_group = topic->qos;
    }
    rtps_api_qos_merge(&with_group, qos, &merged);
    rtps_api_fill_endpoint(topic, &merged, &info);
    rc = nb_rtps_participant_add_endpoint(
        participant->engine, NB_RTPS_ENDPOINT_PUBLICATION, &info);
    if (rc != NB_OK) {
        free(writer);
        return rtps_api_error(rc);
    }
    writer->topic = topic;
    writer->participant = participant;
    writer->publisher = publisher;
    writer->guid = info.endpoint_guid;
    writer->qos = merged;

    ntk_mutex_lock(participant->lock);
    writer->next = participant->writers;
    participant->writers = writer;
    if (publisher) {
        writer->pub_next = publisher->writers;
        publisher->writers = writer;
        publisher->writer_count++;
    }
    topic->writer_count++;
    ntk_mutex_unlock(participant->lock);
    *out = writer;
    return RTPS_OK;
}

int rtps_writer_create(rtps_topic_t *topic,
                       const rtps_qos_t *qos,
                       rtps_writer_t **out)
{
    if (!topic || !topic->participant) {
        return RTPS_ERR_INVALID;
    }
    return rtps_writer_create_internal(topic->participant, NULL, topic, qos, out);
}

int rtps_writer_create_on(rtps_publisher_t *publisher,
                          rtps_topic_t *topic,
                          const rtps_qos_t *qos,
                          rtps_writer_t **out)
{
    if (!publisher || !topic || !out) {
        return RTPS_ERR_INVALID;
    }
    return rtps_writer_create_internal(
        publisher->participant, publisher, topic, qos, out);
}

int rtps_writer_destroy(rtps_writer_t *writer)
{
    struct rtps_participant *participant;
    struct rtps_writer **link;

    if (!writer) {
        return RTPS_ERR_INVALID;
    }
    participant = writer->participant;
    (void)nb_rtps_participant_remove_endpoint(participant->engine, &writer->guid);

    ntk_mutex_lock(participant->lock);
    for (link = &participant->writers; *link; link = &(*link)->next) {
        if (*link == writer) {
            *link = writer->next;
            break;
        }
    }
    if (writer->publisher) {
        struct rtps_writer **pub_link = &writer->publisher->writers;

        while (*pub_link) {
            if (*pub_link == writer) {
                *pub_link = writer->pub_next;
                break;
            }
            pub_link = &(*pub_link)->pub_next;
        }
        if (writer->publisher->writer_count > 0) {
            writer->publisher->writer_count--;
        }
    }
    if (writer->topic && writer->topic->writer_count > 0) {
        writer->topic->writer_count--;
    }
    ntk_mutex_unlock(participant->lock);
    free(writer);
    return RTPS_OK;
}

static int writer_publish_body(struct rtps_writer *writer,
                               const void *data,
                               size_t size)
{
    uint8_t *buffer;
    size_t total;
    int rc;

    if (!writer || !data || size == 0 ||
        size > NB_RTPS_SAMPLE_MAX - NB_RTPS_CDR_HEADER_SIZE) {
        return RTPS_ERR_INVALID;
    }
    total = NB_RTPS_CDR_HEADER_SIZE + size;
    buffer = (uint8_t *)malloc(total);
    if (!buffer) {
        return RTPS_ERR_NOMEM;
    }
    /* XCDR1 little-endian encapsulation, the same header the engine strips on
     * receive. */
    buffer[0] = 0x00;
    buffer[1] = 0x01;
    buffer[2] = 0x00;
    buffer[3] = 0x00;
    memcpy(buffer + NB_RTPS_CDR_HEADER_SIZE, data, size);
    rc = nb_rtps_participant_publish(
        writer->participant->engine, &writer->guid, buffer, total);
    free(buffer);
    /* No matched reader yet is not an error for a write. */
    if (rc == NB_ERR_NOTFOUND) {
        return RTPS_OK;
    }
    return rtps_api_error(rc);
}

int rtps_writer_write(rtps_writer_t *writer, const void *data, size_t size)
{
    return writer_publish_body(writer, data, size);
}

int rtps_writer_write_serialized(rtps_writer_t *writer,
                                 const void *payload,
                                 size_t size)
{
    int rc;

    if (!writer || !payload || size == 0 || size > NB_RTPS_SAMPLE_MAX) {
        return RTPS_ERR_INVALID;
    }
    rc = nb_rtps_participant_publish(
        writer->participant->engine, &writer->guid, payload, size);
    if (rc == NB_ERR_NOTFOUND) {
        return RTPS_OK;
    }
    return rtps_api_error(rc);
}

int rtps_writer_write_batch(rtps_writer_t *writer,
                            const rtps_payload_t *payloads,
                            size_t count)
{
    size_t i;

    if (!writer || (count > 0 && !payloads)) {
        return RTPS_ERR_INVALID;
    }
    for (i = 0; i < count; i++) {
        int rc = rtps_writer_write(writer, payloads[i].data, payloads[i].size);

        if (rc != RTPS_OK) {
            return rc;
        }
    }
    return RTPS_OK;
}

int rtps_writer_write_instance(rtps_writer_t *writer,
                               const rtps_instance_handle_t *handle,
                               rtps_instance_state_t state,
                               const void *data,
                               size_t size)
{
    uint32_t status;
    uint8_t *buffer = NULL;
    size_t total = 0;
    int rc;

    if (!writer || !handle) {
        return RTPS_ERR_INVALID;
    }
    switch (state) {
    case RTPS_INSTANCE_ALIVE:
        status = 0;
        break;
    case RTPS_INSTANCE_NOT_ALIVE_DISPOSED:
        status = 1;
        break;
    case RTPS_INSTANCE_NOT_ALIVE_NO_WRITERS:
        status = 2;
        break;
    default:
        return RTPS_ERR_INVALID;
    }
    if (state == RTPS_INSTANCE_ALIVE && (!data || size == 0)) {
        return RTPS_ERR_INVALID;
    }
    if (data && size > 0) {
        if (size > NB_RTPS_SAMPLE_MAX - NB_RTPS_CDR_HEADER_SIZE) {
            return RTPS_ERR_TOOBIG;
        }
        total = NB_RTPS_CDR_HEADER_SIZE + size;
        buffer = (uint8_t *)malloc(total);
        if (!buffer) {
            return RTPS_ERR_NOMEM;
        }
        buffer[0] = 0x00;
        buffer[1] = 0x01;
        buffer[2] = 0x00;
        buffer[3] = 0x00;
        memcpy(buffer + NB_RTPS_CDR_HEADER_SIZE, data, size);
    }
    rc = nb_rtps_participant_publish_instance(writer->participant->engine,
                                              &writer->guid,
                                              handle->bytes,
                                              status,
                                              buffer,
                                              total);
    free(buffer);
    if (rc == NB_ERR_NOTFOUND) {
        return RTPS_OK;
    }
    return rtps_api_error(rc);
}

int rtps_writer_get_guid(const rtps_writer_t *writer, rtps_guid_t *out)
{
    if (!writer || !out) {
        return RTPS_ERR_INVALID;
    }
    memcpy(out, &writer->guid, sizeof(*out));
    return RTPS_OK;
}

int rtps_writer_get_qos(const rtps_writer_t *writer, rtps_qos_t *out)
{
    if (!writer || !out) {
        return RTPS_ERR_INVALID;
    }
    *out = writer->qos;
    return RTPS_OK;
}

size_t rtps_writer_matched_count(const rtps_writer_t *writer)
{
    if (!writer) {
        return 0;
    }
    return nb_rtps_participant_matched_count(writer->participant->engine,
                                             &writer->guid);
}
