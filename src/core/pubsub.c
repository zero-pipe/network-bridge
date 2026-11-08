/*
 * Pub/sub spine: participants, topics, writers and readers.
 *
 * A participant borrows one topic-capable binding of a node. Topics are
 * registered with the provider by their own name (nb_transport_provider_ops
 * .register_topic), so the wire topic is the application's topic name rather
 * than a "service.method" route.
 *
 * Inbound samples arrive as EVENT messages whose route is the topic name.
 * dispatch.c hands those to nb_ps_handle_event() before the service lookup, so
 * topics never collide with RPC routes (topic names may not contain '.'). A
 * sample is delivered either to a callback reader or into a KEEP_LAST history
 * that reuses nb_sample_queue.
 *
 * Threading: writer calls are synchronous sends and may be made from any
 * thread that may send on the node. Callback readers run on the receiving
 * provider thread. History readers and read/take are safe for one application
 * reader per subscription.
 */

#include "node_internal.h"
#include "pubsub_internal.h"
#include "sample_queue.h"
#include "transport_internal.h"
#include "util.h"

#include <nb/error.h>
#include <nb/log.h>
#include <nb/pubsub.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Readers sharing one topic in one process. The receive path delivers to at
 * most this many readers without allocating, so the cap is also the delivery
 * limit; reader creation past it fails with NB_ERR_TOOBIG. */
#define NB_PS_MAX_READERS_PER_TOPIC 8u

typedef struct nb_ps_reader_entry {
    struct nb_ps_reader_entry *next;
    char topic[NB_TOPIC_NAME_MAX];
    nb_reader_data_fn cb;
    void *user;
    nb_sample_queue_t *queue;
    /* One reference for the registry plus one per in-flight delivery, so a
     * reader destroyed from inside a callback cannot be freed underneath the
     * delivery loop. */
    unsigned refs;
    int removed;
} nb_ps_reader_entry_t;

struct nb_participant {
    nb_node_t *node;
    nb_transport_t *transport;
    char name[NB_NAME_MAX];
    nb_ps_qos_t default_qos;
};

struct nb_topic {
    nb_participant_t *participant;
    char name[NB_TOPIC_NAME_MAX];
    char type_name[NB_TOPIC_TYPE_MAX];
    nb_ps_qos_t qos;
    int writer_registered;
    int reader_registered;
};

struct nb_writer {
    nb_topic_t *topic;
    nb_endpoint_t target;
};

struct nb_reader {
    nb_topic_t *topic;
    nb_ps_reader_entry_t *entry;
};

/* ---- helpers ------------------------------------------------------------ */

static int nb_ps_provider_supports_topics(const nb_transport_t *t)
{
    const nb_transport_provider_ops_t *ops;

    if (!t || !t->provider || !t->provider->ops) {
        return 0;
    }
    ops = t->provider->ops;
    if (ops->struct_size < offsetof(nb_transport_provider_ops_t, register_topic) +
                               sizeof(ops->register_topic)) {
        return 0;
    }
    return ops->register_topic != NULL;
}

static void nb_ps_reader_entry_free(nb_ps_reader_entry_t *entry)
{
    if (!entry) {
        return;
    }
    nb_sample_queue_destroy(entry->queue);
    free(entry);
}

/* Unlink an entry from the node registry and drop the registry reference. The
 * entry is freed once no delivery is still holding a reference. */
static void nb_ps_entry_unlink(nb_node_t *node, nb_ps_reader_entry_t *entry)
{
    nb_ps_reader_entry_t **link;

    if (!node || !entry || !node->ps_lock) {
        return;
    }
    ntk_mutex_lock(node->ps_lock);
    link = (nb_ps_reader_entry_t **)&node->ps_readers;
    while (*link && *link != entry) {
        link = &(*link)->next;
    }
    if (*link == entry) {
        *link = entry->next;
        entry->removed = 1;
        entry->refs--;
        if (entry->refs == 0) {
            nb_ps_reader_entry_free(entry);
        }
    }
    ntk_mutex_unlock(node->ps_lock);
}

