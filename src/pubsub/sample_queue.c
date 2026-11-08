#include "sample_queue.h"

#include <nb/error.h>

#include <stdlib.h>
#include <string.h>

typedef struct nb_queued_sample {
    struct nb_queued_sample *next;
    uint8_t *data;
    size_t len;
    uint8_t encoding;
    nb_sample_info_t info;
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
        free(sample->data);
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

int nb_sample_queue_push(nb_sample_queue_t *queue, const nb_message_t *msg)
{
    nb_queued_sample_t *sample;
    nb_sample_notify_fn listeners[NB_SAMPLE_QUEUE_MAX_LISTENERS];
    void *listener_user[NB_SAMPLE_QUEUE_MAX_LISTENERS];
    size_t listener_count = 0;
    size_t i;
    size_t len;

    if (!queue || !msg) {
        return NB_ERR_INVALID;
    }
    len = msg->payload.len;
    sample = (nb_queued_sample_t *)calloc(1, sizeof(*sample));
    if (!sample) {
        return NB_ERR_NOMEM;
    }
    if (len > 0) {
        if (!msg->payload.data) {
            free(sample);
            return NB_ERR_INVALID;
        }
        sample->data = (uint8_t *)malloc(len);
        if (!sample->data) {
            free(sample);
            return NB_ERR_NOMEM;
        }
        memcpy(sample->data, msg->payload.data, len);
    }
    sample->len = len;
    sample->encoding = msg->payload.encoding;
    sample->info = msg->sample;

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

static int nb_state_allows(nb_state_mask_t mask, nb_state_mask_t bits)
{
    return (mask & bits) != 0;
}

#define NB_SAMPLE_STATE_BITS (NB_STATE_SAMPLE_READ | NB_STATE_SAMPLE_NOT_READ)
#define NB_VIEW_STATE_BITS (NB_STATE_VIEW_NEW | NB_STATE_VIEW_NOT_NEW)
#define NB_INSTANCE_STATE_BITS                                                               \
    (NB_STATE_INSTANCE_ALIVE | NB_STATE_INSTANCE_DISPOSED | NB_STATE_INSTANCE_NO_WRITERS)

static int nb_sample_matches(const nb_queued_sample_t *sample, const nb_sample_filter_t *filter)
{
    nb_state_mask_t mask = filter ? filter->state_mask : NB_STATE_ANY;
    nb_state_mask_t instance_bit;

    if (mask == 0) {
        mask = NB_STATE_ANY;
    }
    /*
     * DDS StateMask combines three independent masks: a dimension only
     * constrains the result when the caller selected one of its bits.
     */
    if ((mask & NB_SAMPLE_STATE_BITS) &&
        !nb_state_allows(mask,
                         sample->read ? NB_STATE_SAMPLE_READ : NB_STATE_SAMPLE_NOT_READ)) {
        return 0;
    }
    if ((mask & NB_VIEW_STATE_BITS) &&
        !nb_state_allows(mask,
                         sample->info.view_state == NB_VIEW_STATE_NEW ? NB_STATE_VIEW_NEW
                                                                      : NB_STATE_VIEW_NOT_NEW)) {
        return 0;
    }
    switch (sample->info.instance_state) {
    case NB_INSTANCE_STATE_NOT_ALIVE_DISPOSED:
        instance_bit = NB_STATE_INSTANCE_DISPOSED;
        break;
    case NB_INSTANCE_STATE_NOT_ALIVE_NO_WRITERS:
        instance_bit = NB_STATE_INSTANCE_NO_WRITERS;
        break;
    case NB_INSTANCE_STATE_ALIVE:
    default:
        instance_bit = NB_STATE_INSTANCE_ALIVE;
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

size_t nb_sample_queue_read(nb_sample_queue_t *queue,
                            const nb_sample_filter_t *filter,
                            nb_sample_t *out,
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
        nb_message_view_init(&out[count].data);
        if (nb_message_view_set_borrowed(&out[count].data, sample->data, sample->len) != NB_OK) {
            continue;
        }
        out[count].data.encoding = sample->encoding;
        out[count].info = sample->info;
        sample->read = 1;
        count++;
    }
    ntk_mutex_unlock(queue->lock);
    return count;
}

size_t nb_sample_queue_take(nb_sample_queue_t *queue,
                            const nb_sample_filter_t *filter,
                            nb_sample_t *out,
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
        nb_message_view_init(&out[count].data);
        if (nb_message_view_set_data(&out[count].data, sample->data, sample->len) == NB_OK) {
            out[count].data.encoding = sample->encoding;
            out[count].info = sample->info;
            count++;
        }
        nb_queued_sample_free(sample);
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

size_t nb_sample_queue_match_count(nb_sample_queue_t *queue, const nb_sample_filter_t *filter)
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
