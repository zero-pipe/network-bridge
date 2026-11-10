#ifndef NB_DDS_INTERNAL_H
#define NB_DDS_INTERNAL_H

/* Internal composition root for the DDS spine. */

#include <nb/dds.h>

#include <ntk.h>

#include "sample_queue.h"

struct nb_dds_participant {
    char name[NB_NAME_MAX];
    char prefix[NB_DDS_TOPIC_NAME_MAX];
    char partition[NB_DDS_QOS_PARTITION_MAX];
    nb_dds_qos_t default_qos;
    nb_dds_backend_t backend_id;
    const nb_dds_backend_ops_t *ops;
    void *backend;
    ntk_mutex *lock; /* protects topics/status callback/backend lifetime */
    nb_dds_topic_t *topics;
    nb_dds_status_callback_t status_cb;
    void *status_user;
};

struct nb_dds_topic {
    nb_dds_participant_t *participant;
    char name[NB_DDS_TOPIC_NAME_MAX];
    char wire_name[NB_DDS_TOPIC_NAME_MAX];
    char type_name[NB_DDS_TOPIC_TYPE_MAX];
    nb_dds_qos_t qos;
    void *backend;
    unsigned writer_count;
    unsigned reader_count;
    struct nb_dds_reader *readers; /* local readers for intra-process delivery */
    struct nb_dds_topic *next;
};

struct nb_dds_writer {
    nb_dds_topic_t *topic;
    nb_dds_qos_t qos;
    void *backend;
    int destroyed;
};

struct nb_dds_reader {
    nb_dds_topic_t *topic;
    nb_dds_qos_t qos;
    void *backend;
    nb_dds_reader_data_fn on_data;
    void *user;
    nb_sample_queue_t *history; /* NULL for callback readers */
    unsigned refs;
    int removed;
    struct nb_dds_reader *next; /* local reader list of the topic */
};

typedef struct nb_dds_waitset_link {
    nb_dds_reader_t *reader;
    nb_sample_queue_t *queue;
    struct nb_dds_waitset_link *next;
} nb_dds_waitset_link_t;

struct nb_dds_waitset {
    nb_dds_participant_t *participant;
    ntk_mutex *lock;
    ntk_sem *sem;
    nb_dds_waitset_link_t *links;
};

/* reader.c */
void nb_dds_reader_deliver(nb_dds_reader_t *reader,
                           const void *data,
                           size_t len,
                           uint32_t encoding,
                           const nb_dds_sample_info_t *info);
nb_sample_queue_t *nb_dds_reader_history(const nb_dds_reader_t *reader);
/* Intra-process delivery: snapshot live readers (each ref is held) and release
 * them after delivery. */
size_t nb_dds_reader_snapshot(nb_dds_topic_t *topic, nb_dds_reader_t **out, size_t max);
void nb_dds_reader_unref(nb_dds_reader_t *reader);

#endif /* NB_DDS_INTERNAL_H */
