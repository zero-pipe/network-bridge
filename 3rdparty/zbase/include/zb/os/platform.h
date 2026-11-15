/**
 * @file platform.h
 * @brief 跨平台基础能力：时钟 / 休眠 / 线程标识。
 */
#ifndef ZB_OS_PLATFORM_H
#define ZB_OS_PLATFORM_H

#include "zb/zb_export.h"
#include "zb/zb_errno.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 单调时钟，毫秒；仅用于间隔计算 */
ZB_API uint64_t zb_monotonic_ms(void);

/** 墙上时钟，毫秒（Unix epoch） */
ZB_API uint64_t zb_wall_ms(void);

ZB_API void zb_sleep_ms(unsigned int ms);

/** 当前线程 ID（仅日志/调试，不保证跨平台可比） */
ZB_API uint64_t zb_thread_self_id(void);

#ifdef __cplusplus
}
#endif

#endif /* ZB_OS_PLATFORM_H */
