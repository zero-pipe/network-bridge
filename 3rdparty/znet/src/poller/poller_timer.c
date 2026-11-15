#include "poller_internal.h"
#include "zn/poller/poller.h"
#include "zn/platform.h"
#include "zn/zn_errno.h"
#include <stdlib.h>
#include <string.h>

typedef struct delay_insert_ctx {
    zn_poller *poller;
    zn_poller_timer *timer;
    uint64_t deadline_ms;
} delay_insert_ctx;

/** 单次 poll 最多处理的 timer 个数，防止饿死 accept/async */
#define ZN_TIMER_FLUSH_MAX 256

#define ZN_TIMER_HEAP_INDEX_NONE ((size_t)-1)

static int timer_heap_reserve(zn_poller *p, size_t need)
{
    zn_poller_timer **nb;
    size_t cap = p->timer_heap_cap ? p->timer_heap_cap : 8;

    if (need <= p->timer_heap_cap)
        return 0;
    while (cap < need)
        cap *= 2;
    nb = (zn_poller_timer **)realloc(p->timer_heap, cap * sizeof(*nb));
    if (!nb)
        return -1;
    p->timer_heap = nb;
    p->timer_heap_cap = cap;
    return 0;
}

static void timer_heap_swap(zn_poller *p, size_t i, size_t j)
{
    zn_poller_timer *tmp = p->timer_heap[i];
    p->timer_heap[i] = p->timer_heap[j];
    p->timer_heap[j] = tmp;
    p->timer_heap[i]->heap_index = i;
    p->timer_heap[j]->heap_index = j;
}

static void timer_heap_bubble_up(zn_poller *p, size_t i)
{
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (p->timer_heap[parent]->deadline_ms <= p->timer_heap[i]->deadline_ms)
            break;
        timer_heap_swap(p, parent, i);
        i = parent;
    }
}

static void timer_heap_bubble_down(zn_poller *p, size_t i)
{
    for (;;) {
        size_t left = i * 2 + 1;
        size_t right = left + 1;
        size_t smallest = i;

        if (left < p->timer_heap_count &&
            p->timer_heap[left]->deadline_ms < p->timer_heap[smallest]->deadline_ms)
            smallest = left;
        if (right < p->timer_heap_count &&
            p->timer_heap[right]->deadline_ms < p->timer_heap[smallest]->deadline_ms)
            smallest = right;
        if (smallest == i)
            break;
        timer_heap_swap(p, i, smallest);
        i = smallest;
    }
}

static int timer_heap_push(zn_poller *p, zn_poller_timer *timer)
{
    if (timer_heap_reserve(p, p->timer_heap_count + 1) != 0)
        return -1;
    timer->heap_index = p->timer_heap_count;
    p->timer_heap[p->timer_heap_count++] = timer;
    timer_heap_bubble_up(p, timer->heap_index);
    return 0;
}

static zn_poller_timer *timer_heap_pop_min(zn_poller *p)
{
    zn_poller_timer *top;

    if (!p || p->timer_heap_count == 0)
        return NULL;
    top = p->timer_heap[0];
    top->heap_index = ZN_TIMER_HEAP_INDEX_NONE;
    --p->timer_heap_count;
    if (p->timer_heap_count > 0) {
        p->timer_heap[0] = p->timer_heap[p->timer_heap_count];
        p->timer_heap[0]->heap_index = 0;
        timer_heap_bubble_down(p, 0);
    }
    return top;
}

static void timer_heap_remove(zn_poller *p, zn_poller_timer *timer)
{
    size_t i = timer->heap_index;

    if (!p || !timer || i == ZN_TIMER_HEAP_INDEX_NONE || i >= p->timer_heap_count)
        return;

    timer->heap_index = ZN_TIMER_HEAP_INDEX_NONE;
    --p->timer_heap_count;
    if (i < p->timer_heap_count) {
        p->timer_heap[i] = p->timer_heap[p->timer_heap_count];
        p->timer_heap[i]->heap_index = i;
        timer_heap_bubble_down(p, i);
        timer_heap_bubble_up(p, i);
    }
}

void zn_poller_timer_init(zn_poller *p)
{
    if (!p)
        return;
    /* 回调在 flush_expired_timers 中于锁外执行（见该函数），持锁路径只做堆操作，
     * 因此普通互斥即可；回调内再调用 zn_timer_start/zn_poller_do_delay 也不会
     * 自锁，更不会与其它线程的 zn_poller_timer_cancel 形成锁序环。 */
    p->timer_mtx = zb_mutex_create(ZB_MUTEX_NORMAL);
    p->timer_heap = NULL;
    p->timer_heap_count = 0;
    p->timer_heap_cap = 0;
}

