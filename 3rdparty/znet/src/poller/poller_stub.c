#include "poller_internal.h"
#include "zn/poller/poller.h"
#include <stdlib.h>

/* resolve_timeout declared in poller_internal.h */

zn_poller *zn_poller_create(void)
{
    zn_poller *p = (zn_poller *)calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->epfd = -1;
    p->wake_r = -1;
    p->wake_w = -1;
    zn_poller_task_init(p);
    p->epfd = -1;
    return p;
}

void zn_poller_destroy(zn_poller *p)
{
    if (!p)
        return;
    zn_poller_task_fini(p);
    free(p);
}

zn_err_t zn_poller_add(zn_poller *p, int fd, unsigned events, zn_poller_cb cb, void *user)
{
    (void)p;
    (void)fd;
    (void)events;
    (void)cb;
    (void)user;
    return ZN_ERR_NOT_IMPL;
}

zn_err_t zn_poller_mod(zn_poller *p, int fd, unsigned events)
{
    (void)p;
    (void)fd;
    (void)events;
    return ZN_ERR_NOT_IMPL;
}

zn_err_t zn_poller_del(zn_poller *p, int fd)
{
    (void)p;
    (void)fd;
    return ZN_ERR_NOT_IMPL;
}

int zn_poller_poll(zn_poller *p, int timeout_ms)
{
    (void)timeout_ms;
    if (!p)
        return -1;
    (void)zn_poller_resolve_timeout_ms(p, timeout_ms);
    zn_poller_drain_tasks(p);
    return 0;
}

int zn_poller_wake_read_fd(zn_poller *p)
{
    (void)p;
    return -1;
}

int zn_poller_wake_write_fd(zn_poller *p)
{
    (void)p;
    return -1;
}

zn_err_t zn_poller_wake(zn_poller *p)
{
    if (!p)
        return ZN_ERR_INVALID;
    /* 无 pipe 时仅排队，由 poll/process_pending 排空 */
    return ZN_OK;
}