static size_t nb_ps_reader_count(nb_node_t *node, const char *topic)
{
    nb_ps_reader_entry_t *entry;
    size_t count = 0;

    for (entry = (nb_ps_reader_entry_t *)node->ps_readers; entry; entry = entry->next) {
        if (strcmp(entry->topic, topic) == 0) {
            count++;
        }
    }
    return count;
}

/* ---- participant -------------------------------------------------------- */

int nb_participant_create(const nb_participant_options_t *options, nb_participant_t **out)
{
    nb_participant_t *participant;
    nb_transport_t *transport = NULL;
    size_t i;

    if (!options || !options->node || !out) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    for (i = 0; i < options->node->transport_count; i++) {
        nb_transport_t *t = options->node->transports[i];

        if (!t) {
            continue;
        }
        if (options->scheme && strcmp(t->scheme, options->scheme) != 0) {
            continue;
        }
        if (!nb_ps_provider_supports_topics(t)) {
            continue;
        }
        transport = t;
        break;
    }
    if (!transport) {
        /* Either the named scheme has no binding, or its backend has no native
         * topics (today: the udp/tcp RPC links). */
        return options->scheme ? NB_ERR_NOT_SUPPORTED : NB_ERR_NOTFOUND;
    }
    participant = (nb_participant_t *)calloc(1, sizeof(*participant));
    if (!participant) {
        return NB_ERR_NOMEM;
    }
    participant->node = options->node;
    participant->transport = transport;
    nb_copy_str(participant->name, sizeof(participant->name), options->name, "ps");
    if (options->default_qos) {
        participant->default_qos = *options->default_qos;
    }
    *out = participant;
    return NB_OK;
}

void nb_participant_destroy(nb_participant_t *participant)
{
    /* The transport belongs to the node; topics hold a back-pointer and must be
     * destroyed first (they are not freed here). */
    free(participant);
}

const char *nb_participant_name(const nb_participant_t *participant)
{
    return participant ? participant->name : NULL;
}

/* ---- topic -------------------------------------------------------------- */

int nb_topic_create(nb_participant_t *participant,
                    const char *name,
                    const char *type_name,
                    const nb_ps_qos_t *qos,
                    nb_topic_t **out)
{
    nb_topic_t *topic;

    if (!participant || !name || !name[0] || !out) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    if (strlen(name) >= NB_TOPIC_NAME_MAX) {
        return NB_ERR_TOOBIG;
    }
    /* Topic names live in a namespace disjoint from "service.method" so the
     * receive path can tell them apart without a per-message flag. */
    if (strchr(name, '.') != NULL) {
        return NB_ERR_INVALID;
    }
    if (type_name && strlen(type_name) >= NB_TOPIC_TYPE_MAX) {
        return NB_ERR_TOOBIG;
    }
    if (qos && nb_ps_qos_validate(qos) != NB_OK) {
        return NB_ERR_INVALID;
    }
    topic = (nb_topic_t *)calloc(1, sizeof(*topic));
    if (!topic) {
        return NB_ERR_NOMEM;
    }
    topic->participant = participant;
    nb_copy_str(topic->name, sizeof(topic->name), name, "");
    nb_copy_str(topic->type_name, sizeof(topic->type_name), type_name, "");
    if (qos) {
        topic->qos = *qos;
    } else {
        nb_ps_qos_inherit(&topic->qos, &participant->default_qos);
    }
    *out = topic;
    return NB_OK;
}

void nb_topic_destroy(nb_topic_t *topic)
{
    free(topic);
}

const char *nb_topic_name(const nb_topic_t *topic)
{
    return topic ? topic->name : NULL;
}

const char *nb_topic_type_name(const nb_topic_t *topic)
{
    return topic ? topic->type_name : NULL;
}

/* ---- writer ------------------------------------------------------------- */

