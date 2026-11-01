#ifndef NB_POLLER_ALLOC_H
#define NB_POLLER_ALLOC_H

#include <stddef.h>

/*
 * poller-local 控制对象分配器：优先用当前 poller 的本地分档池分配定长控制对象
 * （call / task 等），无 poller 或池耗尽时回退到 base 的 nb_malloc/nb_free
 * （全局分配器 + 统计）。
 *
 * 它不是"池"本身：池来自 poller 的 ntk_buf_pool，本模块只负责在分配对象前挂
 * 归属头、在释放时决定归还本地池还是跨线程直接 free。命名与 base 的 alloc.h
 * （nb_malloc/nb_free）刻意区分。
 */
void *nb_poller_alloc(size_t size);
void nb_poller_free(void *ptr);

#endif /* NB_POLLER_ALLOC_H */
