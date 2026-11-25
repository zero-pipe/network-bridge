/* DDS spine: participant lifecycle and backend wiring. */
#include "dds_internal.h"

#include <nb/base/error.h>
#include <nb/base/log.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void nb_dds_status_trampoline(void *user, const nb_dds_status_event_t *event)
{
    nb_dds_participant_t *participant = (nb_dds_participant_t *)user;
    nb_dds_status_callback_t callback;
    void *cb_user;

    if (!participant) {
        return;
    }
    zb_mutex_lock(participant->lock);
    callback = participant->status_cb;
    cb_user = participant->status_user;
    zb_mutex_unlock(participant->lock);
    if (callback) {
        callback(cb_user, event);
    }
}

static void nb_dds_copy_name(char *dst, size_t cap, const char *src)
{
    if (src && src[0]) {
        (void)snprintf(dst, cap, "%s", src);
    } else {
        (void)snprintf(dst, cap, "%s", "nb-dds");
    }
}

static void nb_dds_copy_prefix(char *dst, size_t cap, const char *src)
{
    size_t len;

    dst[0] = '\0';
    if (!src || !src[0]) {
        return;
    }
    (void)snprintf(dst, cap, "%s", src);
    /* Strip trailing separators so prefix/name never doubles the slash. */
    len = strlen(dst);
    while (len > 0 && dst[len - 1] == '/') {
        dst[--len] = '\0';
    }
}

int nb_dds_participant_create(const nb_dds_participant_options_t *options,
                              nb_dds_participant_t **out)
{
    nb_dds_participant_t *participant;
    nb_dds_participant_options_t backend_options;
    const nb_dds_backend_ops_t *ops;
    int rc;

    if (!options || !out) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    ops = &nb_dds_rtps_backend_ops;
    participant = (nb_dds_participant_t *)calloc(1, sizeof(*participant));
    if (!participant) {
        return NB_ERR_NOMEM;
    }
    nb_dds_copy_name(participant->name, sizeof(participant->name), options->name);
    nb_dds_copy_prefix(participant->prefix, sizeof(participant->prefix), options->prefix);
    if (options->partition && options->partition[0]) {
        (void)snprintf(participant->partition, sizeof(participant->partition), "%s",
                       options->partition);
    }
    nb_dds_qos_init(&participant->default_qos);
    if (options->default_qos) {
        participant->default_qos = *options->default_qos;
    }
    if (participant->partition[0] &&
        !(participant->default_qos.present & NB_DDS_QOS_PRESENT_PARTITION)) {
        nb_dds_qos_set_partition(&participant->default_qos, participant->partition);
    }
    participant->ops = ops;
    participant->lock = zb_mutex_create(ZB_MUTEX_NORMAL);
    if (!participant->lock) {
        free(participant);
        return NB_ERR_NOMEM;
    }

    backend_options = *options;
    backend_options.name = participant->name;
    backend_options.partition = participant->partition[0] ? participant->partition : NULL;
    backend_options.prefix = participant->prefix[0] ? participant->prefix : NULL;
    backend_options.default_qos = &participant->default_qos;
    rc = ops->participant_create(&backend_options, &participant->backend);
    if (rc != NB_OK) {
        zb_mutex_destroy(participant->lock);
        free(participant);
        return rc;
    }
    rc = ops->participant_start(participant->backend);
    if (rc != NB_OK) {
        ops->participant_destroy(participant->backend);
        zb_mutex_destroy(participant->lock);
        free(participant);
        return rc;
    }
    if (ops->set_status_callback) {
        (void)ops->set_status_callback(participant->backend, nb_dds_status_trampoline,
                                       participant);
    }
    *out = participant;
    return NB_OK;
}

void nb_dds_participant_destroy(nb_dds_participant_t *participant)
{
    nb_dds_topic_t *topic;

    if (!participant) {
        return;
    }
    zb_mutex_lock(participant->lock);
    topic = participant->topics;
    participant->topics = NULL;
    zb_mutex_unlock(participant->lock);
    while (topic) {
        nb_dds_topic_t *next = topic->next;
        if (participant->ops->topic_destroy) {
            participant->ops->topic_destroy(participant->backend, topic->backend);
        }
        free(topic);
        topic = next;
    }
    if (participant->ops->participant_stop) {
        participant->ops->participant_stop(participant->backend);
    }
    participant->ops->participant_destroy(participant->backend);
    zb_mutex_destroy(participant->lock);
    free(participant);
}

const char *nb_dds_participant_name(const nb_dds_participant_t *participant)
{
    return participant ? participant->name : "";
}

int nb_dds_participant_set_status_callback(nb_dds_participant_t *participant,
                                           nb_dds_status_callback_t callback,
                                           void *user)
{
    if (!participant) {
        return NB_ERR_INVALID;
    }
    zb_mutex_lock(participant->lock);
    participant->status_cb = callback;
    participant->status_user = user;
    zb_mutex_unlock(participant->lock);
    return NB_OK;
}

size_t nb_dds_participant_discovery_read(nb_dds_participant_t *participant,
                                         nb_dds_builtin_topic_t topic,
                                         nb_dds_discovery_sample_t *out,
                                         size_t max)
{
    if (!participant || !out || max == 0 || !participant->ops->discovery_read) {
        return 0;
    }
    return participant->ops->discovery_read(participant->backend, topic, out, max);
}

void nb_dds_participant_metrics(const nb_dds_participant_t *participant,
                                nb_dds_metrics_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (participant && participant->ops->metrics_read) {
        participant->ops->metrics_read(participant->backend, out);
    }
}
