#include "zb/os/thread.h"
#include <stdlib.h>
#include <windows.h>

struct zb_thread {
    HANDLE h;
    zb_thread_fn fn;
    void *user;
    zb_thread_priority_t priority;
};

static DWORD WINAPI trampoline(LPVOID arg)
{
    zb_thread *t = (zb_thread *)arg;
    zb_thread_set_current_priority(t->priority);
    t->fn(t->user);
    return 0;
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
    t->h = CreateThread(NULL, 0, trampoline, t, 0, NULL);
    if (!t->h) {
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
    DWORD r = WaitForSingleObject(t->h, INFINITE);
    return r == WAIT_OBJECT_0 ? ZB_OK : ZB_ERR_PLATFORM;
}

void zb_thread_destroy(zb_thread *t)
{
    if (!t)
        return;
    if (t->h)
        CloseHandle(t->h);
    free(t);
}

unsigned int zb_thread_hardware_concurrency(void)
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    if (info.dwNumberOfProcessors < 1)
        return 1;
    return (unsigned int)info.dwNumberOfProcessors;
}
