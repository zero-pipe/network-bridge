#include <nb/buf.h>
#include <nb/poolset.h>
#include "atomic.h"

#include <nb/error.h>

#include <string.h>

#define NB_BUF_MAGIC 0x4e424246u /* 'NBBF' */

#if defined(_MSC_VER)
#define NB_TLS __declspec(thread)
#else
#define NB_TLS __thread
#endif

struct nb_buf {
    uint32_t magic;
    uint32_t flags;
    int32_t refcount;
    size_t size;
    size_t capacity;
    nb_buf_pool_t *pool;
    nb_buf_free_fn free_fn;
    void *opaque;
    uint8_t *data;
    struct nb_buf *next;
    uint8_t inline_data[];
};

struct nb_buf_pool {
    nb_buf_pool_config_t config;
    nb_spinlock_t lock;
    nb_buf_t *free_head;
    size_t created;
    size_t cached;
    size_t outstanding;
    int dead;
    int thread_safe;      /* 0 = NB_BUF_POOL_FLAG_LOCAL: lock-free, owned thread */
    uintptr_t owner_token; /* thread token of the owning thread (thread_safe=0) */
    nb_buf_pool_stats_t stats;
};

/* Per-thread token: the address of a thread-local byte is unique per thread. */
static NB_TLS char g_thread_token;
static uintptr_t nb_thread_token(void)
{
    return (uintptr_t)&g_thread_token;
}

static void pool_lock(nb_buf_pool_t *pool)
{
    if (pool->thread_safe) {
        nb_spinlock_lock(&pool->lock);
    }
}

static void pool_unlock(nb_buf_pool_t *pool)
{
    if (pool->thread_safe) {
        nb_spinlock_unlock(&pool->lock);
    }
}

static nb_buf_t *nb_buf_new_internal(size_t size, size_t alignment, nb_buf_pool_t *pool)
{
    nb_buf_t *buf;
    size_t total;
    uint8_t *data;

    if (size == 0 || size > (size_t)-1 - sizeof(nb_buf_t)) {
        return NULL;
    }
    total = sizeof(nb_buf_t) + size;
    if (alignment > 1) {
        if (alignment & (alignment - 1)) {
            return NULL;
        }
        if (size > (size_t)-1 - sizeof(nb_buf_t) - (alignment - 1)) {
            return NULL;
        }
        total += alignment - 1;
    }
    buf = alignment > 1 ? (nb_buf_t *)nb_malloc_aligned(total, alignment)
                        : (nb_buf_t *)nb_malloc(total);
    if (!buf) {
        return NULL;
    }
    data = buf->inline_data;
    if (alignment > 1) {
        uintptr_t p = (uintptr_t)data;
        data = (uint8_t *)((p + alignment - 1) & ~(uintptr_t)(alignment - 1));
    }
    buf->magic = NB_BUF_MAGIC;
    buf->flags = pool ? NB_BUF_FLAG_POOLED : 0u;
    buf->refcount = 0;
    buf->size = size;
    buf->capacity = size;
    buf->pool = pool;
    buf->free_fn = NULL;
    buf->opaque = NULL;
    buf->data = data;
    buf->next = NULL;
    return buf;
}

static void nb_buf_destroy(nb_buf_t *buf)
{
    if (!buf) {
        return;
    }
    if ((buf->flags & NB_BUF_FLAG_EXTERNAL) && buf->free_fn) {
        buf->free_fn(buf->opaque, buf->data);
    }
    nb_free(buf);
}

static void nb_buf_reset_for_use(nb_buf_t *buf, nb_buf_pool_t *pool)
{
    buf->magic = NB_BUF_MAGIC;
    buf->flags = NB_BUF_FLAG_POOLED;
    buf->refcount = 0;
    buf->size = pool->config.slot_size;
    buf->capacity = pool->config.slot_size;
    buf->pool = pool;
    buf->free_fn = NULL;
    buf->opaque = NULL;
    buf->data = buf->inline_data;
    buf->next = NULL;
}

static void nb_buf_pool_return(nb_buf_pool_t *pool, nb_buf_t *buf);

