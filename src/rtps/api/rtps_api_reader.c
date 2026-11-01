#include "rtps_api_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- reader ------------------------------------------------------------- */

static size_t history_depth_from_qos(const rtps_qos_t *qos)
{
    uint32_t depth = 1;

    if (!qos) {
        return depth;
    }
    if (qos->present & RTPS_QOS_PRESENT_HISTORY) {
        if (qos->history == RTPS_HISTORY_KEEP_ALL) {
            depth = (qos->present & RTPS_QOS_PRESENT_RESOURCE_LIMITS) &&
                            qos->max_samples > 0
                        ? qos->max_samples
                        : 1024u;
        } else if (qos->depth > 0) {
            depth = qos->depth;
        }
    }
    if (depth > 4096u) {
        depth = 4096u;
    }
    return (size_t)depth;
}

int rtps_reader_create_internal(struct rtps_participant *participant,
                                struct rtps_subscriber *subscriber,
                                struct rtps_topic *topic,
                                const rtps_qos_t *qos,
                                struct rtps_reader **out)
{
    struct rtps_reader *reader;
    nb_rtps_endpoint_info_t info;
    rtps_qos_t merged;
    rtps_qos_t with_group;
    size_t depth;
    int rc;

    if (!participant || !topic || !out) {
        return RTPS_ERR_INVALID;
    }
    if (subscriber && subscriber->participant != participant) {
        return RTPS_ERR_INVALID;
    }
    *out = NULL;
    reader = (struct rtps_reader *)calloc(1, sizeof(*reader));
    if (!reader) {
        return RTPS_ERR_NOMEM;
    }
    /* Endpoint > subscriber > topic > DDS default. */
    if (subscriber) {
        rtps_api_qos_merge(&topic->qos, &subscriber->qos, &with_group);
    } else {
        with_group = topic->qos;
    }
    rtps_api_qos_merge(&with_group, qos, &merged);
    rtps_api_fill_endpoint(topic, &merged, &info);
    rc = nb_rtps_participant_add_endpoint(
        participant->engine, NB_RTPS_ENDPOINT_SUBSCRIPTION, &info);
    if (rc != NB_OK) {
        free(reader);
        return rtps_api_error(rc);
    }
    reader->topic = topic;
    reader->participant = participant;
    reader->subscriber = subscriber;
    reader->guid = info.endpoint_guid;
    reader->qos = merged;
    reader->refs = 1;

    /* A reader whose effective QoS asks for a history keeps one from the
     * start; callback-only readers allocate nothing. */
    if (merged.present & RTPS_QOS_PRESENT_HISTORY) {
        depth = history_depth_from_qos(&merged);
        reader->history = rtps_history_create(depth);
        if (!reader->history) {
            (void)nb_rtps_participant_remove_endpoint(
                reader->participant->engine, &reader->guid);
            free(reader);
            return RTPS_ERR_NOMEM;
        }
    }

    ntk_mutex_lock(participant->lock);
    reader->next = participant->readers;
    participant->readers = reader;
    if (subscriber) {
        reader->sub_next = subscriber->readers;
        subscriber->readers = reader;
        subscriber->reader_count++;
    }
    topic->reader_count++;
    ntk_mutex_unlock(participant->lock);
    *out = reader;
    return RTPS_OK;
}

int rtps_reader_create(rtps_topic_t *topic,
                       const rtps_qos_t *qos,
                       rtps_reader_t **out)
{
    if (!topic || !topic->participant) {
        return RTPS_ERR_INVALID;
    }
    return rtps_reader_create_internal(topic->participant, NULL, topic, qos, out);
}

int rtps_reader_create_on(rtps_subscriber_t *subscriber,
                          rtps_topic_t *topic,
                          const rtps_qos_t *qos,
                          rtps_reader_t **out)
{
    if (!subscriber || !topic || !out) {
        return RTPS_ERR_INVALID;
    }
    return rtps_reader_create_internal(
        subscriber->participant, subscriber, topic, qos, out);
}

/* Called with participant->lock held. Unlinks every wait set that watches this
 * reader's history. */
static void reader_remove_waitset_links_locked(struct rtps_reader *reader)
{
    rtps_reader_history_t *history = reader->history;

    if (!history) {
        return;
    }
    ntk_mutex_lock(history->lock);
    while (history->listeners) {
        rtps_ws_link_t *link = history->listeners;
        rtps_ws_link_t **slot;

        history->listeners = link->hist_next;
        slot = &link->waitset->links;
        while (*slot) {
            if (*slot == link) {
                *slot = link->ws_next;
                break;
            }
            slot = &(*slot)->ws_next;
        }
        free(link);
    }
    ntk_mutex_unlock(history->lock);
}

void rtps_reader_unref_locked(struct rtps_reader *reader)
{
    rtps_reader_history_t *history;

    if (!reader || !reader->destroyed || reader->refs > 0) {
        return;
    }
    history = reader->history;
    reader->history = NULL;
    if (history) {
        rtps_history_destroy(history);
    }
    free(reader);
}