void zn_poller_timer_fini(zn_poller *p)
{
    zn_poller_timer *left[ZN_TIMER_FLUSH_MAX];
    size_t n;
    size_t i;

    if (!p)
        return;
    /* 分批摘链后释放，避免长时间持有 timer_mtx（与 flush 的摘链方式一致）。 */
    for (;;) {
        zb_mutex_lock(p->timer_mtx);
        for (n = 0; n < ZN_TIMER_FLUSH_MAX && p->timer_heap_count > 0; ++n) {
            left[n] = timer_heap_pop_min(p);
        }
        if (p->timer_heap_count == 0) {
            free(p->timer_heap);
            p->timer_heap = NULL;
            p->timer_heap_cap = 0;
        }
        zb_mutex_unlock(p->timer_mtx);
        for (i = 0; i < n; ++i) {
            if (left[i]->on_release) {
                left[i]->on_release(left[i]->user);
            }
            free(left[i]);
        }
        if (n == 0) {
            break;
        }
    }
    if (p->timer_mtx) {
        zb_mutex_destroy(p->timer_mtx);
        p->timer_mtx = NULL;
    }
}

void zn_poller_timer_insert_locked(zn_poller *p, zn_poller_timer *timer, uint64_t deadline_ms)
{
    if (!p || !timer)
        return;
    timer->deadline_ms = deadline_ms;
    if (timer->heap_index != ZN_TIMER_HEAP_INDEX_NONE)
        timer_heap_remove(p, timer);
    (void)timer_heap_push(p, timer);
}

static void delay_insert_async(void *user)
{
    delay_insert_ctx *ctx = (delay_insert_ctx *)user;
    zb_mutex_lock(ctx->poller->timer_mtx);
    zn_poller_timer_insert_locked(ctx->poller, ctx->timer, ctx->deadline_ms);
    zb_mutex_unlock(ctx->poller->timer_mtx);
    free(ctx);
}

/** 释放一个引用；refs 归零时回调 on_release 并销毁。调用方须持有 timer_mtx 且不再访问 timer。
 *  on_release 只允许做释放类动作（如 free 包装层对象），不得回调本模块 API。 */
static void timer_release_locked(zn_poller_timer *timer)
{
    if (!timer || --timer->refs > 0) {
        return;
    }
    if (timer->on_release) {
        timer->on_release(timer->user);
    }
    free(timer);
}

void zn_poller_timer_set_on_release(zn_poller_timer *timer, void (*fn)(void *user))
{
    zn_poller *p;

    if (!timer) {
        return;
    }
    p = timer->poller;
    if (!p || !p->timer_mtx) {
        timer->on_release = fn;
        return;
    }
    zb_mutex_lock(p->timer_mtx);
    timer->on_release = fn;
    zb_mutex_unlock(p->timer_mtx);
}

/** 堆顶定时器距到期还有多久（毫秒）。调用方须持有 timer_mtx。
 *  返回 -1 表示堆空，0 表示已到期。 */
static int64_t timer_next_delay_locked(zn_poller *p, uint64_t now_ms)
{
    if (p->timer_heap_count == 0) {
        return -1;
    }
    if (p->timer_heap[0]->deadline_ms <= now_ms) {
        return 0;
    }
    return (int64_t)(p->timer_heap[0]->deadline_ms - now_ms);
}

static int64_t flush_expired_timers(zn_poller *p, uint64_t now_ms)
{
    zn_poller_timer *due[ZN_TIMER_FLUSH_MAX];
    uint64_t next_ms[ZN_TIMER_FLUSH_MAX];
    size_t n = 0;
    size_t i;
    int64_t delay;

    /* 阶段一（持锁）：把到期定时器摘出堆。摘出的定时器仍持有堆引用，
     * 跨线程 cancel 通过引用计数等待其结束，不会悬空。 */
    zb_mutex_lock(p->timer_mtx);
    while (n < ZN_TIMER_FLUSH_MAX && p->timer_heap_count > 0 &&
           p->timer_heap[0]->deadline_ms <= now_ms) {
        zn_poller_timer *timer = timer_heap_pop_min(p);

        if (!timer || timer->cancelled) {
            timer_release_locked(timer);
            continue;
        }
        due[n++] = timer;
    }
    delay = timer_next_delay_locked(p, now_ms);
    zb_mutex_unlock(p->timer_mtx);

    if (n == 0) {
        return delay;
    }

    /* 阶段二（锁外）：执行回调。回调可能取业务锁（如 HLS 的 rec->mu）、也可能
     * 再次注册/取消定时器；放在 timer_mtx 之外是避免跨线程 ABBA 死锁的关键。 */
    for (i = 0; i < n; ++i) {
        next_ms[i] = due[i]->cb ? due[i]->cb(due[i]->user) : 0;
    }

    /* 阶段三（持锁）：按回调结果重挂或释放；期间被取消的直接释放。 */
    zb_mutex_lock(p->timer_mtx);
    for (i = 0; i < n; ++i) {
        zn_poller_timer *timer = due[i];

        if (next_ms[i] > 0 && !timer->cancelled) {
            zn_poller_timer_insert_locked(p, timer, now_ms + next_ms[i]);
        } else {
            timer_release_locked(timer);
        }
    }
    delay = timer_next_delay_locked(p, now_ms);
    zb_mutex_unlock(p->timer_mtx);
    return delay;
}

