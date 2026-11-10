#include "sample_queue.h"

#include <nb/base/buf.h>
#include <nb/base/error.h>

#include <stdlib.h>
#include <string.h>

typedef struct nb_queued_sample {
    struct nb_queued_sample *next;
    nb_buf_t *buf;
    size_t len;
    uint8_t encoding;
    nb_dds_sample_info_t info;
    int read; /* DDS SAMPLE_STATE_READ */
} nb_queued_sample_t;

struct nb_sample_queue {
    ntk_mutex *lock;
    nb_queued_sample_t *head;
    nb_queued_sample_t *tail;
    size_t count;
    size_t depth;
    uint64_t dropped;
    nb_sample_notify_fn listeners[NB_SAMPLE_QUEUE_MAX_LISTENERS];
    void *listener_user[NB_SAMPLE_QUEUE_MAX_LISTENERS];
};

static void nb_queued_sample_free(nb_queued_sample_t *sample)
{
    if (sample) {
        nb_buf_release(sample->buf);
        free(sample);
    }
}

nb_sample_queue_t *nb_sample_queue_create(uint32_t depth)
{
    nb_sample_queue_t *queue = (nb_sample_queue_t *)calloc(1, sizeof(*queue));

    if (!queue) {
        return NULL;
    }
    queue->lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    if (!queue->lock) {
        free(queue);
        return NULL;
    }
    queue->depth = depth ? depth : NB_SAMPLE_QUEUE_DEFAULT_DEPTH;
    return queue;
}

void nb_sample_queue_destroy(nb_sample_queue_t *queue)
{
    nb_queued_sample_t *sample;

    if (!queue) {
        return;
    }
    sample = queue->head;
    while (sample) {
        nb_queued_sample_t *next = sample->next;
        nb_queued_sample_free(sample);
        sample = next;
    }
    ntk_mutex_destroy(queue->lock);
    free(queue);
}

int nb_sample_queue_push(nb_sample_queue_t *queue,
                         const void *data,
                         size_t len,
                         uint8_t encoding,
                         const nb_dds_sample_info_t *info)
{
    nb_queued_sample_t *sample;
    nb_sample_notify_fn listeners[NB_SAMPLE_QUEUE_MAX_LISTENERS];
    void *listener_user[NB_SAMPLE_QUEUE_MAX_LISTENERS];
    size_t listener_count = 0;
    size_t i;

    if (!queue || (len > 0 && !data)) {
        return NB_ERR_INVALID;
    }
    sample = (nb_queued_sample_t *)calloc(1, sizeof(*sample));
    if (!sample) {
        return NB_ERR_NOMEM;
    }
    if (len > 0) {
        sample->buf = nb_buf_alloc(len);
        if (!sample->buf) {
            free(sample);
            return NB_ERR_NOMEM;
        }
        memcpy(nb_buf_data(sample->buf), data, len);
    }
    sample->len = len;
    sample->encoding = encoding;
    if (info) {
        sample->info = *info;
    }

    ntk_mutex_lock(queue->lock);
    while (queue->count >= queue->depth && queue->head) {
        nb_queued_sample_t *old = queue->head;
        queue->head = old->next;
        if (!queue->head) {
            queue->tail = NULL;
        }
        queue->count--;
        queue->dropped++;
        nb_queued_sample_free(old);
    }
    if (queue->tail) {
        queue->tail->next = sample;
    } else {
        queue->head = sample;
    }
    queue->tail = sample;
    queue->count++;
    for (i = 0; i < NB_SAMPLE_QUEUE_MAX_LISTENERS; i++) {
        if (queue->listeners[i]) {
            listeners[listener_count] = queue->listeners[i];
            listener_user[listener_count] = queue->listener_user[i];
            listener_count++;
        }
    }
    ntk_mutex_unlock(queue->lock);

    for (i = 0; i < listener_count; i++) {
        listeners[i](listener_user[i]);
    }
    return NB_OK;
}

