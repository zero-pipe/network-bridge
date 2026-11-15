#ifndef ZN_UTIL_TIMER_H
#define ZN_UTIL_TIMER_H

#include "../zn_export.h"
#include "../zn_errno.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zn_poller;
typedef struct zn_poller zn_poller;
typedef struct zn_poller_timer zn_poller_timer;
typedef struct zn_timer zn_timer;

/**
 * 延迟任务回调：返回下次间隔（毫秒），0 表示结束。
 */
typedef uint64_t (*zn_poller_delay_cb)(void *user);

/** 在 poller 线程触发；跨线程投递用 async 入队 */
ZN_API zn_poller_timer *zn_poller_do_delay(zn_poller *p, uint64_t delay_ms,
                                                 zn_poller_delay_cb cb, void *user);

ZN_API void zn_poller_timer_cancel(zn_poller_timer *timer);

/**
 * 设置定时器引用归零（取消已生效、在途回调也已结束）时的释放回调。
 * 供 zn_timer 包装层延迟释放自身对象；fn 内不得回调本模块 API。
 */
ZN_API void zn_poller_timer_set_on_release(zn_poller_timer *timer, void (*fn)(void *user));

/** 固定间隔定时器（repeat=0 表示单次） */
typedef void (*zn_timer_cb)(void *user);

ZN_API zn_timer *zn_timer_start(zn_poller *p, uint64_t interval_ms, int repeat,
                                      zn_timer_cb cb, void *user);
ZN_API void zn_timer_stop(zn_timer *timer);

/** Wall-clock milliseconds since Unix epoch (shared with ZMS play/RTCP clocks). */
ZN_API uint64_t zn_wall_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* ZN_UTIL_TIMER_H */
