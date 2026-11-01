#ifndef RTPS_ERROR_H
#define RTPS_ERROR_H

/*
 * Status codes returned by the rtps_* interface. Values are stable; mapping
 * from the framework's internal error codes happens at the API boundary so
 * callers never need those headers.
 */

#include <stdbool.h>

#include "rtps_version.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rtps_error {
    RTPS_OK = 0,
    RTPS_ERR_INVALID = -1,      /* bad argument or malformed descriptor */
    RTPS_ERR_NOMEM = -2,        /* allocation failed */
    RTPS_ERR_NOTFOUND = -3,     /* no such entity or match */
    RTPS_ERR_TIMEOUT = -4,      /* deadline expired */
    RTPS_ERR_IO = -5,           /* socket or transport failure */
    RTPS_ERR_STATE = -6,        /* wrong lifecycle state or closed handle */
    RTPS_ERR_AGAIN = -7,        /* temporary condition; retry may succeed */
    RTPS_ERR_TOOBIG = -8,       /* sample, name or list exceeds a limit */
    RTPS_ERR_CLOSED = -9,       /* handle was already destroyed */
    RTPS_ERR_ALREADY = -10,     /* entry exists or operation already started */
    RTPS_ERR_UNSUPPORTED = -11, /* feature not implemented */
    RTPS_ERR_CANCELED = -12,    /* operation canceled */
    RTPS_ERR_OVERLOAD = -13,    /* resource limit reached; retry later */
    RTPS_ERR_BAD_MESSAGE = -14, /* wire data did not decode */
    RTPS_ERR_BUSY = -15,        /* object still in use */
    RTPS_ERR_PRECONDITION = -16,/* topic/type mismatch, wrong entity kind */
    RTPS_ERR_INTERNAL = -17     /* invariant violated */
} rtps_error_t;

/* Stable symbolic text, for example "RTPS_ERR_TIMEOUT". Never NULL. */
RTPS_API const char *rtps_error_name(int error);
/* Human-readable description. Never NULL. */
RTPS_API const char *rtps_strerror(int error);
/* True when retrying the same operation may succeed unchanged. */
RTPS_API bool rtps_error_is_retryable(int error);

#ifdef __cplusplus
}
#endif

#endif /* RTPS_ERROR_H */