static int nb_state_allows(nb_dds_state_mask_t mask, nb_dds_state_mask_t bits)
{
    return (mask & bits) != 0;
}

#define NB_SAMPLE_STATE_BITS (NB_DDS_STATE_SAMPLE_READ | NB_DDS_STATE_SAMPLE_NOT_READ)
#define NB_VIEW_STATE_BITS (NB_DDS_STATE_VIEW_NEW | NB_DDS_STATE_VIEW_NOT_NEW)
#define NB_INSTANCE_STATE_BITS                                                               \
    (NB_DDS_STATE_INSTANCE_ALIVE | NB_DDS_STATE_INSTANCE_DISPOSED |                          \
     NB_DDS_STATE_INSTANCE_NO_WRITERS)

static int nb_sample_matches(const nb_queued_sample_t *sample,
                             const nb_dds_sample_filter_t *filter)
{
    nb_dds_state_mask_t mask = filter ? filter->state_mask : NB_DDS_STATE_ANY;
    nb_dds_state_mask_t instance_bit;

    if (mask == 0) {
        mask = NB_DDS_STATE_ANY;
    }
    if ((mask & NB_SAMPLE_STATE_BITS) &&
        !nb_state_allows(mask,
                         sample->read ? NB_DDS_STATE_SAMPLE_READ
                                      : NB_DDS_STATE_SAMPLE_NOT_READ)) {
        return 0;
    }
    if ((mask & NB_VIEW_STATE_BITS) &&
        !nb_state_allows(mask,
                         sample->info.view_state == NB_VIEW_STATE_NEW ? NB_DDS_STATE_VIEW_NEW
                                                                      : NB_DDS_STATE_VIEW_NOT_NEW)) {
        return 0;
    }
    switch (sample->info.instance_state) {
    case NB_INSTANCE_STATE_NOT_ALIVE_DISPOSED:
        instance_bit = NB_DDS_STATE_INSTANCE_DISPOSED;
        break;
    case NB_INSTANCE_STATE_NOT_ALIVE_NO_WRITERS:
        instance_bit = NB_DDS_STATE_INSTANCE_NO_WRITERS;
        break;
    case NB_INSTANCE_STATE_ALIVE:
    default:
        instance_bit = NB_DDS_STATE_INSTANCE_ALIVE;
        break;
    }
    if ((mask & NB_INSTANCE_STATE_BITS) && !nb_state_allows(mask, instance_bit)) {
        return 0;
    }
    if (filter && filter->instance) {
        if (!sample->info.has_instance_handle ||
            memcmp(sample->info.instance_handle, filter->instance, NB_INSTANCE_HANDLE_SIZE) != 0) {
            return 0;
        }
    }
    return 1;
}

static void nb_sample_fill(nb_dds_sample_t *out, const nb_queued_sample_t *sample, int take)
{
    memset(out, 0, sizeof(*out));
    out->data.data = sample->buf ? nb_buf_const_data(sample->buf) : NULL;
    out->data.len = sample->len;
    out->data.encoding = sample->encoding;
    out->info = sample->info;
    out->owner = take ? sample->buf : NULL;
}

size_t nb_sample_queue_read(nb_sample_queue_t *queue,
                            const nb_dds_sample_filter_t *filter,
                            nb_dds_sample_t *out,
                            size_t max)
{
    nb_queued_sample_t *sample;
    size_t count = 0;

    if (!queue || !out || max == 0) {
        return 0;
    }
    ntk_mutex_lock(queue->lock);
    for (sample = queue->head; sample && count < max; sample = sample->next) {
        if (!nb_sample_matches(sample, filter)) {
            continue;
        }
        nb_sample_fill(&out[count], sample, 0);
        sample->read = 1;
        count++;
    }
    ntk_mutex_unlock(queue->lock);
    return count;
}

