#ifndef NB_LOG_H
#define NB_LOG_H

/*
 * Logging facade. nb_log_* short-circuit when logging is disabled; the backend
 * stays an implementation detail, so consumers only include this header and do
 * not depend on a third-party logger. nb_node_create installs the configured
 * switch and level.
 */

#include <nb/types.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the backend on first use; thread-safe and idempotent. */
void nb_log_init(void);

/*
 * Runtime switch: when enabled is false every nb_log_* macro short-circuits
 * without formatting or output. level is an nb_log_level_t (0=assert ..
 * 5=verbose). nb_node_create calls this once from the log configuration.
 */
void nb_log_configure(bool enabled, nb_log_level_t level);

/* Whether logging is currently enabled; the first call initializes the backend. */
bool nb_log_enabled(void);

/*
 * Emit one log record. Use the nb_log_* macros instead of calling this
 * directly. The formatted message is truncated at an internal fixed-size
 * buffer (768 bytes including the terminator).
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 3, 4)))
#endif
void nb_log_write(nb_log_level_t level, const char *tag, const char *format, ...);

#ifdef __cplusplus
}
#endif

#ifndef NB_LOG_TAG
#define NB_LOG_TAG "nb"
#endif

#define NB_LOG_AT_(level_, ...)                           \
    do {                                                  \
        if (nb_log_enabled()) {                           \
            nb_log_write((level_), NB_LOG_TAG, __VA_ARGS__); \
        }                                                 \
    } while (0)

#define nb_log_debug(...) NB_LOG_AT_(NB_LOG_LEVEL_DEBUG, __VA_ARGS__)
#define nb_log_info(...)  NB_LOG_AT_(NB_LOG_LEVEL_INFO, __VA_ARGS__)
#define nb_log_warn(...)  NB_LOG_AT_(NB_LOG_LEVEL_WARN, __VA_ARGS__)
#define nb_log_error(...) NB_LOG_AT_(NB_LOG_LEVEL_ERROR, __VA_ARGS__)

#endif /* NB_LOG_H */
