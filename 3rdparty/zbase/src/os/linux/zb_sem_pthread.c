#include "zb/os/sem.h"
#include "zb/zb_errno.h"
#include "zb_config.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#if defined(ZB_HAVE_POSIX_SEM)
#  include <semaphore.h>
#endif

struct zb_sem {
#if defined(ZB_HAVE_POSIX_SEM)
    sem_t native;
#else
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    unsigned int count;
#endif
};

zb_sem *zb_sem_create(unsigned int initial)
{
    zb_sem *s = (zb_sem *)calloc(1, sizeof(*s));
    if (!s)
        return NULL;

#if defined(ZB_HAVE_POSIX_SEM)
    if (sem_init(&s->native, 0, initial) != 0) {
        free(s);
        return NULL;
    }
#else
    pthread_mutex_init(&s->mtx, NULL);
    pthread_cond_init(&s->cond, NULL);
    s->count = initial;
#endif
    return s;
}

void zb_sem_destroy(zb_sem *s)
{
    if (!s)
        return;
#if defined(ZB_HAVE_POSIX_SEM)
    sem_destroy(&s->native);
#else
    pthread_mutex_destroy(&s->mtx);
    pthread_cond_destroy(&s->cond);
#endif
    free(s);
}

static zb_err_t sem_wait_impl(zb_sem *s, int timeout_ms)
{
#if defined(ZB_HAVE_POSIX_SEM)
    if (timeout_ms < 0) {
        while (sem_wait(&s->native) != 0) {
            if (errno != EINTR)
                return ZB_ERR_PLATFORM;
        }
        return ZB_OK;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    int r;
    do {
        r = sem_timedwait(&s->native, &ts);
    } while (r != 0 && errno == EINTR);
    if (r == 0)
        return ZB_OK;
    if (errno == ETIMEDOUT)
        return ZB_ERR_TIMEOUT;
    return ZB_ERR_PLATFORM;
#else
    pthread_mutex_lock(&s->mtx);
    if (timeout_ms < 0) {
        while (s->count == 0)
            pthread_cond_wait(&s->cond, &s->mtx);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        while (s->count == 0) {
            int r = pthread_cond_timedwait(&s->cond, &s->mtx, &ts);
            if (r == ETIMEDOUT) {
                pthread_mutex_unlock(&s->mtx);
                return ZB_ERR_TIMEOUT;
            }
            if (r != 0 && r != EINTR) {
                pthread_mutex_unlock(&s->mtx);
                return ZB_ERR_PLATFORM;
            }
        }
    }
    s->count--;
    pthread_mutex_unlock(&s->mtx);
    return ZB_OK;
#endif
}

zb_err_t zb_sem_wait(zb_sem *s)
{
    if (!s)
        return ZB_ERR_INVALID;
    return sem_wait_impl(s, -1);
}

zb_err_t zb_sem_timedwait(zb_sem *s, unsigned int timeout_ms)
{
    if (!s)
        return ZB_ERR_INVALID;
    return sem_wait_impl(s, (int)timeout_ms);
}

zb_err_t zb_sem_post(zb_sem *s, unsigned int count)
{
    if (!s || count == 0)
        return ZB_ERR_INVALID;

#if defined(ZB_HAVE_POSIX_SEM)
    for (unsigned int i = 0; i < count; ++i) {
        if (sem_post(&s->native) != 0)
            return ZB_ERR_PLATFORM;
    }
    return ZB_OK;
#else
    pthread_mutex_lock(&s->mtx);
    s->count += count;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->mtx);
    return ZB_OK;
#endif
}
