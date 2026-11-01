#include "rtps_api_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- filters ------------------------------------------------------------ */

static int sample_matches(const rtps_sample_info_t *info,
                          const rtps_sample_filter_t *filter)
{
    if (!filter) {
        return 1;
    }
    if (filter->sample_state_mask != RTPS_STATE_MASK_ANY) {
        uint32_t bit = info->sample_state == RTPS_SAMPLE_STATE_READ
                           ? RTPS_SAMPLE_STATE_READ_BIT
                           : RTPS_SAMPLE_STATE_NOT_READ_BIT;

        if ((filter->sample_state_mask & bit) == 0) {
            return 0;
        }
    }
    if (filter->view_state_mask != RTPS_STATE_MASK_ANY) {
        uint32_t bit = info->view_state == RTPS_VIEW_STATE_NEW
                           ? RTPS_VIEW_STATE_NEW_BIT
                           : RTPS_VIEW_STATE_NOT_NEW_BIT;

        if ((filter->view_state_mask & bit) == 0) {
            return 0;
        }
    }
    if (filter->instance_state_mask != RTPS_STATE_MASK_ANY) {
        uint32_t bit;

        switch (info->instance_state) {
        case RTPS_INSTANCE_NOT_ALIVE_DISPOSED:
            bit = RTPS_INSTANCE_STATE_DISPOSED_BIT;
            break;
        case RTPS_INSTANCE_NOT_ALIVE_NO_WRITERS:
            bit = RTPS_INSTANCE_STATE_NO_WRITERS_BIT;
            break;
        default:
            bit = RTPS_INSTANCE_STATE_ALIVE_BIT;
            break;
        }
        if ((filter->instance_state_mask & bit) == 0) {
            return 0;
        }
    }
    if (filter->has_instance_handle) {
        if (!info->has_instance_handle ||
            memcmp(info->instance_handle.bytes,
                   filter->instance_handle.bytes,
                   RTPS_INSTANCE_HANDLE_SIZE) != 0) {
            return 0;
        }
    }
    return 1;
}

/* ---- history ------------------------------------------------------------ */

rtps_reader_history_t *rtps_history_create(size_t depth)
{
    rtps_reader_history_t *history;

    if (depth == 0) {
        depth = 1;
    }
    history = (rtps_reader_history_t *)calloc(1, sizeof(*history));
    if (!history) {
        return NULL;
    }
    history->lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    if (!history->lock) {
        free(history);
        return NULL;
    }
    history->depth = depth;
    return history;
}

static void history_sample_free(rtps_hist_sample_t *sample)
{
    if (!sample) {
        return;
    }
    free(sample->data);
    free(sample);
}

void rtps_history_destroy(rtps_reader_history_t *history)
{
    rtps_hist_sample_t *sample;

    if (!history) {
        return;
    }
    sample = history->head;
    while (sample) {
        rtps_hist_sample_t *next = sample->next;

        history_sample_free(sample);
        sample = next;
    }
    if (history->lock) {
        ntk_mutex_destroy(history->lock);
    }
    free(history);
}

void rtps_history_push(rtps_reader_history_t *history, const rtps_sample_t *sample)
{
    rtps_hist_sample_t *node;
    rtps_ws_link_t *link;

    if (!history || !sample) {
        return;
    }
    node = (rtps_hist_sample_t *)calloc(1, sizeof(*node));
    if (!node) {
        return;
    }
    if (sample->size > 0) {
        node->data = (uint8_t *)malloc(sample->size);
        if (!node->data) {
            free(node);
            return;
        }
        memcpy(node->data, sample->data, sample->size);
    }
    node->size = sample->size;
    node->info = sample->info;
    node->info.sample_state = RTPS_SAMPLE_STATE_NOT_READ;

    ntk_mutex_lock(history->lock);
    if (history->count >= history->depth) {
        rtps_hist_sample_t *old = history->head;

        if (old) {
            history->head = old->next;
            if (!history->head) {
                history->tail = NULL;
            }
            history->count--;
            history->dropped++;
            history_sample_free(old);
        }
    }
    if (history->tail) {
        history->tail->next = node;
    } else {
        history->head = node;
    }
    history->tail = node;
    history->count++;
    /* Wake every wait set attached to this history. Posting a semaphore never
     * blocks, so it is safe under the history lock. */
    for (link = history->listeners; link; link = link->hist_next) {
        if (link->waitset && link->waitset->sem) {
            (void)ntk_sem_post(link->waitset->sem, 1);
        }
    }
    ntk_mutex_unlock(history->lock);
}

void rtps_history_set_depth(rtps_reader_history_t *history, size_t depth)
{
    if (!history || depth == 0) {
        return;
    }
    ntk_mutex_lock(history->lock);
    history->depth = depth;
    while (history->count > history->depth && history->head) {
        rtps_hist_sample_t *old = history->head;

        history->head = old->next;
        if (!history->head) {
            history->tail = NULL;
        }
        history->count--;
        history->dropped++;
        history_sample_free(old);
    }
    ntk_mutex_unlock(history->lock);
}

size_t rtps_history_size(rtps_reader_history_t *history)
{
    size_t count;

    if (!history) {
        return 0;
    }
    ntk_mutex_lock(history->lock);
    count = history->count;
    ntk_mutex_unlock(history->lock);
    return count;
}

size_t rtps_history_read(rtps_reader_history_t *history,
                         const rtps_sample_filter_t *filter,
                         rtps_sample_t *out,
                         size_t max,
                         int take)
{
    rtps_hist_sample_t *sample;
    rtps_hist_sample_t *prev = NULL;
    size_t written = 0;

    if (!history) {
        return 0;
    }
    ntk_mutex_lock(history->lock);
    sample = history->head;
    while (sample) {
        rtps_hist_sample_t *next = sample->next;

        if (!sample_matches(&sample->info, filter)) {
            prev = sample;
            sample = next;
            continue;
        }
        if (!out || written == max) {
            /* match_count path, or a read without room. */
            if (!out) {
                written++;
                prev = sample;
                sample = next;
                continue;
            }
            break;
        }
        out[written].data = sample->data;
        out[written].size = sample->size;
        out[written].info = sample->info;
        out[written].info.sample_state = RTPS_SAMPLE_STATE_READ;
        written++;
        if (take) {
            if (prev) {
                prev->next = next;
            } else {
                history->head = next;
            }
            if (history->tail == sample) {
                history->tail = prev;
            }
            history->count--;
            sample->data = NULL; /* ownership moved to the caller */
            history_sample_free(sample);
        } else {
            sample->info.sample_state = RTPS_SAMPLE_STATE_READ;
            prev = sample;
        }
        sample = next;
    }
    ntk_mutex_unlock(history->lock);
    return written;
}

size_t rtps_history_match_count(rtps_reader_history_t *history,
                                const rtps_sample_filter_t *filter)
{
    return rtps_history_read(history, filter, NULL, 0, 0);
}

void rtps_sample_release(rtps_sample_t *samples, size_t count)
{
    size_t i;

    if (!samples) {
        return;
    }
    for (i = 0; i < count; i++) {
        free((void *)samples[i].data);
        samples[i].data = NULL;
        samples[i].size = 0;
    }
}
