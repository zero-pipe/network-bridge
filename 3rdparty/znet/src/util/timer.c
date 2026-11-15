#include "zn/util/timer.h"
#include "zn/poller/poller.h"
#include <stdlib.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/time.h>
#endif

struct zn_timer {
    zn_poller *poller;
    zn_poller_timer *handle;
    uint64_t interval_ms;
    int repeat;
    zn_timer_cb cb;
    void *user;
};

static uint64_t timer_bridge(void *user)
{
    zn_timer *t = (zn_timer *)user;
    uint64_t interval = t->interval_ms;
    int repeat = t->repeat;
    zn_timer_cb cb = t->cb;
    void *cb_user = t->user;

    /* 回调内允许 zn_timer_stop(t)（t 随之被释放），因此先快照再回调。 */
    if (cb)
        cb(cb_user);
    return repeat ? interval : 0;
}

zn_timer *zn_timer_start(zn_poller *p, uint64_t interval_ms, int repeat, zn_timer_cb cb, void *user)
{
    if (!p || !cb || interval_ms == 0)
        return NULL;

    zn_timer *t = (zn_timer *)calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->poller = p;
    t->interval_ms = interval_ms;
    t->repeat = repeat ? 1 : 0;
    t->cb = cb;
    t->user = user;

    t->handle = zn_poller_do_delay(p, interval_ms, timer_bridge, t);
    if (!t->handle) {
        free(t);
        return NULL;
    }
    /* 句柄持有包装对象的生命周期：取消生效且在途回调结束后由 on_release 释放，
     * 从而支持跨线程 stop（调用线程与定时器所属 poller 线程不同时也安全）。 */
    zn_poller_timer_set_on_release(t->handle, (void (*)(void *))free);
    return t;
}

void zn_timer_stop(zn_timer *t)
{
    zn_poller_timer *h;

    if (!t)
        return;
    h = t->handle;
    t->handle = NULL;
    if (!h) {
        free(t);
        return;
    }
    /* cancel 后 t 可能立即在定时器所属线程被释放，此后不得再访问 t。 */
    zn_poller_timer_cancel(h);
}

uint64_t zn_wall_ms(void)
{
#if defined(_WIN32)
    FILETIME ft;
    ULARGE_INTEGER u;

    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart / 10000ULL - 11644473600000ULL;
#else
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
#endif
}
