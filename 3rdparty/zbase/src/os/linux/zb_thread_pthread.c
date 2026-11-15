#include "zb/os/thread.h"
#include "zb/zb_errno.h"
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

struct zb_thread {
    pthread_t native;
    zb_thread_fn fn;
    void *user;
    zb_thread_priority_t priority;
};

static void *thread_trampoline(void *arg)
{
    zb_thread *t = (zb_thread *)arg;
    zb_thread_set_current_priority(t->priority);
    t->fn(t->user);
    return NULL;
}

zb_thread *zb_thread_create_ex(zb_thread_fn fn, void *user, zb_thread_priority_t priority)
{
    if (!fn)
        return NULL;
    zb_thread *t = (zb_thread *)calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->fn = fn;
    t->user = user;
    t->priority = priority;
    if (pthread_create(&t->native, NULL, thread_trampoline, t) != 0) {
        free(t);
        return NULL;
    }
    return t;
}

zb_thread *zb_thread_create(zb_thread_fn fn, void *user)
{
    return zb_thread_create_ex(fn, user, ZB_THREAD_PRIO_NORMAL);
}

zb_err_t zb_thread_join(zb_thread *t)
{
    if (!t)
        return ZB_ERR_INVALID;
    void *ret = NULL;
    if (pthread_join(t->native, &ret) != 0)
        return ZB_ERR_PLATFORM;
    return ZB_OK;
}

void zb_thread_destroy(zb_thread *t)
{
    free(t);
}

unsigned int zb_thread_hardware_concurrency(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1)
        n = 1;
    return (unsigned int)n;
}
