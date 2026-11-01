#include "rtps_api_internal.h"

#include <stdlib.h>
#include <string.h>

/*
 * Publisher and Subscriber are DCPS grouping entities: they carry a QoS
 * template for their writers/readers and own them for lifetime purposes.
 * Nothing about them is announced on the wire and they have no RTPS entity id.
 */

int rtps_publisher_create(rtps_participant_t *participant,
                          const rtps_qos_t *qos,
                          rtps_publisher_t **out)
{
    struct rtps_publisher *publisher;

    if (!participant || !out) {
        return RTPS_ERR_INVALID;
    }
    *out = NULL;
    publisher = (struct rtps_publisher *)calloc(1, sizeof(*publisher));
    if (!publisher) {
        return RTPS_ERR_NOMEM;
    }
    publisher->participant = participant;
    if (qos) {
        publisher->qos = *qos;
    }
    ntk_mutex_lock(participant->lock);
    publisher->next = participant->publishers;
    participant->publishers = publisher;
    ntk_mutex_unlock(participant->lock);
    *out = publisher;
    return RTPS_OK;
}

int rtps_publisher_destroy(rtps_publisher_t *publisher)
{
    struct rtps_participant *participant;
    struct rtps_publisher **link;

    if (!publisher) {
        return RTPS_ERR_INVALID;
    }
    participant = publisher->participant;
    /* DDS parent/child rule: the publisher owns its writers. */
    while (publisher->writers) {
        if (rtps_writer_destroy(publisher->writers) != RTPS_OK) {
            break;
        }
    }
    ntk_mutex_lock(participant->lock);
    for (link = &participant->publishers; *link; link = &(*link)->next) {
        if (*link == publisher) {
            *link = publisher->next;
            break;
        }
    }
    ntk_mutex_unlock(participant->lock);
    free(publisher);
    return RTPS_OK;
}

int rtps_publisher_get_qos(const rtps_publisher_t *publisher, rtps_qos_t *out)
{
    if (!publisher || !out) {
        return RTPS_ERR_INVALID;
    }
    *out = publisher->qos;
    return RTPS_OK;
}

size_t rtps_publisher_writer_count(const rtps_publisher_t *publisher)
{
    return publisher ? publisher->writer_count : 0;
}

int rtps_subscriber_create(rtps_participant_t *participant,
                           const rtps_qos_t *qos,
                           rtps_subscriber_t **out)
{
    struct rtps_subscriber *subscriber;

    if (!participant || !out) {
        return RTPS_ERR_INVALID;
    }
    *out = NULL;
    subscriber = (struct rtps_subscriber *)calloc(1, sizeof(*subscriber));
    if (!subscriber) {
        return RTPS_ERR_NOMEM;
    }
    subscriber->participant = participant;
    if (qos) {
        subscriber->qos = *qos;
    }
    ntk_mutex_lock(participant->lock);
    subscriber->next = participant->subscribers;
    participant->subscribers = subscriber;
    ntk_mutex_unlock(participant->lock);
    *out = subscriber;
    return RTPS_OK;
}

int rtps_subscriber_destroy(rtps_subscriber_t *subscriber)
{
    struct rtps_participant *participant;
    struct rtps_subscriber **link;

    if (!subscriber) {
        return RTPS_ERR_INVALID;
    }
    participant = subscriber->participant;
    while (subscriber->readers) {
        if (rtps_reader_destroy(subscriber->readers) != RTPS_OK) {
            break;
        }
    }
    ntk_mutex_lock(participant->lock);
    for (link = &participant->subscribers; *link; link = &(*link)->next) {
        if (*link == subscriber) {
            *link = subscriber->next;
            break;
        }
    }
    ntk_mutex_unlock(participant->lock);
    free(subscriber);
    return RTPS_OK;
}

int rtps_subscriber_get_qos(const rtps_subscriber_t *subscriber, rtps_qos_t *out)
{
    if (!subscriber || !out) {
        return RTPS_ERR_INVALID;
    }
    *out = subscriber->qos;
    return RTPS_OK;
}

size_t rtps_subscriber_reader_count(const rtps_subscriber_t *subscriber)
{
    return subscriber ? subscriber->reader_count : 0;
}