static void nb_buf_recycle(nb_buf_t *buf)
{
    if ((buf->flags & NB_BUF_FLAG_EXTERNAL) || !buf->pool) {
        nb_buf_destroy(buf);
        return;
    }
    nb_buf_pool_return(buf->pool, buf);
}

static void nb_buf_pool_free_mem(nb_buf_pool_t *pool)
{
    if (pool) {
        nb_free(pool);
    }
}

static void nb_buf_pool_return(nb_buf_pool_t *pool, nb_buf_t *buf)
{
    int last;

    /*
     * Lock-free local pool: a return from a non-owning thread cannot touch the
     * freelist, so free the block directly. The block is malloc-backed, so this
     * is always safe and the pool's counters stay owned-thread-only.
     */
    if (!pool->thread_safe && pool->owner_token != nb_thread_token()) {
        nb_buf_destroy(buf);
        return;
    }

    pool_lock(pool);
    if (pool->outstanding > 0) {
        pool->outstanding--;
    }
    if (!pool->dead &&
        (pool->config.max_cached == 0 || pool->cached < pool->config.max_cached)) {
        buf->next = pool->free_head;
        pool->free_head = buf;
        pool->cached++;
        pool_unlock(pool);
        return;
    }
    if (pool->created > 0) {
        pool->created--;
    }
    last = pool->dead && pool->outstanding == 0;
    pool_unlock(pool);
    nb_buf_destroy(buf);
    if (last) {
        nb_buf_pool_free_mem(pool);
    }
}

nb_buf_t *nb_buf_alloc(size_t size)
{
    return nb_buf_new_internal(size, 0, NULL);
}

nb_buf_t *nb_buf_alloc_aligned(size_t size, size_t alignment)
{
    return nb_buf_new_internal(size, alignment, NULL);
}

nb_buf_t *nb_buf_create_external(void *data,
                                 size_t size,
                                 nb_buf_free_fn free_fn,
                                 void *opaque,
                                 uint32_t flags)
{
    nb_buf_t *buf;

    if (!data || size == 0) {
        return NULL;
    }
    buf = (nb_buf_t *)nb_malloc(sizeof(*buf));
    if (!buf) {
        return NULL;
    }
    buf->magic = NB_BUF_MAGIC;
    buf->flags = NB_BUF_FLAG_EXTERNAL | (flags & NB_BUF_FLAG_READONLY);
    buf->refcount = 0;
    buf->size = size;
    buf->capacity = size;
    buf->pool = NULL;
    buf->free_fn = free_fn;
    buf->opaque = opaque;
    buf->data = (uint8_t *)data;
    buf->next = NULL;
    return buf;
}

nb_buf_t *nb_buf_move(nb_buf_t **buf)
{
    nb_buf_t *out;

    if (!buf) {
        return NULL;
    }
    out = *buf;
    *buf = NULL;
    return out;
}

void nb_buf_release(nb_buf_t *buf)
{
    if (!buf) {
        return;
    }
    if (!(buf->flags & NB_BUF_FLAG_SHARED)) {
        nb_buf_recycle(buf);
        return;
    }
    /* nb_atomic_ref_dec 返回递减后的值；0 才是最后一个引用。 */
    if (nb_atomic_ref_dec(&buf->refcount) == 0) {
        nb_buf_recycle(buf);
    }
}

int nb_buf_share(nb_buf_t *buf)
{
    if (!buf) {
        return NB_ERR_INVALID;
    }
    if (buf->flags & NB_BUF_FLAG_SHARED) {
        return NB_ERR_ALREADY;
    }
    buf->flags |= NB_BUF_FLAG_SHARED;
    buf->refcount = 1;
    return NB_OK;
}

nb_buf_t *nb_buf_ref(nb_buf_t *buf)
{
    if (!buf || !(buf->flags & NB_BUF_FLAG_SHARED)) {
        return NULL;
    }
    nb_atomic_ref_inc(&buf->refcount);
    return buf;
}

