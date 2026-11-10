#ifndef RTPS_API_INTERNAL_H
#define RTPS_API_INTERNAL_H

/*
 * Private implementation of the rtps_* interface layer.
 *
 * The engine headers under src/rtps are included here and nowhere in the
 * public headers: everything a caller sees is opaque or plain data.
 *
 * Locking: participant->lock guards the API object graph (topics, writers,
 * readers, waitsets and the links between them). Each reader history has its
 * own lock for the sample list. The order is always
 *
 *     participant->lock  ->  history->lock
 *
 * and never the reverse. The engine data callback pushes into a history with
 * only the history lock held; it calls the application callback after
 * releasing every lock.
 */

#include <rtps/rtps.h>

#include <nb/base/error.h>
#include <nb/base/sample_info.h>

#include <ntk.h>

#include "rtps_participant.h"
#include "rtps_types.h"

typedef struct rtps_hist_sample {
    struct rtps_hist_sample *next;
    uint8_t *data;
    size_t size;
    rtps_sample_info_t info;
} rtps_hist_sample_t;

typedef struct rtps_ws_link {
    struct rtps_ws_link *ws_next;   /* rtps_waitset.links */
    struct rtps_ws_link *hist_next; /* rtps_history.listeners */
    struct rtps_waitset *waitset;
    struct rtps_reader *reader;
    rtps_sample_filter_t filter;
} rtps_ws_link_t;

typedef struct rtps_reader_history {
    ntk_mutex *lock;
    rtps_hist_sample_t *head;
    rtps_hist_sample_t *tail;
    size_t count;
    size_t depth;
    uint64_t dropped;
    rtps_ws_link_t *listeners;
} rtps_reader_history_t;

struct rtps_participant {
    nb_rtps_participant_t *engine;
    ntk_mutex *lock;
    struct rtps_topic *topics;
    struct rtps_publisher *publishers;
    struct rtps_subscriber *subscribers;
    struct rtps_writer *writers; /* every writer, including published ones */
    struct rtps_reader *readers;
    struct rtps_waitset *waitsets;
    rtps_status_callback_t status_cb;
    void *status_user;
    int started;
    int destroyed;
    char name[RTPS_TOPIC_NAME_MAX];
};

struct rtps_topic {
    struct rtps_topic *next;
    struct rtps_participant *participant;
    char name[RTPS_TOPIC_NAME_MAX];
    char type_name[RTPS_TOPIC_TYPE_MAX];
    rtps_qos_t qos;
    size_t writer_count;
    size_t reader_count;
};

struct rtps_publisher {
    struct rtps_publisher *next;
    struct rtps_participant *participant;
    rtps_qos_t qos;
    struct rtps_writer *writers;
    size_t writer_count;
};

struct rtps_subscriber {
    struct rtps_subscriber *next;
    struct rtps_participant *participant;
    rtps_qos_t qos;
    struct rtps_reader *readers;
    size_t reader_count;
};

struct rtps_writer {
    struct rtps_writer *next;
    struct rtps_writer *pub_next;
    struct rtps_topic *topic;
    struct rtps_participant *participant;
    struct rtps_publisher *publisher; /* NULL when created directly */
    nb_rtps_guid_t guid;
    rtps_qos_t qos;
};

struct rtps_reader {
    struct rtps_reader *next;
    struct rtps_reader *sub_next;
    struct rtps_topic *topic;
    struct rtps_participant *participant;
    struct rtps_subscriber *subscriber; /* NULL when created directly */
    nb_rtps_guid_t guid;
    rtps_qos_t qos;
    rtps_data_callback_t callback;
    void *callback_user;
    rtps_reader_history_t *history;
    /* Data callbacks in flight; the reader is freed when it drops to zero
     * after destroy. Guarded by participant->lock. */
    uint32_t refs;
    int destroyed;
};

struct rtps_waitset {
    struct rtps_waitset *next;
    struct rtps_participant *participant;
    ntk_sem *sem;
    rtps_ws_link_t *links; /* guarded by participant->lock */
};

/* ---- helpers shared by the translation units ------------------------- */

int rtps_api_error(int nb_status);
int rtps_api_size_ok(uint32_t struct_size, size_t expected);
void rtps_api_copy(char *dst, size_t cap, const char *src);

/* Field-wise merge: override wins where its present bit is set. */
void rtps_api_qos_merge(const rtps_qos_t *base,
                        const rtps_qos_t *override,
                        rtps_qos_t *out);

/* Resolved endpoint QoS -> engine endpoint description. */
void rtps_api_fill_endpoint(const struct rtps_topic *topic,
                            const rtps_qos_t *qos,
                            nb_rtps_endpoint_info_t *info);

/* Engine callback payload -> public sample view. */
void rtps_api_fill_sample(const nb_rtps_dds_message_t *message,
                          rtps_sample_t *out);

/* History. All functions take history->lock themselves. */
rtps_reader_history_t *rtps_history_create(size_t depth);
void rtps_history_destroy(rtps_reader_history_t *history);
void rtps_history_push(rtps_reader_history_t *history, const rtps_sample_t *sample);
/* take == 0: borrowed views; take == 1: ownership transferred to the caller. */
size_t rtps_history_read(rtps_reader_history_t *history,
                         const rtps_sample_filter_t *filter,
                         rtps_sample_t *out,
                         size_t max,
                         int take);
size_t rtps_history_match_count(rtps_reader_history_t *history,
                                const rtps_sample_filter_t *filter);
size_t rtps_history_size(rtps_reader_history_t *history);
/* Resize the KEEP_LAST depth; shrinking drops the oldest samples. */
void rtps_history_set_depth(rtps_reader_history_t *history, size_t depth);

/* Wait set link bookkeeping; called with participant->lock held. */
void rtps_waitset_remove_reader_locked(struct rtps_participant *participant,
                                       struct rtps_reader *reader);

/* Reader lifetime. */
void rtps_reader_unref_locked(struct rtps_reader *reader);

/* Common creators used by the direct and the publisher/subscriber forms. */
int rtps_writer_create_internal(struct rtps_participant *participant,
                                struct rtps_publisher *publisher,
                                struct rtps_topic *topic,
                                const rtps_qos_t *qos,
                                struct rtps_writer **out);
int rtps_reader_create_internal(struct rtps_participant *participant,
                                struct rtps_subscriber *subscriber,
                                struct rtps_topic *topic,
                                const rtps_qos_t *qos,
                                struct rtps_reader **out);

#endif /* RTPS_API_INTERNAL_H */
