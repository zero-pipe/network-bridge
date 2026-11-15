#include "zb/os/sem.h"
#include <stdlib.h>
#include <windows.h>

struct zb_sem {
    HANDLE h;
};

zb_sem *zb_sem_create(unsigned int initial)
{
    zb_sem *s = (zb_sem *)calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->h = CreateSemaphoreA(NULL, (LONG)initial, 0x7fffffff, NULL);
    if (!s->h) {
        free(s);
        return NULL;
    }
    return s;
}

void zb_sem_destroy(zb_sem *s)
{
    if (!s)
        return;
    if (s->h)
        CloseHandle(s->h);
    free(s);
}

zb_err_t zb_sem_wait(zb_sem *s)
{
    if (!s)
        return ZB_ERR_INVALID;
    return WaitForSingleObject(s->h, INFINITE) == WAIT_OBJECT_0 ? ZB_OK : ZB_ERR_PLATFORM;
}

zb_err_t zb_sem_timedwait(zb_sem *s, unsigned int timeout_ms)
{
    if (!s)
        return ZB_ERR_INVALID;
    DWORD r = WaitForSingleObject(s->h, timeout_ms);
    if (r == WAIT_OBJECT_0)
        return ZB_OK;
    if (r == WAIT_TIMEOUT)
        return ZB_ERR_TIMEOUT;
    return ZB_ERR_PLATFORM;
}

zb_err_t zb_sem_post(zb_sem *s, unsigned int count)
{
    if (!s || !count)
        return ZB_ERR_INVALID;
    return ReleaseSemaphore(s->h, (LONG)count, NULL) ? ZB_OK : ZB_ERR_PLATFORM;
}
