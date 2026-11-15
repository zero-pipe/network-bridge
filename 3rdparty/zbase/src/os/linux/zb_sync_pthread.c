#include "zb/os/sync.h"
#include "zb/zb_errno.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

struct zb_mutex {
    pthread_mutex_t native;
};

zb_mutex *zb_mutex_create(int flags)
{
    zb_mutex *m = (zb_mutex *)calloc(1, sizeof(*m));
    if (!m)
        return NULL;

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    if (flags & ZB_MUTEX_RECURSIVE)
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);

    if (pthread_mutex_init(&m->native, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        free(m);
        return NULL;
    }
    pthread_mutexattr_destroy(&attr);
    return m;
}

void zb_mutex_destroy(zb_mutex *m)
{
    if (!m)
        return;
    pthread_mutex_destroy(&m->native);
    free(m);
}

zb_err_t zb_mutex_lock(zb_mutex *m)
{
    if (!m)
        return ZB_ERR_INVALID;
    return pthread_mutex_lock(&m->native) == 0 ? ZB_OK : ZB_ERR_PLATFORM;
}

zb_err_t zb_mutex_unlock(zb_mutex *m)
{
    if (!m)
        return ZB_ERR_INVALID;
    return pthread_mutex_unlock(&m->native) == 0 ? ZB_OK : ZB_ERR_PLATFORM;
}

zb_err_t zb_mutex_trylock(zb_mutex *m)
{
    if (!m)
        return ZB_ERR_INVALID;
    int r = pthread_mutex_trylock(&m->native);
    if (r == 0)
        return ZB_OK;
    if (r == EBUSY)
        return ZB_ERR_AGAIN;
    return ZB_ERR_PLATFORM;
}
