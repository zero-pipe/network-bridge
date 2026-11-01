/*
 * rpc 控制对象分配器（poller-local 池）。
 *
 * 基于当前 poller 的本地分档池分配定长控制对象（call / task 等）。每个对象
 * 前置一个归属头，记录来源池、owner poller 与容量：
 *   - 释放发生在 owner poller 线程：归还本地池，命中 freelist。
 *   - 释放发生在其他线程（如 nb_node_destroy）：直接 free，避免向已停止的 poller
 *     投递延迟归还任务而在池销毁后触发悬垂。
 * 池块本身由 malloc 支撑，因此跨线程直接 free 始终安全，只是放弃该次复用。
 * 没有 poller 或池耗尽时，回退到 nb_malloc/nb_free，使自定义分配器与
 * 内存统计也能覆盖该路径。
 */
#include "poller_alloc.h"

#include <nb/alloc.h>

#include <ntk.h>

#include <stdlib.h>

typedef struct nb_poller_hdr {
    ntk_buf_pool *pool;
    ntk_poller *owner;
    size_t cap;
} nb_poller_hdr_t;

void *nb_poller_alloc(size_t size)
{
    nb_poller_hdr_t *h;
    ntk_poller *owner = ntk_poller_current();
    ntk_buf_pool *pool = owner ? ntk_poller_buf_pool(owner) : NULL;
    size_t cap = 0;
    void *raw = NULL;

    if (size == 0) {
        return NULL;
    }
    if (pool) {
        raw = ntk_buf_pool_acquire(pool, sizeof(*h) + size, &cap);
    }
    if (!raw) {
        raw = nb_malloc(sizeof(*h) + size);
        if (!raw) {
            return NULL;
        }
        cap = 0;
        pool = NULL;
        owner = NULL;
    }
    h = (nb_poller_hdr_t *)raw;
    h->pool = pool;
    h->owner = owner;
    h->cap = cap;
    return (void *)(h + 1);
}

void nb_poller_free(void *ptr)
{
    nb_poller_hdr_t *h;

    if (!ptr) {
        return;
    }
    h = ((nb_poller_hdr_t *)ptr) - 1;
    if (h->pool) {
        if (h->owner && ntk_poller_is_current_thread(h->owner)) {
            ntk_buf_pool_release(h->pool, h, h->cap);
        } else {
            free(h); /* 池块由 malloc 支撑，跨线程直接释放 */
        }
    } else {
        nb_free(h); /* 回退路径由 nb_malloc 分配 */
    }
}