int zn_poller_resolve_timeout_ms(zn_poller *p, int request_ms)
{
    uint64_t now;
    int64_t delay;

    if (!p || !p->timer_mtx)
        return request_ms;

    /* flush_expired_timers 内部自行加/解锁（阶段二要求锁外执行回调）。
     * 这里不能再持有 timer_mtx，否则 NORMAL 互斥会自死锁。 */
    now = zn_monotonic_ms();
    delay = flush_expired_timers(p, now);

    if (delay < 0)
        return request_ms;
    if (request_ms < 0)
        return (int)(delay > 0x7fffffff ? 0x7fffffff : delay);
    if (request_ms == 0)
        return 0;
    return (int)(delay < request_ms ? delay : request_ms);
}

zn_poller_timer *zn_poller_do_delay(zn_poller *p, uint64_t delay_ms, zn_poller_delay_cb cb, void *user)
{
    if (!p || !cb || !p->timer_mtx)
        return NULL;

    zn_poller_timer *timer = (zn_poller_timer *)calloc(1, sizeof(*timer));
    if (!timer)
        return NULL;
    timer->poller = p;
    timer->cb = cb;
    timer->user = user;
    timer->refs = 1;
    timer->heap_index = ZN_TIMER_HEAP_INDEX_NONE;

    uint64_t deadline = zn_monotonic_ms() + delay_ms;

    if (p->owner_thread != 0 && p->owner_thread == zb_thread_self_id()) {
        zb_mutex_lock(p->timer_mtx);
        zn_poller_timer_insert_locked(p, timer, deadline);
        zb_mutex_unlock(p->timer_mtx);
        zn_poller_wake(p);
        return timer;
    }

    delay_insert_ctx *ctx = (delay_insert_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        free(timer);
        return NULL;
    }
    ctx->poller = p;
    ctx->timer = timer;
    ctx->deadline_ms = deadline;
    if (zn_poller_async_first(p, delay_insert_async, ctx, 0) != ZN_OK) {
        free(ctx);
        free(timer);
        return NULL;
    }
    return timer;
}

typedef struct timer_cancel_ctx {
    zn_poller_timer *timer;
    zn_poller *poller;
} timer_cancel_ctx;

static void timer_cancel_async(void *user)
{
    timer_cancel_ctx *ctx = (timer_cancel_ctx *)user;
    zn_poller_timer *timer = ctx->timer;
    zn_poller *p = ctx->poller;
    free(ctx);
    if (!p || !p->timer_mtx) {
        /* poller 已销毁：不再访问 timer，取消预取的引用随进程结束丢弃。 */
        return;
    }
    zb_mutex_lock(p->timer_mtx);
    if (timer->heap_index != ZN_TIMER_HEAP_INDEX_NONE) {
        timer_heap_remove(p, timer);
        timer_release_locked(timer); /* 堆引用 */
    }
    timer_release_locked(timer); /* 取消预取引用 */
    zb_mutex_unlock(p->timer_mtx);
}

void zn_poller_timer_cancel(zn_poller_timer *timer)
{
    if (!timer)
        return;
    zn_poller *p = timer->poller;
    if (!p || !p->timer_mtx)
        return;

    if (p->owner_thread != 0 && p->owner_thread == zb_thread_self_id()) {
        zb_mutex_lock(p->timer_mtx);
        timer->cancelled = 1;
        if (timer->heap_index != ZN_TIMER_HEAP_INDEX_NONE) {
            timer_heap_remove(p, timer);
            timer_release_locked(timer);
        }
        zb_mutex_unlock(p->timer_mtx);
        return;
    }

    /* poller 线程未运行：不会有回调并发，直接摘链。 */
    if (!zn_poller_is_running(p)) {
        zb_mutex_lock(p->timer_mtx);
        timer->cancelled = 1;
        if (timer->heap_index != ZN_TIMER_HEAP_INDEX_NONE) {
            timer_heap_remove(p, timer);
            timer_release_locked(timer);
        }
        zb_mutex_unlock(p->timer_mtx);
        return;
    }

    timer_cancel_ctx *ctx = (timer_cancel_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx)
        return;
    ctx->timer = timer;
    ctx->poller = p;
    /* 预取一个引用，避免 cancel 任务到达前定时器已被执行并释放。 */
    zb_mutex_lock(p->timer_mtx);
    timer->cancelled = 1;
    timer->refs++;
    zb_mutex_unlock(p->timer_mtx);
    if (zn_poller_async(p, timer_cancel_async, ctx, 0) != ZN_OK) {
        zb_mutex_lock(p->timer_mtx);
        timer_release_locked(timer); /* 归还预取引用 */
        zb_mutex_unlock(p->timer_mtx);
        free(ctx);
    }
}