size_t nb_sample_queue_take(nb_sample_queue_t *queue,
                            const nb_dds_sample_filter_t *filter,
                            nb_dds_sample_t *out,
                            size_t max)
{
    nb_queued_sample_t *prev = NULL;
    nb_queued_sample_t *sample;
    size_t count = 0;

    if (!queue || !out || max == 0) {
        return 0;
    }
    ntk_mutex_lock(queue->lock);
    sample = queue->head;
    while (sample && count < max) {
        nb_queued_sample_t *next = sample->next;

        if (!nb_sample_matches(sample, filter)) {
            prev = sample;
            sample = next;
            continue;
        }
        if (prev) {
            prev->next = next;
        } else {
            queue->head = next;
        }
        if (queue->tail == sample) {
            queue->tail = prev;
        }
        queue->count--;
        nb_sample_fill(&out[count], sample, 1);
        sample->buf = NULL; /* ownership moved to the caller */
        nb_queued_sample_free(sample);
        count++;
        sample = next;
    }
    ntk_mutex_unlock(queue->lock);
    return count;
}

size_t nb_sample_queue_size(nb_sample_queue_t *queue)
{
    size_t size;

    if (!queue) {
        return 0;
    }
    ntk_mutex_lock(queue->lock);
    size = queue->count;
    ntk_mutex_unlock(queue->lock);
    return size;
}

uint64_t nb_sample_queue_dropped(nb_sample_queue_t *queue)
{
    uint64_t dropped;

    if (!queue) {
        return 0;
    }
    ntk_mutex_lock(queue->lock);
    dropped = queue->dropped;
    ntk_mutex_unlock(queue->lock);
    return dropped;
}

void nb_sample_queue_set_depth(nb_sample_queue_t *queue, uint32_t depth)
{
    if (!queue) {
        return;
    }
    ntk_mutex_lock(queue->lock);
    queue->depth = depth ? depth : NB_SAMPLE_QUEUE_DEFAULT_DEPTH;
    ntk_mutex_unlock(queue->lock);
}

size_t nb_sample_queue_match_count(nb_sample_queue_t *queue,
                                   const nb_dds_sample_filter_t *filter)
{
    nb_queued_sample_t *sample;
    size_t count = 0;

    if (!queue) {
        return 0;
    }
    ntk_mutex_lock(queue->lock);
    for (sample = queue->head; sample; sample = sample->next) {
        if (nb_sample_matches(sample, filter)) {
            count++;
        }
    }
    ntk_mutex_unlock(queue->lock);
    return count;
}

int nb_sample_queue_add_listener(nb_sample_queue_t *queue, nb_sample_notify_fn fn, void *user)
{
    size_t i;
    int rc = NB_ERR_AGAIN;

    if (!queue || !fn) {
        return NB_ERR_INVALID;
    }
    ntk_mutex_lock(queue->lock);
    for (i = 0; i < NB_SAMPLE_QUEUE_MAX_LISTENERS; i++) {
        if (queue->listeners[i] == fn && queue->listener_user[i] == user) {
            rc = NB_OK;
            break;
        }
        if (!queue->listeners[i]) {
            queue->listeners[i] = fn;
            queue->listener_user[i] = user;
            rc = NB_OK;
            break;
        }
    }
    ntk_mutex_unlock(queue->lock);
    return rc;
}

void nb_sample_queue_remove_listener(nb_sample_queue_t *queue,
                                     nb_sample_notify_fn fn,
                                     void *user)
{
    size_t i;

    if (!queue || !fn) {
        return;
    }
    ntk_mutex_lock(queue->lock);
    for (i = 0; i < NB_SAMPLE_QUEUE_MAX_LISTENERS; i++) {
        if (queue->listeners[i] == fn && queue->listener_user[i] == user) {
            queue->listeners[i] = NULL;
            queue->listener_user[i] = NULL;
            break;
        }
    }
    ntk_mutex_unlock(queue->lock);
}
