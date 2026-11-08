#ifndef NB_PUBSUB_H
#define NB_PUBSUB_H

/*
 * Publish/subscribe spine: typed topics, writers, readers, history, waitsets
 * and status events.
 *
 * Object model:
 *   nb_participant_t  a runtime attached to a node's pub/sub binding
 *   nb_topic_t        name + type name + QoS; the unit of matching
 *   nb_writer_t       publishes samples on one topic
 *   nb_reader_t       receives samples, either through a callback or a
 *                     bounded KEEP_LAST history drained with read/take
 *
 * Topic-based backends: dds, rtps, shm. The policies in <nb/qos.h> are DDS
 * policies and are honoured by these backends; point-to-point RPC links
 * ignore them.
 *
 * Reader histories and wait sets belong to this spine: a history reader keeps
 * a bounded KEEP_LAST queue that nb_reader_read / nb_reader_take drain, and
 * nb_waitset_attach_reader makes a wait set observe it.
 */

#include <nb/base.h>
#include <nb/qos.h>
#include <nb/sample.h>
#include <nb/status.h>
#include <nb/waitset.h>
#include <nb/discovery_view.h>

/* ---- Object model ---- */
#define NB_TOPIC_NAME_MAX 128
#define NB_TOPIC_TYPE_MAX 128

typedef struct nb_participant nb_participant_t;
typedef struct nb_topic nb_topic_t;
typedef struct nb_writer nb_writer_t;
typedef struct nb_reader nb_reader_t;

/*
 * A participant binds the pub/sub spine to one pub/sub binding of a node.
 * `node` owns the threads and the bindings; `scheme` selects which binding to
 * use ("rtps", "dds", "shm", ...). When scheme is NULL the first binding whose
 * backend supports native topics is used. The participant does not create a
 * transport; the node must already have the binding configured.
 */
typedef struct nb_participant_options {
    nb_node_t *node;                /* required */
    const char *scheme;             /* NULL = first topic-capable binding */
    const char *name;               /* optional; diagnostics only */
    const nb_ps_qos_t *default_qos; /* NULL = provider defaults */
} nb_participant_options_t;

int nb_participant_create(const nb_participant_options_t *options, nb_participant_t **out);
void nb_participant_destroy(nb_participant_t *participant);
const char *nb_participant_name(const nb_participant_t *participant);

/*
 * Create a topic. `name` is the topic name used for matching on the wire and
 * must be non-empty, shorter than NB_TOPIC_NAME_MAX and contain no '.', so a
 * topic can never collide with a "service.method" route. `type_name` is
 * advisory today: the payload is opaque bytes, so two peers must agree on the
 * encoding themselves (a codec can be used, see codec.h). QoS on the topic is
 * the default for its endpoints; a writer or reader QoS overrides it.
 */
int nb_topic_create(nb_participant_t *participant,
                    const char *name,
                    const char *type_name,
                    const nb_ps_qos_t *qos,
                    nb_topic_t **out);
void nb_topic_destroy(nb_topic_t *topic);
const char *nb_topic_name(const nb_topic_t *topic);
const char *nb_topic_type_name(const nb_topic_t *topic);

/* ---- Writing ---- */
/* qos may be NULL to inherit the topic/default QoS. */
int nb_writer_create(nb_topic_t *topic, const nb_ps_qos_t *qos, nb_writer_t **out);
void nb_writer_destroy(nb_writer_t *writer);
/* Publish one sample. The payload is borrowed for the duration of the call. */
int nb_writer_write(nb_writer_t *writer, const nb_payload_t *payload);
/* Publish an owned payload; ownership transfers even on error. */
int nb_writer_write_owned(nb_writer_t *writer, nb_owned_payload_t *payload);

/* ---- Reading ---- */
/* Callback reader: on_data runs on the receiving provider thread and the
 * sample (including its payload) is valid only inside the callback. */
typedef void (*nb_reader_data_fn)(void *user, const nb_sample_t *sample);

/* depth is the KEEP_LAST history depth; 0 selects NB_SAMPLE_QUEUE_DEFAULT_DEPTH.
 * A callback reader passes on_data and depth 0; a history reader passes
 * on_data NULL and a non-zero depth. */
int nb_reader_create(nb_topic_t *topic,
                     const nb_ps_qos_t *qos,
                     nb_reader_data_fn on_data,
                     void *user,
                     uint32_t depth,
                     nb_reader_t **out);
void nb_reader_destroy(nb_reader_t *reader);

/* History readers only. read() returns borrowed views valid until the sample is
 * taken or the reader is destroyed; take() returns owned copies released with
 * nb_sample_release. */
size_t nb_reader_read(nb_reader_t *reader,
                      const nb_sample_filter_t *filter,
                      nb_sample_t *out,
                      size_t max);
size_t nb_reader_take(nb_reader_t *reader,
                      const nb_sample_filter_t *filter,
                      nb_sample_t *out,
                      size_t max);
/* Samples currently held by the history (0 for callback readers). */
size_t nb_reader_size(const nb_reader_t *reader);
/* Samples dropped because the history was full (0 for callback readers). */
uint64_t nb_reader_dropped(const nb_reader_t *reader);

/* ---- Wait sets over readers ---- */
/* A wait set created with nb_waitset_create can observe reader histories:
 * nb_waitset_wait returns 1 as soon as any attached reader has a matching
 * sample. Only history readers can be attached; a callback reader has no
 * history and returns NB_ERR_INVALID. Do not watch the same history through
 * both a reader and the legacy service-method attach. */
int nb_waitset_attach_reader(nb_waitset_t *waitset, nb_reader_t *reader);
int nb_waitset_detach_reader(nb_waitset_t *waitset, nb_reader_t *reader);

/* Topic-based backends; including these also makes their registration calls
 * visible. They are opt-in providers, so including them here is what a
 * pub/sub application needs in practice. */
#include <nb/dds.h>
#include <nb/rtps.h>
#include <nb/shm.h>

#endif /* NB_PUBSUB_H */