int nb_writer_create(nb_topic_t *topic, const nb_ps_qos_t *qos, nb_writer_t **out)
{
    nb_participant_t *participant;
    nb_writer_t *writer;
    int rc;

    if (!topic || !out || !topic->participant) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    if (qos && nb_ps_qos_validate(qos) != NB_OK) {
        return NB_ERR_INVALID;
    }
    participant = topic->participant;
    if (!topic->writer_registered) {
        rc = nb_transport_register_topic(participant->transport,
                                         topic->name,
                                         NB_PS_TOPIC_WRITER);
        if (rc != NB_OK) {
            return rc;
        }
        topic->writer_registered = 1;
    }
    writer = (nb_writer_t *)calloc(1, sizeof(*writer));
    if (!writer) {
        return NB_ERR_NOMEM;
    }
    writer->topic = topic;
    /* The send target only selects the participant's binding; the providers
     * resolve the actual peers through their own discovery/matching. */
    if (nb_endpoint_from_parts(&writer->target,
                               participant->transport->scheme,
                               participant->transport->bind_host,
                               participant->transport->port,
                               participant->transport->kind) != NB_OK) {
        free(writer);
        return NB_ERR_STATE;
    }
    *out = writer;
    return NB_OK;
}

void nb_writer_destroy(nb_writer_t *writer)
{
    free(writer);
}

int nb_writer_write(nb_writer_t *writer, const nb_payload_t *payload)
{
    nb_message_t message;

    if (!writer || !payload || (!payload->data && payload->len > 0)) {
        return NB_ERR_INVALID;
    }
    memset(&message, 0, sizeof(message));
    message.kind = NB_MSG_KIND_EVENT;
    message.route = writer->topic->name;
    message.payload = *payload;
    return nb_node_send(writer->topic->participant->node, &writer->target, &message);
}

int nb_writer_write_owned(nb_writer_t *writer, nb_owned_payload_t *payload)
{
    nb_payload_t view;
    int rc;

    if (!payload) {
        return NB_ERR_INVALID;
    }
    view = nb_owned_payload_view(payload);
    rc = nb_writer_write(writer, &view);
    /* The framework copies synchronously, so the owned buffer is ours to free
     * regardless of the send result. */
    nb_owned_payload_release(payload);
    return rc;
}

/* ---- reader ------------------------------------------------------------- */

int nb_reader_create(nb_topic_t *topic,
                     const nb_ps_qos_t *qos,
                     nb_reader_data_fn on_data,
                     void *user,
                     uint32_t depth,
                     nb_reader_t **out)
{
    nb_participant_t *participant;
    nb_ps_reader_entry_t *entry;
    nb_reader_t *reader;
    int rc;

    if (!topic || !out || !topic->participant) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    if (!on_data && depth == 0) {
        return NB_ERR_INVALID; /* neither a callback nor a history */
    }
    if (on_data && depth != 0) {
        return NB_ERR_INVALID; /* a reader is either a callback or a history */
    }
    if (qos && nb_ps_qos_validate(qos) != NB_OK) {
        return NB_ERR_INVALID;
    }
    participant = topic->participant;
    entry = (nb_ps_reader_entry_t *)calloc(1, sizeof(*entry));
    if (!entry) {
        return NB_ERR_NOMEM;
    }
    nb_copy_str(entry->topic, sizeof(entry->topic), topic->name, "");
    entry->cb = on_data;
    entry->user = user;
    if (!on_data) {
        entry->queue = nb_sample_queue_create(depth);
        if (!entry->queue) {
            free(entry);
            return NB_ERR_NOMEM;
        }
    }

    ntk_mutex_lock(participant->node->ps_lock);
    if (!topic->reader_registered) {
        rc = nb_transport_register_topic(participant->transport,
                                         topic->name,
                                         NB_PS_TOPIC_READER);
        if (rc != NB_OK) {
            ntk_mutex_unlock(participant->node->ps_lock);
            nb_ps_reader_entry_free(entry);
            return rc;
        }
        topic->reader_registered = 1;
    }
    if (nb_ps_reader_count(participant->node, topic->name) >= NB_PS_MAX_READERS_PER_TOPIC) {
        ntk_mutex_unlock(participant->node->ps_lock);
        nb_ps_reader_entry_free(entry);
        return NB_ERR_TOOBIG;
    }
    entry->refs = 1;
    entry->next = (nb_ps_reader_entry_t *)participant->node->ps_readers;
    participant->node->ps_readers = entry;
    ntk_mutex_unlock(participant->node->ps_lock);

    reader = (nb_reader_t *)calloc(1, sizeof(*reader));
    if (!reader) {
        nb_ps_entry_unlink(participant->node, entry);
        return NB_ERR_NOMEM;
    }
    reader->topic = topic;
    reader->entry = entry;
    *out = reader;
    return NB_OK;
}

