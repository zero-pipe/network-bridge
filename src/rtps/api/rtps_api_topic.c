#include "rtps_api_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- topic -------------------------------------------------------------- */

void rtps_topic_desc_init(rtps_topic_desc_t *desc)
{
    if (!desc) {
        return;
    }
    memset(desc, 0, sizeof(*desc));
    desc->struct_size = (uint32_t)sizeof(*desc);
}

static struct rtps_topic *find_topic_locked(struct rtps_participant *participant,
                                            const char *name)
{
    struct rtps_topic *topic;

    for (topic = participant->topics; topic; topic = topic->next) {
        if (strcmp(topic->name, name) == 0) {
            return topic;
        }
    }
    return NULL;
}

int rtps_topic_create(rtps_participant_t *participant,
                      const rtps_topic_desc_t *desc,
                      rtps_topic_t **out)
{
    struct rtps_topic *topic;
    struct rtps_topic *existing;

    if (!participant || !desc || !out || !desc->name || !desc->type_name) {
        return RTPS_ERR_INVALID;
    }
    if (!rtps_api_size_ok(desc->struct_size, sizeof(*desc)) ||
        desc->name[0] == '\0' || desc->type_name[0] == '\0' ||
        strlen(desc->name) >= RTPS_TOPIC_NAME_MAX ||
        strlen(desc->type_name) >= RTPS_TOPIC_TYPE_MAX) {
        return RTPS_ERR_INVALID;
    }
    *out = NULL;
    ntk_mutex_lock(participant->lock);
    existing = find_topic_locked(participant, desc->name);
    if (existing) {
        int same_type = strcmp(existing->type_name, desc->type_name) == 0;

        if (same_type) {
            *out = existing;
            ntk_mutex_unlock(participant->lock);
            return RTPS_OK;
        }
        ntk_mutex_unlock(participant->lock);
        return RTPS_ERR_PRECONDITION;
    }
    ntk_mutex_unlock(participant->lock);

    topic = (struct rtps_topic *)calloc(1, sizeof(*topic));
    if (!topic) {
        return RTPS_ERR_NOMEM;
    }
    topic->participant = participant;
    rtps_api_copy(topic->name, sizeof(topic->name), desc->name);
    rtps_api_copy(topic->type_name, sizeof(topic->type_name), desc->type_name);
    topic->qos = desc->qos;

    ntk_mutex_lock(participant->lock);
    /* Re-check: another thread may have created it while we allocated. */
    existing = find_topic_locked(participant, desc->name);
    if (existing) {
        int same_type = strcmp(existing->type_name, desc->type_name) == 0;

        ntk_mutex_unlock(participant->lock);
        free(topic);
        if (!same_type) {
            return RTPS_ERR_PRECONDITION;
        }
        *out = existing;
        return RTPS_OK;
    }
    topic->next = participant->topics;
    participant->topics = topic;
    ntk_mutex_unlock(participant->lock);
    *out = topic;
    return RTPS_OK;
}

static struct rtps_writer *topic_any_writer(struct rtps_participant *participant,
                                            struct rtps_topic *topic)
{
    struct rtps_writer *writer;

    for (writer = participant->writers; writer; writer = writer->next) {
        if (writer->topic == topic) {
            return writer;
        }
    }
    return NULL;
}

static struct rtps_reader *topic_any_reader(struct rtps_participant *participant,
                                            struct rtps_topic *topic)
{
    struct rtps_reader *reader;

    for (reader = participant->readers; reader; reader = reader->next) {
        if (reader->topic == topic) {
            return reader;
        }
    }
    return NULL;
}

int rtps_topic_destroy(rtps_topic_t *topic)
{
    struct rtps_participant *participant;
    struct rtps_topic **link;

    if (!topic) {
        return RTPS_ERR_INVALID;
    }
    participant = topic->participant;
    /* DDS parent/child rule: destroying a topic destroys its endpoints. Each
     * destroy talks to the engine, so it runs outside the participant lock. */
    for (;;) {
        struct rtps_writer *writer;
        struct rtps_reader *reader;

        ntk_mutex_lock(participant->lock);
        writer = topic_any_writer(participant, topic);
        reader = writer ? NULL : topic_any_reader(participant, topic);
        ntk_mutex_unlock(participant->lock);
        if (writer) {
            (void)rtps_writer_destroy(writer);
            continue;
        }
        if (reader) {
            (void)rtps_reader_destroy(reader);
            continue;
        }
        break;
    }

    ntk_mutex_lock(participant->lock);
    for (link = &participant->topics; *link; link = &(*link)->next) {
        if (*link == topic) {
            *link = topic->next;
            break;
        }
    }
    ntk_mutex_unlock(participant->lock);
    free(topic);
    return RTPS_OK;
}

int rtps_topic_find(rtps_participant_t *participant,
                    const char *name,
                    rtps_topic_t **out)
{
    struct rtps_topic *topic;

    if (!participant || !name || !out) {
        return RTPS_ERR_INVALID;
    }
    *out = NULL;
    ntk_mutex_lock(participant->lock);
    topic = find_topic_locked(participant, name);
    ntk_mutex_unlock(participant->lock);
    *out = topic;
    return topic ? RTPS_OK : RTPS_ERR_NOTFOUND;
}

const char *rtps_topic_name(const rtps_topic_t *topic)
{
    return topic ? topic->name : "";
}

const char *rtps_topic_type_name(const rtps_topic_t *topic)
{
    return topic ? topic->type_name : "";
}

int rtps_topic_get_qos(const rtps_topic_t *topic, rtps_qos_t *out)
{
    if (!topic || !out) {
        return RTPS_ERR_INVALID;
    }
    *out = topic->qos;
    return RTPS_OK;
}

int rtps_topic_endpoint_count(const rtps_topic_t *topic,
                              size_t *writers,
                              size_t *readers)
{
    if (!topic) {
        return RTPS_ERR_INVALID;
    }
    if (writers) {
        *writers = topic->writer_count;
    }
    if (readers) {
        *readers = topic->reader_count;
    }
    return RTPS_OK;
}