nb_buf_t *nb_buf_ref_n(nb_buf_t *buf, size_t n)
{
    if (!buf || !(buf->flags & NB_BUF_FLAG_SHARED) || n > 0x7ffffffeu) {
        return NULL;
    }
    if (n > 0) {
        nb_atomic_ref_add(&buf->refcount, (int32_t)n);
    }
    return buf;
}

uint8_t *nb_buf_data(nb_buf_t *buf)
{
    return buf ? buf->data : NULL;
}

const uint8_t *nb_buf_const_data(const nb_buf_t *buf)
{
    return buf ? buf->data : NULL;
}

size_t nb_buf_size(const nb_buf_t *buf)
{
    return buf ? buf->size : 0;
}

size_t nb_buf_capacity(const nb_buf_t *buf)
{
    return buf ? buf->capacity : 0;
}

uint32_t nb_buf_flags(const nb_buf_t *buf)
{
    return buf ? buf->flags : 0;
}

bool nb_buf_is_shared(const nb_buf_t *buf)
{
    return buf && (buf->flags & NB_BUF_FLAG_SHARED);
}

int nb_buf_set_size(nb_buf_t *buf, size_t size)
{
    if (!buf || size > buf->capacity) {
        return NB_ERR_INVALID;
    }
    buf->size = size;
    return NB_OK;
}

int nb_buf_slice(nb_buf_t *owner, size_t offset, size_t size, nb_buf_view_t *out)
{
    if (!owner || !out || offset > owner->size || size > owner->size - offset) {
        return NB_ERR_INVALID;
    }
    out->owner = owner;
    out->data = owner->data + offset;
    out->size = size;
    return NB_OK;
}

nb_buf_pool_t *nb_buf_pool_create(const nb_buf_pool_config_t *config)
{
    nb_buf_pool_t *pool;
    size_t i;

    if (!config || config->slot_size == 0) {
        return NULL;
    }
    pool = (nb_buf_pool_t *)nb_malloc(sizeof(*pool));
    if (!pool) {
        return NULL;
    }
    memset(pool, 0, sizeof(*pool));
    pool->config = *config;
    if (pool->config.max_cached == 0) {
        pool->config.max_cached = pool->config.slot_count ? pool->config.slot_count : 64;
    }
    nb_spinlock_init(&pool->lock);
    pool->thread_safe = (pool->config.flags & NB_BUF_POOL_FLAG_LOCAL) ? 0 : 1;
    pool->owner_token = pool->thread_safe ? 0u : nb_thread_token();
    if (pool->config.slot_count == 0 || (pool->config.flags & NB_BUF_POOL_FLAG_LAZY)) {
        return pool;
    }
    for (i = 0; i < pool->config.slot_count; i++) {
        nb_buf_t *buf = nb_buf_new_internal(pool->config.slot_size, 0, pool);
        if (!buf) {
            while (pool->free_head) {
                nb_buf_t *next = pool->free_head->next;
                nb_buf_destroy(pool->free_head);
                pool->free_head = next;
            }
            nb_free(pool);
            return NULL;
        }
        buf->next = pool->free_head;
        pool->free_head = buf;
        pool->cached++;
        pool->created++;
    }
    return pool;
}

nb_buf_t *nb_buf_acquire(nb_buf_pool_t *pool)
{
    nb_buf_t *buf = NULL;
    int need_alloc = 0;

    if (!pool) {
        return NULL;
    }
    pool_lock(pool);
    if (pool->dead) {
        pool_unlock(pool);
        return NULL;
    }
    if (pool->free_head) {
        buf = pool->free_head;
        pool->free_head = buf->next;
        if (pool->cached > 0) {
            pool->cached--;
        }
        pool->outstanding++;
        pool->stats.hits++;
        pool_unlock(pool);
        nb_buf_reset_for_use(buf, pool);
        return buf;
    }
    if (pool->config.slot_count != 0 && pool->created >= pool->config.slot_count) {
        pool->stats.misses++;
        pool_unlock(pool);
        return NULL;
    }
    pool->created++;
    pool->outstanding++;
    pool->stats.misses++;
    need_alloc = 1;
    pool_unlock(pool);

    if (need_alloc) {
        buf = nb_buf_new_internal(pool->config.slot_size, 0, pool);
        if (!buf) {
            pool_lock(pool);
            if (pool->created > 0) {
                pool->created--;
            }
            if (pool->outstanding > 0) {
                pool->outstanding--;
            }
            pool->stats.failed++;
            pool_unlock(pool);
        }
    }
    return buf;
}