void nb_reader_destroy(nb_reader_t *reader)
{
    nb_node_t *node;

    if (!reader) {
        return;
    }
    node = reader->topic ? reader->topic->participant->node : NULL;
    nb_ps_entry_unlink(node, reader->entry);
    free(reader);
}

size_t nb_reader_read(nb_reader_t *reader,
                      const nb_sample_filter_t *filter,
                      nb_sample_t *out,
                      size_t max)
{
    if (!reader || !reader->entry || !reader->entry->queue || !out) {
        return 0;
    }
    return nb_sample_queue_read(reader->entry->queue, filter, out, max);
}

size_t nb_reader_take(nb_reader_t *reader,
                      const nb_sample_filter_t *filter,
                      nb_sample_t *out,
                      size_t max)
{
    if (!reader || !reader->entry || !reader->entry->queue || !out) {
        return 0;
    }
    return nb_sample_queue_take(reader->entry->queue, filter, out, max);
}

size_t nb_reader_size(const nb_reader_t *reader)
{
    if (!reader || !reader->entry || !reader->entry->queue) {
        return 0;
    }
    return nb_sample_queue_size(reader->entry->queue);
}

nb_sample_queue_t *nb_ps_reader_queue(nb_reader_t *reader)
{
    return (reader && reader->entry) ? reader->entry->queue : NULL;
}

/* ---- inbound delivery (called from dispatch.c) -------------------------- */

int nb_ps_handle_event(nb_node_t *node, nb_transport_t *transport, nb_message_t *message)
{
    nb_ps_reader_entry_t *matched[NB_PS_MAX_READERS_PER_TOPIC];
    size_t count = 0;
    size_t i;
    int found = 0;

    (void)transport;
    if (!node || !message || !message->route || !message->route[0] || !node->ps_lock) {
        return 0;
    }
    ntk_mutex_lock(node->ps_lock);
    if (node->ps_readers) {
        nb_ps_reader_entry_t *entry;

        for (entry = (nb_ps_reader_entry_t *)node->ps_readers; entry; entry = entry->next) {
            if (strcmp(entry->topic, message->route) != 0) {
                continue;
            }
            found = 1;
            if (count < NB_PS_MAX_READERS_PER_TOPIC) {
                entry->refs++;
                matched[count++] = entry;
            }
        }
    }
    ntk_mutex_unlock(node->ps_lock);
    if (!found) {
        return 0;
    }

    for (i = 0; i < count; i++) {
        nb_ps_reader_entry_t *entry = matched[i];

        if (entry->queue) {
            if (nb_sample_queue_push(entry->queue, message) != NB_OK) {
                nb_log_warn("pubsub: enqueue on topic %s failed", entry->topic);
            }
        } else if (entry->cb) {
            nb_sample_t sample;

            memset(&sample, 0, sizeof(sample));
            nb_message_view_init(&sample.data);
            (void)nb_message_view_set_borrowed(&sample.data,
                                               message->payload.data,
                                               message->payload.len);
            sample.data.encoding = message->payload.encoding;
            sample.info = message->sample;
            entry->cb(entry->user, &sample);
        }
    }

    ntk_mutex_lock(node->ps_lock);
    for (i = 0; i < count; i++) {
        nb_ps_reader_entry_t *entry = matched[i];

        entry->refs--;
        if (entry->removed && entry->refs == 0) {
            nb_ps_reader_entry_free(entry);
        }
    }
    ntk_mutex_unlock(node->ps_lock);
    return 1;
}

void nb_ps_node_destroy(nb_node_t *node)
{
    nb_ps_reader_entry_t *entry;

    if (!node || !node->ps_lock) {
        return;
    }
    ntk_mutex_lock(node->ps_lock);
    entry = (nb_ps_reader_entry_t *)node->ps_readers;
    node->ps_readers = NULL;
    ntk_mutex_unlock(node->ps_lock);
    while (entry) {
        nb_ps_reader_entry_t *next = entry->next;

        nb_ps_reader_entry_free(entry);
        entry = next;
    }
}
