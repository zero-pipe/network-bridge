#include "zn/util/buf.h"
#include "zn/poller/poller.h"
#include "zb/os/sync.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <windows.h>
static int zn_atomic_fetch_add(volatile long *v, int delta)
{
    return (int)InterlockedExchangeAdd(v, (long)delta);
}
#elif defined(__GNUC__) || defined(__clang__)
static int zn_atomic_fetch_add(volatile int *v, int delta)
{
    return __atomic_fetch_add(v, delta, __ATOMIC_SEQ_CST);
}
#else
#error "zn_buf requires MSVC or GCC/clang atomics"
#endif

/* ── zn_buf：引用计数载荷 ── */

struct zn_buf {
#if defined(_MSC_VER)
    volatile long refcnt;
#else
    volatile int refcnt;
#endif
    size_t len;
    size_t cap;
    zn_buf_pool *pool;
    size_t pool_blk_cap;
    uint8_t data[];
};

static zn_buf_pool *g_shared_pool;

void zn_buf_set_shared_pool(zn_buf_pool *pool)
{
    g_shared_pool = pool;
}

static zn_buf *buf_create_malloc(size_t cap)
{
    zn_buf *b = (zn_buf *)malloc(sizeof(*b) + cap);
    if (!b) {
        return NULL;
    }
    b->refcnt = 1;
    b->len = 0;
    b->cap = cap;
    b->pool = NULL;
    b->pool_blk_cap = 0;
    return b;
}

static zn_buf *buf_create_from_pool(zn_buf_pool *pool, size_t cap)
{
    size_t hdr = offsetof(zn_buf, data);
    size_t need = hdr + cap;
    size_t blk_cap = 0;
    void *mem;

    if (!pool) {
        return buf_create_malloc(cap);
    }

    mem = zn_buf_pool_acquire(pool, need, &blk_cap);
    if (!mem) {
        return buf_create_malloc(cap);
    }

    {
        zn_buf *b = (zn_buf *)mem;
        b->refcnt = 1;
        b->len = 0;
        b->pool = pool;
        b->pool_blk_cap = blk_cap;
        b->cap = blk_cap > hdr ? blk_cap - hdr : cap;
        return b;
    }
}

zn_buf *zn_buf_alloc(size_t cap)
{
    if (cap == 0) {
        return NULL;
    }
    return buf_create_from_pool(g_shared_pool, cap);
}

zn_buf *zn_buf_alloc_local(zn_poller *poller, size_t cap)
{
    zn_buf_pool *local;

    if (cap == 0) {
        return NULL;
    }
    local = zn_poller_buf_pool(poller);
    if (local) {
        return buf_create_from_pool(local, cap);
    }
    return zn_buf_alloc(cap);
}

zn_buf *zn_buf_ref(zn_buf *b)
{
    if (!b) {
        return NULL;
    }
    zn_atomic_fetch_add(&b->refcnt, 1);
    return b;
}

zn_buf *zn_buf_try_ref(zn_buf *b)
{
    if (!b) {
        return NULL;
    }
#if defined(_MSC_VER)
    for (;;) {
        long c = b->refcnt;
        if (c <= 0) {
            return NULL;
        }
        if (InterlockedCompareExchange(&b->refcnt, c + 1, c) == c) {
            return b;
        }
    }
#elif defined(__GNUC__) || defined(__clang__)
    for (;;) {
        int c = __atomic_load_n(&b->refcnt, __ATOMIC_ACQUIRE);
        if (c <= 0) {
            return NULL;
        }
        if (__atomic_compare_exchange_n(&b->refcnt, &c, c + 1, 0, __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST)) {
            return b;
        }
    }
#else
#error "zn_buf_try_ref requires MSVC or GCC/clang atomics"
#endif
}

void zn_buf_unref(zn_buf *b)
{
    if (!b) {
        return;
    }
    if (zn_atomic_fetch_add(&b->refcnt, -1) == 1) {
        if (b->pool) {
            zn_buf_pool_release(b->pool, b, b->pool_blk_cap);
        } else {
            free(b);
        }
    }
}

const void *zn_buf_data(const zn_buf *b)
{
    return b ? (const void *)b->data : NULL;
}