void nb_buf_pool_destroy(nb_buf_pool_t *pool)
{
    nb_buf_pool_t *p;
    nb_buf_t *list;
    size_t count;
    int last;

    if (!pool) {
        return;
    }
    p = pool;

    if (!p->thread_safe) {
        /*
         * Owner-thread teardown of a lock-free local pool: callers destroy it at
         * thread exit, so no further owner-thread returns can happen. Cross-thread
         * returns free their block directly and never touch the pool. Drain the
         * freelist and free the pool object without the (absent) lock.
         */
        p->dead = 1;
        list = p->free_head;
        p->free_head = NULL;
        p->cached = 0;
        while (list) {
            nb_buf_t *next = list->next;
            nb_buf_destroy(list);
            list = next;
        }
        nb_buf_pool_free_mem(p);
        return;
    }

    pool_lock(p);
    p->dead = 1;
    list = p->free_head;
    count = p->cached;
    p->free_head = NULL;
    p->cached = 0;
    if (p->created >= count) {
        p->created -= count;
    } else {
        p->created = 0;
    }
    last = p->outstanding == 0;
    pool_unlock(p);

    while (list) {
        nb_buf_t *next = list->next;
        nb_buf_destroy(list);
        list = next;
    }
    if (last) {
        nb_buf_pool_free_mem(p);
    }
}

void nb_buf_pool_get_stats(const nb_buf_pool_t *pool, nb_buf_pool_stats_t *out)
{
    if (!pool || !out) {
        return;
    }
    pool_lock((nb_buf_pool_t *)pool);
    out->hits = pool->stats.hits;
    out->misses = pool->stats.misses;
    out->failed = pool->stats.failed;
    out->outstanding = pool->outstanding;
    out->cached = pool->cached;
    out->created = pool->created;
    pool_unlock((nb_buf_pool_t *)pool);
}

/* ---- per-thread local pools (nb_buf_alloc_local) ---- */

static const size_t k_local_class_sizes[] = {
    64,    128,   256,   512,   1024,   2048,   4096,   8192,
    16384, 32768, 65536, 131072, 524288, 1048576, 2097152,
};
static const size_t k_local_class_counts[] = {
    4096, 4096, 2048, 1024, 1024, 512, 256, 128,
    64,   32,   16,   8,    4,     2,     1,
};
#define NB_LOCAL_CLASS_COUNT (sizeof(k_local_class_sizes) / sizeof(k_local_class_sizes[0]))

static NB_TLS nb_poolset_t *g_local_poolset;
static NB_TLS int g_local_poolset_state; /* 0 = uninit, 1 = ready, 2 = disabled */

static nb_poolset_t *nb_local_poolset(void)
{
    if (g_local_poolset_state == 0) {
        g_local_poolset = nb_poolset_create_ex(k_local_class_sizes,
                                               k_local_class_counts,
                                               NB_LOCAL_CLASS_COUNT, 1);
        g_local_poolset_state = g_local_poolset ? 1 : 2;
    }
    return g_local_poolset_state == 1 ? g_local_poolset : NULL;
}

nb_buf_t *nb_buf_alloc_local(size_t size)
{
    nb_poolset_t *set = nb_local_poolset();

    if (set) {
        nb_buf_t *buf = nb_poolset_acquire(set, size);
        if (buf) {
            return buf;
        }
    }
    return nb_buf_alloc(size);
}

void nb_buf_local_release(void)
{
    if (g_local_poolset_state == 1 && g_local_poolset) {
        nb_poolset_destroy(g_local_poolset);
    }
    g_local_poolset = NULL;
    g_local_poolset_state = 0;
}
