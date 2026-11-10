/* DDS spine: wait sets over reader histories. */
#include "dds_internal.h"

#include <nb/base/error.h>

#include <stdlib.h>

static void nb_dds_waitset_notify(void *user)
{
    nb_dds_waitset_t *waitset = (nb_dds_waitset_t *)user;
    (void)ntk_sem_post(waitset->sem, 1);
}

int nb_dds_waitset_create(nb_dds_participant_t *participant, nb_dds_waitset_t **out)
{
    nb_dds_waitset_t *waitset;

    if (!participant || !out) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    waitset = (nb_dds_waitset_t *)calloc(1, sizeof(*waitset));
    if (!waitset) {
        return NB_ERR_NOMEM;
    }
    waitset->participant = participant;
    waitset->lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    waitset->sem = ntk_sem_create(0);
    if (!waitset->lock || !waitset->sem) {
        ntk_mutex_destroy(waitset->lock);
        ntk_sem_destroy(waitset->sem);
        free(waitset);
        return NB_ERR_NOMEM;
    }
    *out = waitset;
    return NB_OK;
}

static void nb_dds_waitset_detach_link(nb_dds_waitset_t *waitset, nb_dds_waitset_link_t *link)
{
    if (link->queue) {
        nb_sample_queue_remove_listener(link->queue, nb_dds_waitset_notify, waitset);
    }
    free(link);
}

void nb_dds_waitset_destroy(nb_dds_waitset_t *waitset)
{
    nb_dds_waitset_link_t *link;

    if (!waitset) {
        return;
    }
    ntk_mutex_lock(waitset->lock);
    link = waitset->links;
    waitset->links = NULL;
    ntk_mutex_unlock(waitset->lock);
    while (link) {
        nb_dds_waitset_link_t *next = link->next;
        nb_dds_waitset_detach_link(waitset, link);
        link = next;
    }
    ntk_mutex_destroy(waitset->lock);
    ntk_sem_destroy(waitset->sem);
    free(waitset);
}

static nb_dds_waitset_link_t *nb_dds_waitset_find_locked(nb_dds_waitset_t *waitset,
                                                         nb_dds_reader_t *reader)
{
    nb_dds_waitset_link_t *link;

    for (link = waitset->links; link; link = link->next) {
        if (link->reader == reader) {
            return link;
        }
    }
    return NULL;
}

int nb_dds_waitset_attach_reader(nb_dds_waitset_t *waitset, nb_dds_reader_t *reader)
{
    nb_dds_waitset_link_t *link;
    nb_sample_queue_t *queue;

    if (!waitset || !reader) {
        return NB_ERR_INVALID;
    }
    queue = nb_dds_reader_history(reader);
    if (!queue) {
        return NB_ERR_INVALID; /* callback reader */
    }
    link = (nb_dds_waitset_link_t *)calloc(1, sizeof(*link));
    if (!link) {
        return NB_ERR_NOMEM;
    }
    link->reader = reader;
    link->queue = queue;
    if (nb_sample_queue_add_listener(queue, nb_dds_waitset_notify, waitset) != NB_OK) {
        free(link);
        return NB_ERR_AGAIN;
    }
    ntk_mutex_lock(waitset->lock);
    if (nb_dds_waitset_find_locked(waitset, reader)) {
        ntk_mutex_unlock(waitset->lock);
        nb_sample_queue_remove_listener(queue, nb_dds_waitset_notify, waitset);
        free(link);
        return NB_OK;
    }
    link->next = waitset->links;
    waitset->links = link;
    ntk_mutex_unlock(waitset->lock);
    return NB_OK;
}

int nb_dds_waitset_detach_reader(nb_dds_waitset_t *waitset, nb_dds_reader_t *reader)
{
    nb_dds_waitset_link_t *link;
    nb_dds_waitset_link_t *prev = NULL;

    if (!waitset || !reader) {
        return NB_ERR_INVALID;
    }
    ntk_mutex_lock(waitset->lock);
    for (link = waitset->links; link; prev = link, link = link->next) {
        if (link->reader == reader) {
            if (prev) {
                prev->next = link->next;
            } else {
                waitset->links = link->next;
            }
            ntk_mutex_unlock(waitset->lock);
            nb_dds_waitset_detach_link(waitset, link);
            return NB_OK;
        }
    }
    ntk_mutex_unlock(waitset->lock);
    return NB_ERR_NOTFOUND;
}

size_t nb_dds_waitset_ready(nb_dds_waitset_t *waitset, const nb_dds_sample_filter_t *filter)
{
    nb_dds_waitset_link_t *link;
    size_t ready = 0;

    if (!waitset) {
        return 0;
    }
    ntk_mutex_lock(waitset->lock);
    for (link = waitset->links; link; link = link->next) {
        if (link->queue && nb_sample_queue_match_count(link->queue, filter) > 0) {
            ready++;
        }
    }
    ntk_mutex_unlock(waitset->lock);
    return ready;
}

int nb_dds_waitset_wait(nb_dds_waitset_t *waitset,
                        const nb_dds_sample_filter_t *filter,
                        uint32_t timeout_ms)
{
    uint64_t deadline;

    if (!waitset) {
        return NB_ERR_INVALID;
    }
    if (nb_dds_waitset_ready(waitset, filter) > 0) {
        return 1;
    }
    if (timeout_ms == 0) {
        return 0;
    }
    deadline = ntk_monotonic_ms() + (uint64_t)timeout_ms;
    for (;;) {
        uint64_t now;
        uint64_t slice;

        if (timeout_ms == NB_DDS_WAITSET_INFINITE) {
            (void)ntk_sem_wait(waitset->sem);
        } else {
            now = ntk_monotonic_ms();
            if (now >= deadline) {
                return 0;
            }
            slice = deadline - now;
            if (slice > 1000) {
                slice = 1000;
            }
            (void)ntk_sem_timedwait(waitset->sem, (unsigned int)slice);
        }
        if (nb_dds_waitset_ready(waitset, filter) > 0) {
            return 1;
        }
    }
}
