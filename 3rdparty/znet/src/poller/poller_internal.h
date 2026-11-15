#ifndef ZN_POLLER_INTERNAL_H
#define ZN_POLLER_INTERNAL_H

#include "zb/os/sync.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

typedef struct zn_poller zn_poller;

typedef void (*zn_poller_task_fn)(void *user);
typedef uint64_t (*zn_poller_delay_cb)(void *user);

typedef struct zn_poller_task {
    zn_poller_task_fn fn;
    void *user;
    struct zn_poller_task *next;
} zn_poller_task;

struct zn_poller_timer {
    zn_poller *poller;
    zn_poller_delay_cb cb;
    void *user;
    int cancelled;
    /** 引用计数（timer_mtx 保护）：堆持有 1，跨线程取消预取 1。 */
    int refs;
    uint64_t deadline_ms;
    size_t heap_index;
    /** 引用归零（定时器已彻底停用）时调用，函数内不得回调本模块 API。 */
    void (*on_release)(void *user);
};

typedef struct zn_poller_timer zn_poller_timer;

typedef struct zn_fd_entry zn_fd_entry;

typedef struct zn_poller_load_state {
    zb_mutex *mtx;
    uint64_t last_ms;
    uint64_t run_ms;
    uint64_t sleep_ms;
    int sleeping;
} zn_poller_load_state;

struct zn_buf_pool;

struct zn_poller {
    intptr_t epfd;
    int wake_r;
    int wake_w;
    zn_fd_entry *entries;
    /** 保护 entries 链表：允许其它 poller 线程 add/mod（如 accept 后迁移会话） */
    zb_mutex *entry_mtx;

    /** 可选：per-poller 字节池（attach 挂载；poller 不拥有生命周期；建议 thread_safe=0） */
    struct zn_buf_pool *buf_pool;

    zb_mutex *task_mtx;
    zn_poller_task *task_head;
    zn_poller_task *task_tail;
    uint64_t owner_thread;

    zb_mutex *timer_mtx;
    struct zn_poller_timer **timer_heap;
    size_t timer_heap_count;
    size_t timer_heap_cap;

    zn_poller_load_state load;
};

void zn_poller_task_init(struct zn_poller *p);
void zn_poller_task_fini(struct zn_poller *p);

void zn_poller_timer_init(struct zn_poller *p);
void zn_poller_timer_fini(struct zn_poller *p);

void zn_poller_drain_tasks(struct zn_poller *p);

/** 计算 epoll_wait 超时（毫秒），-1 表示无限 */
int zn_poller_resolve_timeout_ms(struct zn_poller *p, int request_ms);

zn_err_t zn_poller_async_impl(struct zn_poller *p, zn_poller_task_fn fn, void *user, int may_sync,
                                  int push_front);

void zn_poller_timer_insert_locked(zn_poller *p, zn_poller_timer *timer, uint64_t deadline_ms);

void zn_poller_load_init(zn_poller *p);
void zn_poller_load_fini(zn_poller *p);
void zn_poller_load_on_sleep(zn_poller *p);
void zn_poller_load_on_wake(zn_poller *p);
int zn_poller_load_percent(zn_poller *p);

#endif /* ZN_POLLER_INTERNAL_H */
