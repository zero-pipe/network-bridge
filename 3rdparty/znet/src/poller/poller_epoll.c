#include "poller_internal.h"
#include "zn/poller/poller.h"
#include "zn/zn_errno.h"
#include "zn_config.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#define ZN_EPOLL_MAX_EVENTS 64

struct zn_fd_entry {
    int fd;
    unsigned events;
    zn_poller_cb cb;
    void *user;
    struct zn_fd_entry *next;
};

static zn_fd_entry *find_entry(zn_poller *p, int fd)
{
    for (zn_fd_entry *e = p->entries; e; e = e->next) {
        if (e->fd == fd)
            return e;
    }
    return NULL;
}

static int make_pipe(int fds[2])
{
#if defined(ZN_HAVE_PIPE2)
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0)
        return 0;
#endif
    if (pipe(fds) != 0)
        return -1;
    for (int i = 0; i < 2; ++i) {
        int flags = fcntl(fds[i], F_GETFL, 0);
        if (flags >= 0)
            fcntl(fds[i], F_SETFL, flags | O_NONBLOCK);
    }
    return 0;
}

static zn_err_t epoll_ctl_wrap(int epfd, int op, int fd, uint32_t ev)
{
    struct epoll_event ee;
    memset(&ee, 0, sizeof(ee));
    ee.events = ev;
    ee.data.fd = fd;
    if (epoll_ctl(epfd, op, fd, &ee) != 0)
        return ZN_ERR_PLATFORM;
    return ZN_OK;
}

static uint32_t to_epoll(unsigned events)
{
    uint32_t ev = 0;
    if (events & ZN_POLL_IN)
        ev |= EPOLLIN;
    if (events & ZN_POLL_OUT)
        ev |= EPOLLOUT;
    if (events & ZN_POLL_ERR)
        ev |= EPOLLERR;
    if (events & ZN_POLL_HUP)
        ev |= EPOLLHUP;
    return ev;
}

static void drain_wake_pipe(zn_poller *p)
{
    char buf[256];
    while (read(p->wake_r, buf, sizeof(buf)) > 0) {}
}

zn_poller *zn_poller_create(void)
{
    zn_poller *p = (zn_poller *)calloc(1, sizeof(*p));
    if (!p)
        return NULL;

    p->epfd = -1;
    p->wake_r = -1;
    p->wake_w = -1;
    zn_poller_task_init(p);

    p->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (p->epfd < 0) {
        p->epfd = epoll_create(ZN_EPOLL_MAX_EVENTS);
        if (p->epfd < 0)
            goto fail;
    }

    int pipes[2];
    if (make_pipe(pipes) != 0)
        goto fail;
    p->wake_r = pipes[0];
    p->wake_w = pipes[1];

    zn_fd_entry *we = (zn_fd_entry *)calloc(1, sizeof(*we));
    if (!we)
        goto fail;
    we->fd = p->wake_r;
    we->events = ZN_POLL_IN;
    we->next = p->entries;
    p->entries = we;

    if (epoll_ctl_wrap(p->epfd, EPOLL_CTL_ADD, p->wake_r, EPOLLIN) != ZN_OK)
        goto fail;
    return p;

fail:
    zn_poller_destroy(p);
    return NULL;
}

void zn_poller_destroy(zn_poller *p)
{
    if (!p)
        return;

    zn_poller_task_fini(p);

    while (p->entries) {
        zn_fd_entry *n = p->entries->next;
        free(p->entries);
        p->entries = n;
    }
    if (p->wake_w >= 0)
        close(p->wake_w);
    if (p->wake_r >= 0)
        close(p->wake_r);
    if (p->epfd >= 0)
        close(p->epfd);
    free(p);
}

static zn_err_t poller_add_locked(zn_poller *p, int fd, unsigned events, zn_poller_cb cb, void *user)
{
    if (!p || fd < 0)
        return ZN_ERR_INVALID;

    zn_fd_entry *e = find_entry(p, fd);
    if (e) {
        e->events = events;
        e->cb = cb;
        e->user = user;
        return epoll_ctl_wrap(p->epfd, EPOLL_CTL_MOD, fd, to_epoll(events));
    }

    e = (zn_fd_entry *)calloc(1, sizeof(*e));
    if (!e)
        return ZN_ERR_NOMEM;
    e->fd = fd;
    e->events = events;
    e->cb = cb;
    e->user = user;
    e->next = p->entries;
    p->entries = e;

    if (epoll_ctl_wrap(p->epfd, EPOLL_CTL_ADD, fd, to_epoll(events)) != ZN_OK) {
        p->entries = e->next;
        free(e);
        return ZN_ERR_PLATFORM;
    }
    return ZN_OK;
}