size_t zn_buf_len(const zn_buf *b)
{
    return b ? b->len : 0;
}

size_t zn_buf_cap(const zn_buf *b)
{
    return b ? b->cap : 0;
}

int zn_buf_refcnt(const zn_buf *b)
{
    if (!b) {
        return 0;
    }
#if defined(_MSC_VER)
    return (int)b->refcnt;
#else
    return b->refcnt;
#endif
}

void zn_buf_set_len(zn_buf *b, size_t len)
{
    if (!b || len > b->cap) {
        return;
    }
    b->len = len;
}

/* ── zn_buf_pool：分档侵入式 freelist ── */

#define ZN_BUF_BUCKET_COUNT 9

/* 512 → 2K → 8K → 32K → 64K → 128K → 512K → 1M → 2M */
static const size_t k_bucket_caps[ZN_BUF_BUCKET_COUNT] = {
    512, 2048, 8192, 32768, 65536, 131072, 524288, 1048576, 2097152,
};

typedef struct zn_buf_free_node {
    struct zn_buf_free_node *next;
} zn_buf_free_node;

struct zn_buf_pool {
    unsigned max_per_bucket;
    int thread_safe;
    zn_buf_free_node *free_heads[ZN_BUF_BUCKET_COUNT];
    unsigned counts[ZN_BUF_BUCKET_COUNT];
    zn_buf_pool_stats stats;
    zb_mutex *mu; /* thread_safe=0 时为 NULL */
    zn_poller *owner; /* 无锁本地池所属 poller；跨线程 release 回投 */
};

typedef struct zn_buf_pool_defer {
    zn_buf_pool *pool;
    void *ptr;
    size_t cap;
} zn_buf_pool_defer;

void zn_buf_pool_set_owner_poller(zn_buf_pool *pool, zn_poller *owner)
{
    if (pool) {
        pool->owner = owner;
    }
}

static int bucket_index_for_size(size_t size, size_t *out_cap)
{
    int i;

    for (i = 0; i < ZN_BUF_BUCKET_COUNT; ++i) {
        if (size <= k_bucket_caps[i]) {
            if (out_cap) {
                *out_cap = k_bucket_caps[i];
            }
            return i;
        }
    }
    return -1;
}

static int bucket_index_exact_cap(size_t cap)
{
    int i;

    for (i = 0; i < ZN_BUF_BUCKET_COUNT; ++i) {
        if (cap == k_bucket_caps[i]) {
            return i;
        }
    }
    return -1;
}

static void pool_lock(zn_buf_pool *pool)
{
    if (pool && pool->mu) {
        zb_mutex_lock(pool->mu);
    }
}

static void pool_unlock(zn_buf_pool *pool)
{
    if (pool && pool->mu) {
        zb_mutex_unlock(pool->mu);
    }
}

static void pool_release_now(zn_buf_pool *pool, void *ptr, size_t cap)
{
    int bi;
    zn_buf_free_node *node;

    bi = bucket_index_exact_cap(cap);
    if (bi < 0) {
        pool_lock(pool);
        pool->stats.release_dropped++;
        pool_unlock(pool);
        free(ptr);
        return;
    }

    pool_lock(pool);
    if (pool->counts[bi] < pool->max_per_bucket) {
        node = (zn_buf_free_node *)ptr;
        node->next = pool->free_heads[bi];
        pool->free_heads[bi] = node;
        pool->counts[bi]++;
        pool->stats.release_cached++;
        pool_unlock(pool);
        return;
    }
    pool->stats.release_dropped++;
    pool_unlock(pool);
    free(ptr);
}

static void pool_release_deferred(void *user)
{
    zn_buf_pool_defer *d = (zn_buf_pool_defer *)user;

    if (!d) {
        return;
    }
    if (d->pool && d->ptr) {
        pool_release_now(d->pool, d->ptr, d->cap);
    }
    free(d);
}

