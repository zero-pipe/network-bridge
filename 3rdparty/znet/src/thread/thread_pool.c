#include "zn/thread/thread_pool.h"
#include "zn/platform.h"
#include "zb/os/sync.h"
#include "zb/os/sem.h"
#include <stdlib.h>

typedef struct zn_thread_pool_task {
    zn_thread_pool_fn fn;
    void *user;
    struct zn_thread_pool_task *next;
} zn_thread_pool_task;

struct zn_thread_pool {
    zb_mutex *mtx;
    zb_sem *sem;
    zn_thread_pool_task *head;
    zn_thread_pool_task *tail;
    unsigned worker_count;
    zb_thread **workers;
    uint64_t *worker_ids;
    zb_thread_priority_t priority;
    volatile int shutdown;
    int started;
};

static int is_pool_worker(zn_thread_pool *pool)
{
    uint64_t self = zb_thread_self_id();
    for (unsigned i = 0; i < pool->worker_count; ++i) {
        if (pool->worker_ids[i] == self)
            return 1;
    }
    return 0;
}

static zn_thread_pool_task *pop_task(zn_thread_pool *pool)
{
    zn_thread_pool_task *t = pool->head;
    if (t) {
        pool->head = t->next;
        if (!pool->head)
            pool->tail = NULL;
        t->next = NULL;
    }
    return t;
}

typedef struct zn_thread_pool_worker_ctx {
    zn_thread_pool *pool;
    unsigned index;
} zn_thread_pool_worker_ctx;

static void pool_worker(void *arg)
{
    zn_thread_pool_worker_ctx *wctx = (zn_thread_pool_worker_ctx *)arg;
    zn_thread_pool *pool = wctx->pool;
    if (wctx->index < pool->worker_count)
        pool->worker_ids[wctx->index] = zb_thread_self_id();
    free(wctx);

    for (;;) {
        if (zb_sem_wait(pool->sem) != ZB_OK)
            break;

        zb_mutex_lock(pool->mtx);
        zn_thread_pool_task *task = pop_task(pool);
        zb_mutex_unlock(pool->mtx);

        if (!task)
            break;

        /* 先取出任务参数并释放节点，再执行：节点不在执行期间占用内存。 */
        zn_thread_pool_fn fn = task->fn;
        void *user = task->user;
        free(task);
        fn(user);
    }
}

static zn_err_t push_task(zn_thread_pool *pool, zn_thread_pool_fn fn, void *user, int first)
{
    zn_thread_pool_task *task = (zn_thread_pool_task *)calloc(1, sizeof(*task));
    if (!task)
        return ZN_ERR_NOMEM;
    task->fn = fn;
    task->user = user;

    zb_mutex_lock(pool->mtx);
    if (first) {
        task->next = pool->head;
        pool->head = task;
        if (!pool->tail)
            pool->tail = task;
    } else {
        task->next = NULL;
        if (pool->tail)
            pool->tail->next = task;
        else
            pool->head = task;
        pool->tail = task;
    }
    zb_mutex_unlock(pool->mtx);

    return (zn_err_t)zb_sem_post(pool->sem, 1);
}

zn_thread_pool *zn_thread_pool_create(const zn_thread_pool_opts_t *opts)
{
    unsigned n = opts && opts->thread_count ? opts->thread_count : zb_thread_hardware_concurrency();
    if (n < 1)
        n = 1;

    zn_thread_pool *pool = (zn_thread_pool *)calloc(1, sizeof(*pool));
    if (!pool)
        return NULL;

    pool->mtx = zb_mutex_create(ZB_MUTEX_NORMAL);
    pool->sem = zb_sem_create(0);
    if (!pool->mtx || !pool->sem) {
        zn_thread_pool_destroy(pool);
        return NULL;
    }

    pool->worker_count = n;
    pool->priority = opts ? opts->priority : ZB_THREAD_PRIO_HIGHEST;
    pool->workers = (zb_thread **)calloc(n, sizeof(zb_thread *));
    pool->worker_ids = (uint64_t *)calloc(n, sizeof(uint64_t));
    if (!pool->workers || !pool->worker_ids) {
        zn_thread_pool_destroy(pool);
        return NULL;
    }

    if (opts && opts->auto_start) {
        zn_err_t err = zn_thread_pool_start(pool);
        if (err != ZN_OK) {
            zn_thread_pool_destroy(pool);
            return NULL;
        }
    }

    return pool;
}

static void join_workers(zn_thread_pool *pool, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        if (pool->workers[i]) {
            zb_thread_join(pool->workers[i]);
            zb_thread_destroy(pool->workers[i]);
            pool->workers[i] = NULL;
        }
    }
}

zn_err_t zn_thread_pool_start(zn_thread_pool *pool)
{
    unsigned created = 0;

    if (!pool || pool->started)
        return pool && pool->started ? ZN_OK : ZN_ERR_INVALID;

    for (unsigned i = 0; i < pool->worker_count; ++i) {
        zn_thread_pool_worker_ctx *wctx = (zn_thread_pool_worker_ctx *)calloc(1, sizeof(*wctx));
        if (!wctx) {
            join_workers(pool, created);
            return ZN_ERR_NOMEM;
        }
        wctx->pool = pool;
        wctx->index = i;
        pool->workers[i] = zb_thread_create_ex(pool_worker, wctx, pool->priority);
        if (!pool->workers[i]) {
            free(wctx);
            join_workers(pool, created);
            return ZN_ERR_PLATFORM;
        }
        ++created;
    }
    pool->started = 1;
    return ZN_OK;
}

void zn_thread_pool_fini(zn_thread_pool *pool)
{
    if (!pool || !pool->started)
        return;

    pool->shutdown = 1;
    zb_sem_post(pool->sem, pool->worker_count);

    join_workers(pool, pool->worker_count);
    pool->started = 0;
}

void zn_thread_pool_destroy(zn_thread_pool *pool)
{
    if (!pool)
        return;
    if (pool->started)
        zn_thread_pool_fini(pool);

    zn_thread_pool_task *t = pool->head;
    while (t) {
        zn_thread_pool_task *next = t->next;
        free(t);
        t = next;
    }

    if (pool->workers)
        free(pool->workers);
    if (pool->worker_ids)
        free(pool->worker_ids);
    if (pool->sem)
        zb_sem_destroy(pool->sem);
    if (pool->mtx)
        zb_mutex_destroy(pool->mtx);
    free(pool);
}

zn_err_t zn_thread_pool_async(zn_thread_pool *pool, zn_thread_pool_fn fn, void *user, int may_sync)
{
    if (!pool || !fn)
        return ZN_ERR_INVALID;
    if (!pool->started)
        return ZN_ERR_STATE;
    if (may_sync && is_pool_worker(pool)) {
        fn(user);
        return ZN_OK;
    }
    return push_task(pool, fn, user, 0);
}

zn_err_t zn_thread_pool_async_first(zn_thread_pool *pool, zn_thread_pool_fn fn, void *user, int may_sync)
{
    if (!pool || !fn)
        return ZN_ERR_INVALID;
    if (!pool->started)
        return ZN_ERR_STATE;
    if (may_sync && is_pool_worker(pool)) {
        fn(user);
        return ZN_OK;
    }
    return push_task(pool, fn, user, 1);
}

unsigned zn_thread_pool_worker_count(const zn_thread_pool *pool)
{
    return pool ? pool->worker_count : 0;
}