static zn_err_t poller_mod_locked(zn_poller *p, int fd, unsigned events)
{
    if (!p)
        return ZN_ERR_INVALID;
    zn_fd_entry *e = find_entry(p, fd);
    if (!e)
        return ZN_ERR_INVALID;
    e->events = events;
    return epoll_ctl_wrap(p->epfd, EPOLL_CTL_MOD, fd, to_epoll(events));
}

static zn_err_t poller_del_locked(zn_poller *p, int fd)
{
    if (!p)
        return ZN_ERR_INVALID;
    if (fd == p->wake_r)
        return ZN_ERR_INVALID;

    zn_fd_entry **prev = &p->entries;
    for (zn_fd_entry *e = p->entries; e; e = e->next) {
        if (e->fd == fd) {
            *prev = e->next;
            epoll_ctl(p->epfd, EPOLL_CTL_DEL, fd, NULL);
            free(e);
            return ZN_OK;
        }
        prev = &e->next;
    }
    return ZN_ERR_INVALID;
}

zn_err_t zn_poller_add(zn_poller *p, int fd, unsigned events, zn_poller_cb cb, void *user)
{
    zn_err_t rc;
    if (!p)
        return ZN_ERR_INVALID;
    zb_mutex_lock(p->entry_mtx);
    rc = poller_add_locked(p, fd, events, cb, user);
    zb_mutex_unlock(p->entry_mtx);
    return rc;
}

zn_err_t zn_poller_mod(zn_poller *p, int fd, unsigned events)
{
    zn_err_t rc;
    if (!p)
        return ZN_ERR_INVALID;
    zb_mutex_lock(p->entry_mtx);
    rc = poller_mod_locked(p, fd, events);
    zb_mutex_unlock(p->entry_mtx);
    return rc;
}

zn_err_t zn_poller_del(zn_poller *p, int fd)
{
    zn_err_t rc;
    if (!p)
        return ZN_ERR_INVALID;
    zb_mutex_lock(p->entry_mtx);
    rc = poller_del_locked(p, fd);
    zb_mutex_unlock(p->entry_mtx);
    return rc;
}

int zn_poller_poll(zn_poller *p, int timeout_ms)
{
    if (!p)
        return -1;

    struct epoll_event evs[ZN_EPOLL_MAX_EVENTS];
    int wait_ms = zn_poller_resolve_timeout_ms(p, timeout_ms);
    zn_poller_load_on_sleep(p);
    int n = epoll_wait(p->epfd, evs, ZN_EPOLL_MAX_EVENTS, wait_ms);
    zn_poller_load_on_wake(p);
    if (n < 0) {
        if (errno == EINTR)
            return 0;
        return -1;
    }

    if (n == 0 && p->task_mtx) {
        int pending = 0;
        zb_mutex_lock(p->task_mtx);
        pending = p->task_head != NULL;
        zb_mutex_unlock(p->task_mtx);
        if (pending)
            zn_poller_drain_tasks(p);
    }

    for (int i = 0; i < n; ++i) {
        int fd = evs[i].data.fd;
        if (fd == p->wake_r) {
            drain_wake_pipe(p);
            zn_poller_drain_tasks(p);
        }

        zn_poller_cb cb = NULL;
        void *user = NULL;

        zb_mutex_lock(p->entry_mtx);
        zn_fd_entry *e = find_entry(p, fd);
        if (e && e->cb) {
            cb = e->cb;
            user = e->user;
        }
        zb_mutex_unlock(p->entry_mtx);
        if (!cb)
            continue;

        unsigned out = 0;
        if (evs[i].events & EPOLLIN)
            out |= ZN_POLL_IN;
        if (evs[i].events & EPOLLOUT)
            out |= ZN_POLL_OUT;
        if (evs[i].events & EPOLLERR)
            out |= ZN_POLL_ERR;
        if (evs[i].events & EPOLLHUP)
            out |= ZN_POLL_HUP;
        cb(fd, out, user);
    }

    /* Drain async tasks every poll turn: wake pipe writes can coalesce (EAGAIN)
     * while HTTP/HLS fds keep epoll busy, which previously starved SRT demux feeds. */
    if (p->task_mtx) {
        int pending = 0;
        zb_mutex_lock(p->task_mtx);
        pending = p->task_head != NULL;
        zb_mutex_unlock(p->task_mtx);
        if (pending)
            zn_poller_drain_tasks(p);
    }
    return n;
}

int zn_poller_wake_read_fd(zn_poller *p)
{
    return p ? p->wake_r : -1;
}

int zn_poller_wake_write_fd(zn_poller *p)
{
    return p ? p->wake_w : -1;
}

zn_err_t zn_poller_wake(zn_poller *p)
{
    if (!p || p->wake_w < 0)
        return ZN_ERR_INVALID;
    char c = 1;
    if (write(p->wake_w, &c, 1) < 0) {
        if (errno == EAGAIN)
            return ZN_OK;
        return ZN_ERR_PLATFORM;
    }
    return ZN_OK;
}
