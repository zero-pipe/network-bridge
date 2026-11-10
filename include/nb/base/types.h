#ifndef NB_BASE_TYPES_H
#define NB_BASE_TYPES_H

/*
 * Model-neutral value types shared by both spines: status codes, log levels,
 * opaque payloads and the API version.
 *
 * RPC-only types (metadata, streams, request/response views, transport kinds,
 * node modes, call handles) live in <nb/rpc/types.h>.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <nb/base/error.h>
#include <nb/base/buf.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef NB_DEPRECATED
#if defined(__GNUC__) || defined(__clang__)
#define NB_DEPRECATED(msg) __attribute__((deprecated(msg)))
#elif defined(_MSC_VER)
#define NB_DEPRECATED(msg) __declspec(deprecated(msg))
#else
#define NB_DEPRECATED(msg)
#endif
#endif

/* Explicit status payload for replies: code plus a length-delimited message. */
typedef struct nb_status {
    int code;
    const char *message;
    size_t message_len;
} nb_status_t;

/* Log severity; monotonically increasing (0=assert .. 5=verbose). */
typedef enum nb_log_level {
    NB_LOG_LEVEL_ASSERT = 0,
    NB_LOG_LEVEL_ERROR = 1,
    NB_LOG_LEVEL_WARN = 2,
    NB_LOG_LEVEL_INFO = 3,
    NB_LOG_LEVEL_DEBUG = 4,
    NB_LOG_LEVEL_VERBOSE = 5
} nb_log_level_t;

/* Explicit "use the default level" marker for zero-initialized configs. */
#define NB_LOG_LEVEL_DEFAULT ((nb_log_level_t)-1)

/* ---- Payload views ---- */
/*
 * Borrowed application payload (opaque to the framework). data/len are raw
 * bytes; encoding is an application-defined tag (0 = raw bytes). Payloads passed
 * to callbacks are valid only for that callback.
 */
typedef struct nb_payload {
    const void *data;
    size_t len;
    uint8_t encoding;
} nb_payload_t;

/*
 * Owned payload: buf holds reference-counted memory and offset/len select a
 * view into it. Owned-payload APIs take ownership of buf at entry, even when
 * they return an error.
 */
typedef struct nb_owned_payload {
    nb_buf_t *buf;
    size_t offset;
    size_t len;
    uint8_t encoding;
} nb_owned_payload_t;

void nb_owned_payload_init(nb_owned_payload_t *payload,
                           nb_buf_t *buf,
                           size_t offset,
                           size_t len,
                           uint8_t encoding);
nb_buf_t *nb_owned_payload_take(nb_owned_payload_t *payload);
void nb_owned_payload_release(nb_owned_payload_t *payload);
nb_payload_t nb_owned_payload_view(const nb_owned_payload_t *payload);

/* ---- Version ---- */
#define NB_VERSION_MAJOR 0
#define NB_VERSION_MINOR 5
#define NB_VERSION_PATCH 0
#define NB_VERSION_STRING "0.5.0"

/* Runtime version string of the linked SDK (provided by the umbrella library). */
const char *nb_version_string(void);

/* ---- Shared limits ---- */
#define NB_NAME_MAX 64
#define NB_HOST_MAX 64 /* max host / IP text length, excluding the NUL */
#define NB_SCHEME_MAX 16

#ifdef __cplusplus
}
#endif

#endif /* NB_BASE_TYPES_H */
