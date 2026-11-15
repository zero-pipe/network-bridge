#ifndef ZN_UTIL_LOG_H
#define ZN_UTIL_LOG_H

#include "../zn_export.h"
#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zn_log_level {
    ZN_LOG_TRACE = 0,
    ZN_LOG_DEBUG,
    ZN_LOG_INFO,
    ZN_LOG_WARN,
    ZN_LOG_ERROR
} zn_log_level_t;

ZN_API void zn_log_set_level(zn_log_level_t level);
ZN_API int  zn_log_open_file(const char *path);
ZN_API void zn_log_close_file(void);
/** 关闭并重新打开当前日志文件（供 SIGHUP / logrotate 使用）。未打开文件时无操作。 */
ZN_API void zn_log_reopen_file(void);
ZN_API void zn_log_write(zn_log_level_t level, const char *file, int line, const char *fmt, ...);
ZN_API void zn_log_writev(zn_log_level_t level, const char *file, int line, const char *fmt, va_list ap);

/**
 * 配置日志文件按大小自动轮转（需在 zn_log_open_file 之后调用）。
 * @param max_bytes  单个日志文件最大字节数；0 = 不轮转。
 * @param keep_count 保留旧文件数量（.log.1 … .log.N）；建议 3-10。
 */
ZN_API void zn_log_set_rotate(size_t max_bytes, int keep_count);

#define ZN_LOG(lvl, fmt, ...) \
    zn_log_write((lvl), __FILE__, __LINE__, (fmt), ##__VA_ARGS__)

#define zn_trace(fmt, ...) ZN_LOG(ZN_LOG_TRACE, fmt, ##__VA_ARGS__)
#define zn_debug(fmt, ...) ZN_LOG(ZN_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define zn_info(fmt, ...)  ZN_LOG(ZN_LOG_INFO, fmt, ##__VA_ARGS__)
#define zn_warn(fmt, ...)  ZN_LOG(ZN_LOG_WARN, fmt, ##__VA_ARGS__)
#define zn_error(fmt, ...) ZN_LOG(ZN_LOG_ERROR, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* ZN_UTIL_LOG_H */
