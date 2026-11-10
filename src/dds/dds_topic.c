/* DDS spine: topics. */
#include "dds_internal.h"

#include <nb/base/error.h>
#include <nb/base/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int nb_dds_topic_make_wire_name(const nb_dds_participant_t *participant,
                                       const char *name,
                                       char *out,
                                       size_t cap)
{
    int n;

    if (participant->prefix[0]) {
        n = snprintf(out, cap, "%s/%s", participant->prefix, name);
    } else {
        n = snprintf(out, cap, "%s", name);
    }
    if (n < 0 || (size_t)n >= cap) {
        return NB_ERR_TOOBIG;
    }
    return NB_OK;
}

int nb_dds_topic_create(nb_dds_participant_t *participant,
                        const char *name,
                        const char *type_name,
                        const nb_dds_qos_t *qos,
                        nb_dds_topic_t **out)
{
    nb_dds_topic_t *topic;
    nb_dds_topic_t *existing;
    int rc;

    if (!participant || !name || !name[0] || !out) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    if (strlen(name) >= NB_DDS_TOPIC_NAME_MAX ||
        (type_name && strlen(type_name) >= NB_DDS_TOPIC_TYPE_MAX)) {
        return NB_ERR_TOOBIG;
    }
    topic = (nb_dds_topic_t *)calloc(1, sizeof(*topic));
    if (!topic) {
        return NB_ERR_NOMEM;
    }
    topic->participant = participant;
    (void)snprintf(topic->name, sizeof(topic->name), "%s", name);
    rc = nb_dds_topic_make_wire_name(participant, name, topic->wire_name,
                                     sizeof(topic->wire_name));
    if (rc != NB_OK) {
        free(topic);
        return rc;
    }
    ntk_mutex_lock(participant->lock);
    for (existing = participant->topics; existing; existing = existing->next) {
        if (strcmp(existing->wire_name, topic->wire_name) == 0) {
            ntk_mutex_unlock(participant->lock);
            free(topic);
            return NB_ERR_ALREADY;
        }
    }
    ntk_mutex_unlock(participant->lock);
    (void)snprintf(topic->type_name, sizeof(topic->type_name), "%s",
                   (type_name && type_name[0]) ? type_name : "dds::Message");
    if (qos) {
        topic->qos = *qos;
    } else {
        nb_dds_qos_init(&topic->qos);
    }
    nb_dds_qos_inherit(&topic->qos, &participant->default_qos);
    rc = nb_dds_qos_validate(&topic->qos);
    if (rc != NB_OK) {
        free(topic);
        return rc;
    }
    rc = participant->ops->topic_create(participant->backend, topic->wire_name,
                                        topic->type_name, &topic->qos, &topic->backend);
    if (rc != NB_OK) {
        free(topic);
        return rc;
    }
    ntk_mutex_lock(participant->lock);
    topic->next = participant->topics;
    participant->topics = topic;
    ntk_mutex_unlock(participant->lock);
    *out = topic;
    return NB_OK;
}

void nb_dds_topic_destroy(nb_dds_topic_t *topic)
{
    nb_dds_participant_t *participant;
    nb_dds_topic_t **link;

    if (!topic) {
        return;
    }
    participant = topic->participant;
    ntk_mutex_lock(participant->lock);
    if (topic->writer_count > 0 || topic->reader_count > 0) {
        ntk_mutex_unlock(participant->lock);
        nb_log_warn("dds topic '%s' still has endpoints; destroy them first", topic->name);
        return;
    }
    for (link = &participant->topics; *link; link = &(*link)->next) {
        if (*link == topic) {
            *link = topic->next;
            break;
        }
    }
    ntk_mutex_unlock(participant->lock);
    participant->ops->topic_destroy(participant->backend, topic->backend);
    free(topic);
}

const char *nb_dds_topic_name(const nb_dds_topic_t *topic)
{
    return topic ? topic->name : "";
}

const char *nb_dds_topic_type_name(const nb_dds_topic_t *topic)
{
    return topic ? topic->type_name : "";
}

const nb_dds_qos_t *nb_dds_topic_qos(const nb_dds_topic_t *topic)
{
    return topic ? &topic->qos : NULL;
}
