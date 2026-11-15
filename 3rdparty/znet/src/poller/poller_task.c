#include "poller_internal.h"
#include "zn/poller/poller.h"
#include "zn/platform.h"
#include "zn/util/log.h"
#include "zn/zn_errno.h"
#include "zb/os/sem.h"
#include <stdlib.h>
#include <assert.h>

void zn_poller_task_init(zn_poller *p)
{
    if (!p)
        return;
    p->task_mtx = zb_mutex_create(ZB_MUTEX_NORMAL);
    p->entry_mtx = zb_mutex_create(ZB_MUTEX_NORMAL);
    p->task_head = NULL;
    p->task_tail = NULL;
    p->owner_thread = 0;
    zn_poller_timer_init(p);
    zn_poller_load_init(p);
}

void zn_poller_task_fini(zn_poller *p)
{
    if (!p)
        return;
    zn_poller_load_fini(p);
    zn_poller_timer_fini(p);
    zn_poller_drain_tasks(p);
    if (p->task_mtx) {
        zb_mutex_destroy(p->task_mtx);
        p->task_mtx = NULL;
    }
    if (p->entry_mtx) {
        zb_mutex_destroy(p->entry_mtx);
        p->entry_mtx = NULL;
    }
}

void zn_poller_drain_tasks(zn_poller *p)
{
    if (!p || !p->task_mtx)
        return;

    zn_poller_task *list = NULL;
    zb_mutex_lock(p->task_mtx);
    list = p->task_head;
    p->task_head = NULL;
    p->task_tail = NULL;
    zb_mutex_unlock(p->task_mtx);

    while (list) {
        zn_poller_task *next = list->next;
        if (list->fn)
            list->fn(list->user);
        free(list);
        list = next;
    }
}

zn_err_t zn_poller_async_impl(zn_poller *p, zn_poller_task_fn fn, void *user, int may_sync, int push_front)
{
    if (!p || !fn)
        return ZN_ERR_INVALID;

    if (may_sync && p->owner_thread != 0 && p->owner_thread == zb_thread_self_id()) {
        fn(user);
        return ZN_OK;
    }

    if (!p->task_mtx)
        return ZN_ERR_STATE;

    zn_poller_task *task = (zn_poller_task *)calloc(1, sizeof(*task));
    if (!task)
        return ZN_ERR_NOMEM;
    task->fn = fn;
    task->user = user;

    zb_mutex_lock(p->task_mtx);
    if (push_front) {
        task->next = p->task_head;
        p->task_head = task;
        if (!p->task_tail)
            p->task_tail = task;
    } else {
        task->next = NULL;
        if (p->task_tail)
            p->task_tail->next = task;
        else
            p->task_head = task;
        p->task_tail = task;
    }
    zb_mutex_unlock(p->task_mtx);

    return zn_poller_wake(p);
}

int zn_poller_is_current_thread(zn_poller *p)
{
    if (!p || p->owner_thread == 0)
        return 0;
    return p->owner_thread == zb_thread_self_id();
}

int zn_poller_is_running(const zn_poller *p)
{
    return p && p->owner_thread != 0;
}

void zn_poller_assert_owner(const zn_poller *p, const char *what)
{
#if defined(ZN_DEBUG_AFFINITY)
    if (p && p->owner_thread != 0 && p->owner_thread != zb_thread_self_id()) {
        zn_error("poller affinity violation: %s poller=%p thread=%llu owner=%llu",
                 what ? what : "?", (const void *)p,
                 (unsigned long long)zb_thread_self_id(),
                 (unsigned long long)p->owner_thread);
        assert(0);
    }
#else
    (void)p;
    (void)what;
#endif
}

void zn_poller_bind_thread(zn_poller *p)
{
    if (p)
        p->owner_thread = zb_thread_self_id();
}

void zn_poller_unbind_thread(zn_poller *p)
{
    if (p)
        p->owner_thread = 0;
}

typedef struct poller_del_ctx {
    zn_poller *p;
    int fd;
    zb_sem *done;
    zn_err_t rc;
} poller_del_ctx;

static void poller_del_async(void *user)
{
    poller_del_ctx *ctx = (poller_del_ctx *)user;
    ctx->rc = zn_poller_del(ctx->p, ctx->fd);
    zb_sem_post(ctx->done, 1);
}

zn_err_t zn_poller_del_sync(zn_poller *p, int fd)
{
    poller_del_ctx ctx;
    zb_sem *done;
    zn_err_t err;

    if (!p)
        return ZN_ERR_INVALID;
    if (!zn_poller_is_running(p) || zn_poller_is_current_thread(p))
        return zn_poller_del(p, fd);

    done = zb_sem_create(0);
    if (!done)
        return ZN_ERR_NOMEM;

    ctx.p = p;
    ctx.fd = fd;
    ctx.done = done;
    ctx.rc = ZN_ERR_STATE;

    err = zn_poller_async(p, poller_del_async, &ctx, 0);
    if (err == ZN_OK) {
        err = (zn_err_t)zb_sem_timedwait(done, 5000);
        if (err == ZN_OK)
            err = ctx.rc;
        else
            zn_error("poller %p: del fd=%d timeout (%d)", (void *)p, fd, (int)err);
    }
    zb_sem_destroy(done);
    return err;
}

zn_err_t zn_poller_async(zn_poller *p, zn_poller_task_fn fn, void *user, int may_sync)
{
    return zn_poller_async_impl(p, fn, user, may_sync, 0);
}

zn_err_t zn_poller_async_first(zn_poller *p, zn_poller_task_fn fn, void *user, int may_sync)
{
    return zn_poller_async_impl(p, fn, user, may_sync, 1);
}

void zn_poller_process_pending(zn_poller *p)
{
    zn_poller_drain_tasks(p);
}

void zn_poller_run(zn_poller *p, volatile int *stop_flag)
{
    unsigned fails = 0;

    if (!p)
        return;
    zn_poller_bind_thread(p);
    while (!stop_flag || !*stop_flag) {
        int n = zn_poller_poll(p, zn_poller_resolve_timeout_ms(p, ZN_POLLER_PENDING_POLL_MS));
        if (n >= 0) {
            fails = 0;
            continue;
        }
        /* 轮询后端偶发错误不得让 poller 线程静默退出：该线程上的所有会话
         * 会永久卡死且无任何日志。退避重试，错误持续时持续告警。 */
        if (fails == 0) {
            zn_error("poller %p: poll failed (%d), retrying", (void *)p, n);
        }
        if (++fails >= 8) {
            zn_sleep_ms(1);
        }
        if (fails > 0 && (fails % 5000) == 0) {
            zn_error("poller %p: still failing after %u retries", (void *)p, fails);
        }
    }
    zn_poller_unbind_thread(p);
}
