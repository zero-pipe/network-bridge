#include "rtps_api_internal.h"

#include <stdlib.h>
#include <string.h>

/*
 * Wait sets are participant-scoped. The semaphore carries one permit per
 * enqueue on any attached reader; the wait first scans the histories so a
 * permit that arrived before the call is not lost, and only then blocks.
 */

static void waitset_unlink_locked(struct rtps_waitset *waitset)
{
    struct rtps_waitset **slot = &waitset->participant->waitsets;

    while (*slot) {
        if (*slot == waitset) {
            *slot = waitset->next;
            break;
        }
        slot = &(*slot)->next;
    }
}

static void waitset_drop_links_locked(struct rtps_waitset *waitset)
{
    while (waitset->links) {
        rtps_ws_link_t *link = waitset->links;
        rtps_reader_history_t *history = link->reader->history;

        waitset->links = link->ws_next;
        if (history) {
            rtps_ws_link_t **slot;

            ntk_mutex_lock(history->lock);
            slot = &history->listeners;
            while (*slot) {
                if (*slot == link) {
                    *slot = link->hist_next;
                    break;
                }
                slot = &(*slot)->hist_next;
            }
            ntk_mutex_unlock(history->lock);
        }
        free(link);
    }
}

int rtps_waitset_create(rtps_participant_t *participant, rtps_waitset_t **out)
{
    struct rtps_waitset *waitset;

    if (!participant || !out) {
        return RTPS_ERR_INVALID;
    }
    *out = NULL;
    waitset = (struct rtps_waitset *)calloc(1, sizeof(*waitset));
    if (!waitset) {
        return RTPS_ERR_NOMEM;
    }
    waitset->participant = participant;
    waitset->sem = ntk_sem_create(0);
    if (!waitset->sem) {
        free(waitset);
        return RTPS_ERR_NOMEM;
    }
    ntk_mutex_lock(participant->lock);
    waitset->next = participant->waitsets;
    participant->waitsets = waitset;
    ntk_mutex_unlock(participant->lock);
    *out = waitset;
    return RTPS_OK;
}

void rtps_waitset_destroy(rtps_waitset_t *waitset)
{
    if (!waitset) {
        return;
    }
    ntk_mutex_lock(waitset->participant->lock);
    waitset_unlink_locked(waitset);
    waitset_drop_links_locked(waitset);
    ntk_mutex_unlock(waitset->participant->lock);
    ntk_sem_destroy(waitset->sem);
    free(waitset);
}

int rtps_waitset_attach(rtps_waitset_t *waitset,
                        rtps_reader_t *reader,
                        const rtps_sample_filter_t *filter)
{
    struct rtps_participant *participant;
    rtps_ws_link_t *link;
    rtps_ws_link_t **slot;
    int rc = RTPS_OK;

    if (!waitset || !reader || waitset->participant != reader->participant) {
        return RTPS_ERR_INVALID;
    }
    participant = waitset->participant;
    ntk_mutex_lock(participant->lock);
    /* Attaching implies a history: read/take and wait need somewhere to look. */
    if (!reader->history) {
        reader->history = rtps_history_create(1);
        if (!reader->history) {
            ntk_mutex_unlock(participant->lock);
            return RTPS_ERR_NOMEM;
        }
    }
    for (slot = &waitset->links; *slot; slot = &(*slot)->ws_next) {
        if ((*slot)->reader == reader) {
            if (filter) {
                (*slot)->filter = *filter;
            } else {
                rtps_sample_filter_init(&(*slot)->filter);
            }
            ntk_mutex_unlock(participant->lock);
            return RTPS_OK;
        }
    }
    link = (rtps_ws_link_t *)calloc(1, sizeof(*link));
    if (!link) {
        rc = RTPS_ERR_NOMEM;
    } else {
        link->waitset = waitset;
        link->reader = reader;
        if (filter) {
            link->filter = *filter;
        } else {
            rtps_sample_filter_init(&link->filter);
        }
        link->ws_next = waitset->links;
        waitset->links = link;
        ntk_mutex_lock(reader->history->lock);
        link->hist_next = reader->history->listeners;
        reader->history->listeners = link;
        ntk_mutex_unlock(reader->history->lock);
    }
    ntk_mutex_unlock(participant->lock);
    return rc;
}

int rtps_waitset_detach(rtps_waitset_t *waitset, rtps_reader_t *reader)
{
    rtps_ws_link_t **slot;
    int found = 0;

    if (!waitset || !reader) {
        return RTPS_ERR_INVALID;
    }
    ntk_mutex_lock(waitset->participant->lock);
    slot = &waitset->links;
    while (*slot) {
        rtps_ws_link_t *link = *slot;

        if (link->reader == reader) {
            rtps_reader_history_t *history = reader->history;

            *slot = link->ws_next;
            if (history) {
                rtps_ws_link_t **hist_slot;

                ntk_mutex_lock(history->lock);
                hist_slot = &history->listeners;
                while (*hist_slot) {
                    if (*hist_slot == link) {
                        *hist_slot = link->hist_next;
                        break;
                    }
                    hist_slot = &(*hist_slot)->hist_next;
                }
                ntk_mutex_unlock(history->lock);
            }
            free(link);
            found = 1;
            break;
        }
        slot = &(*slot)->ws_next;
    }
    ntk_mutex_unlock(waitset->participant->lock);
    return found ? RTPS_OK : RTPS_ERR_NOTFOUND;
}

int rtps_waitset_wait(rtps_waitset_t *waitset,
                      rtps_reader_t **out,
                      size_t max,
                      int32_t timeout_ms)
{
    uint64_t start_ms;
    uint32_t sem_timeout;

    if (!waitset || !out || max == 0) {
        return RTPS_ERR_INVALID;
    }
    start_ms = ntk_wall_ms();
    for (;;) {
        size_t count = 0;
        rtps_ws_link_t *link;
        ntk_err_t rc;
        uint64_t now;

        ntk_mutex_lock(waitset->participant->lock);
        for (link = waitset->links; link && count < max; link = link->ws_next) {
            if (link->reader->history &&
                rtps_history_match_count(link->reader->history, &link->filter) > 0) {
                out[count++] = link->reader;
            }
        }
        ntk_mutex_unlock(waitset->participant->lock);
        if (count > 0) {
            return (int)count;
        }

        now = ntk_wall_ms();
        if (timeout_ms < 0) {
            rc = ntk_sem_wait(waitset->sem);
        } else {
            uint64_t elapsed = now >= start_ms ? now - start_ms : 0;

            if (elapsed >= (uint64_t)timeout_ms) {
                return 0;
            }
            sem_timeout = (uint32_t)((uint64_t)timeout_ms - elapsed);
            rc = ntk_sem_timedwait(waitset->sem, sem_timeout);
        }
        if (rc != NTK_OK) {
            /* Timeout (or a destroyed semaphore): report "not ready". */
            return 0;
        }
    }
}
