#ifndef NB_POLLER_ALLOC_H
#define NB_POLLER_ALLOC_H

#include <stddef.h>

/*
 * 线程本地控制对象分配器（poller-local）。
 *
 * 基于当前线程的本地分档池（nb_buf_alloc_local）分配定长控制对象（call / task
 * 等），池耗尽或无分档可容纳时回退到全局分配器（nb_buf_alloc → nb_malloc），
 * 使自定义分配器与内存统计也能覆盖该路径。
 *
 * 线程约定：每个对象前置一个归属头记录来源 nb_buf。
 *   - 在分配所在线程释放：归还本地池，命中 freelist。
 *   - 在其他线程释放：nb_buf 的本地池直接 free 该块，避免向已停止的线程回投，
 *     也绝不会踩无锁 freelist（池块由 malloc 支撑，跨线程 free 始终安全）。
 *
 * 本模块不依赖任何框架（ntk 等），仅依赖 nb_buf，可独立使用。
 */
void *nb_poller_alloc(size_t size);
void nb_poller_free(void *ptr);

#endif /* NB_POLLER_ALLOC_H */