int rtps_reader_destroy(rtps_reader_t *reader)
{
    struct rtps_participant *participant;
    struct rtps_reader **link;
    rtps_reader_history_t *history = NULL;
    int free_now = 0;

    if (!reader) {
        return RTPS_ERR_INVALID;
    }
    participant = reader->participant;
    (void)nb_rtps_participant_remove_endpoint(participant->engine, &reader->guid);

    ntk_mutex_lock(participant->lock);
    for (link = &participant->readers; *link; link = &(*link)->next) {
        if (*link == reader) {
            *link = reader->next;
            break;
        }
    }
    if (reader->subscriber) {
        struct rtps_reader **sub_link = &reader->subscriber->readers;

        while (*sub_link) {
            if (*sub_link == reader) {
                *sub_link = reader->sub_next;
                break;
            }
            sub_link = &(*sub_link)->sub_next;
        }
        if (reader->subscriber->reader_count > 0) {
            reader->subscriber->reader_count--;
        }
    }
    if (reader->topic && reader->topic->reader_count > 0) {
        reader->topic->reader_count--;
    }
    reader_remove_waitset_links_locked(reader);
    reader->destroyed = 1;
    if (reader->refs > 0) {
        reader->refs--; /* the owner's reference */
    }
    if (reader->refs == 0) {
        history = reader->history;
        reader->history = NULL;
        free_now = 1;
    }
    ntk_mutex_unlock(participant->lock);

    if (free_now) {
        if (history) {
            rtps_history_destroy(history);
        }
        free(reader);
    }
    return RTPS_OK;
}

int rtps_reader_set_data_callback(rtps_reader_t *reader,
                                  rtps_data_callback_t callback,
                                  void *user)
{
    if (!reader) {
        return RTPS_ERR_INVALID;
    }
    ntk_mutex_lock(reader->participant->lock);
    reader->callback = callback;
    reader->callback_user = user;
    ntk_mutex_unlock(reader->participant->lock);
    return RTPS_OK;
}

int rtps_reader_enable_history(rtps_reader_t *reader, uint32_t depth)
{
    int rc = RTPS_OK;

    if (!reader) {
        return RTPS_ERR_INVALID;
    }
    ntk_mutex_lock(reader->participant->lock);
    if (!reader->history) {
        reader->history = rtps_history_create(depth ? depth : 1u);
        if (!reader->history) {
            rc = RTPS_ERR_NOMEM;
        }
    } else if (depth > 0) {
        rtps_history_set_depth(reader->history, depth);
    }
    ntk_mutex_unlock(reader->participant->lock);
    return rc;
}

size_t rtps_reader_read(rtps_reader_t *reader,
                        const rtps_sample_filter_t *filter,
                        rtps_sample_t *out,
                        size_t max)
{
    size_t count = 0;

    if (!reader) {
        return 0;
    }
    ntk_mutex_lock(reader->participant->lock);
    if (reader->history && out && max > 0) {
        count = rtps_history_read(reader->history, filter, out, max, 0);
    }
    ntk_mutex_unlock(reader->participant->lock);
    return count;
}

size_t rtps_reader_take(rtps_reader_t *reader,
                        const rtps_sample_filter_t *filter,
                        rtps_sample_t *out,
                        size_t max)
{
    size_t count = 0;

    if (!reader) {
        return 0;
    }
    ntk_mutex_lock(reader->participant->lock);
    if (reader->history && out && max > 0) {
        count = rtps_history_read(reader->history, filter, out, max, 1);
    }
    ntk_mutex_unlock(reader->participant->lock);
    return count;
}

size_t rtps_reader_loan(rtps_reader_t *reader,
                        const rtps_sample_filter_t *filter,
                        rtps_sample_t *out,
                        size_t max)
{
    return rtps_reader_read(reader, filter, out, max);
}

void rtps_reader_return_loan(rtps_reader_t *reader,
                             rtps_sample_t *samples,
                             size_t count)
{
    /* read()/loan() return views into the history; nothing to free. The
     * callback exists so DDS-style code keeps its shape. */
    (void)reader;
    (void)samples;
    (void)count;
}

size_t rtps_reader_match_count(rtps_reader_t *reader,
                               const rtps_sample_filter_t *filter)
{
    size_t count = 0;

    if (!reader) {
        return 0;
    }
    ntk_mutex_lock(reader->participant->lock);
    if (reader->history) {
        count = rtps_history_match_count(reader->history, filter);
    }
    ntk_mutex_unlock(reader->participant->lock);
    return count;
}

size_t rtps_reader_size(const rtps_reader_t *reader)
{
    struct rtps_participant *participant;
    size_t count = 0;

    if (!reader) {
        return 0;
    }
    participant = reader->participant;
    ntk_mutex_lock(participant->lock);
    if (reader->history) {
        count = rtps_history_size(reader->history);
    }
    ntk_mutex_unlock(participant->lock);
    return count;
}

int rtps_reader_get_guid(const rtps_reader_t *reader, rtps_guid_t *out)
{
    if (!reader || !out) {
        return RTPS_ERR_INVALID;
    }
    memcpy(out, &reader->guid, sizeof(*out));
    return RTPS_OK;
}

int rtps_reader_get_qos(const rtps_reader_t *reader, rtps_qos_t *out)
{
    if (!reader || !out) {
        return RTPS_ERR_INVALID;
    }
    *out = reader->qos;
    return RTPS_OK;
}

size_t rtps_reader_matched_count(const rtps_reader_t *reader)
{
    if (!reader) {
        return 0;
    }
    return nb_rtps_participant_matched_count(reader->participant->engine,
                                             &reader->guid);
}