zn_buf_pool *zn_buf_pool_create(const zn_buf_pool_opts *opts)
{
    zn_buf_pool *p = (zn_buf_pool *)calloc(1, sizeof(*p));
    int thread_safe = 1;

    if (!p) {
        return NULL;
    }
    p->max_per_bucket =
        (opts && opts->max_per_bucket > 0) ? opts->max_per_bucket : ZN_BUF_POOL_DEFAULT_MAX;
    if (opts) {
        thread_safe = opts->thread_safe ? 1 : 0;
    }
    p->thread_safe = thread_safe;
    if (thread_safe) {
        p->mu = zb_mutex_create(ZB_MUTEX_NORMAL);
        if (!p->mu) {
            free(p);
            return NULL;
        }
    }
    return p;
}

void zn_buf_pool_destroy(zn_buf_pool *pool)
{
    int i;

    if (!pool) {
        return;
    }
    pool->owner = NULL;
    pool_lock(pool);
    for (i = 0; i < ZN_BUF_BUCKET_COUNT; ++i) {
        zn_buf_free_node *n = pool->free_heads[i];
        while (n) {
            zn_buf_free_node *next = n->next;
            free(n);
            n = next;
        }
        pool->free_heads[i] = NULL;
        pool->counts[i] = 0;
    }
    pool_unlock(pool);
    if (pool->mu) {
        zb_mutex_destroy(pool->mu);
    }
    free(pool);
}

void *zn_buf_pool_acquire(zn_buf_pool *pool, size_t size, size_t *out_cap)
{
    size_t cap = 0;
    int bi;
    void *ptr = NULL;

    if (!pool || size == 0) {
        return NULL;
    }

    bi = bucket_index_for_size(size, &cap);
    if (bi < 0) {
        void *p = malloc(size);
        pool_lock(pool);
        pool->stats.acquire_oversize++;
        pool_unlock(pool);
        if (out_cap) {
            *out_cap = size;
        }
        return p;
    }

    pool_lock(pool);
    if (pool->free_heads[bi]) {
        zn_buf_free_node *n = pool->free_heads[bi];
        pool->free_heads[bi] = n->next;
        pool->counts[bi]--;
        pool->stats.acquire_hit++;
        ptr = n;
    } else {
        pool->stats.acquire_miss++;
    }
    pool_unlock(pool);

    if (!ptr) {
        ptr = malloc(cap);
        if (!ptr) {
            return NULL;
        }
    }
    if (out_cap) {
        *out_cap = cap;
    }
    return ptr;
}

void zn_buf_pool_release(zn_buf_pool *pool, void *ptr, size_t cap)
{
    zn_buf_pool_defer *d;

    if (!pool || !ptr) {
        return;
    }

    /* 无锁本地池：非 owner 线程末次释放 → 回投 owner；失败则 free，不踩 freelist */
    if (!pool->thread_safe && pool->owner && !zn_poller_is_current_thread(pool->owner)) {
        d = (zn_buf_pool_defer *)malloc(sizeof(*d));
        if (!d) {
            free(ptr);
            return;
        }
        d->pool = pool;
        d->ptr = ptr;
        d->cap = cap;
        if (zn_poller_async(pool->owner, pool_release_deferred, d, 0) != ZN_OK) {
            free(ptr);
            free(d);
        }
        return;
    }

    pool_release_now(pool, ptr, cap);
}

void zn_buf_pool_get_stats(const zn_buf_pool *pool, zn_buf_pool_stats *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!pool) {
        return;
    }
    pool_lock((zn_buf_pool *)pool);
    *out = pool->stats;
    pool_unlock((zn_buf_pool *)pool);
}

void zn_buf_pool_reset_stats(zn_buf_pool *pool)
{
    if (!pool) {
        return;
    }
    pool_lock(pool);
    memset(&pool->stats, 0, sizeof(pool->stats));
    pool_unlock(pool);
}

int zn_buf_pool_bucket_counts(const zn_buf_pool *pool, unsigned *out_counts, int max_buckets)
{
    int n;
    int i;

    if (!pool || !out_counts || max_buckets <= 0) {
        return 0;
    }
    n = max_buckets < ZN_BUF_BUCKET_COUNT ? max_buckets : ZN_BUF_BUCKET_COUNT;
    pool_lock((zn_buf_pool *)pool);
    for (i = 0; i < n; ++i) {
        out_counts[i] = pool->counts[i];
    }
    pool_unlock((zn_buf_pool *)pool);
    return n;
}
