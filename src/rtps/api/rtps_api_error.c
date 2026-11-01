#include "rtps_api_internal.h"

#include <stdio.h>
#include <string.h>

/*
 * Error mapping, small utilities and the sample filter helpers. Keeping the
 * mapping in one place is what lets the public headers stay free of nb_error.
 */

int rtps_api_error(int nb_status)
{
    switch (nb_status) {
    case NB_OK:
        return RTPS_OK;
    case NB_ERR_INVALID:
        return RTPS_ERR_INVALID;
    case NB_ERR_NOMEM:
        return RTPS_ERR_NOMEM;
    case NB_ERR_NOTFOUND:
        return RTPS_ERR_NOTFOUND;
    case NB_ERR_TIMEOUT:
        return RTPS_ERR_TIMEOUT;
    case NB_ERR_IO:
        return RTPS_ERR_IO;
    case NB_ERR_STATE:
        return RTPS_ERR_STATE;
    case NB_ERR_AGAIN:
        return RTPS_ERR_AGAIN;
    case NB_ERR_TOOBIG:
        return RTPS_ERR_TOOBIG;
    case NB_ERR_CLOSED:
        return RTPS_ERR_CLOSED;
    case NB_ERR_ALREADY:
        return RTPS_ERR_ALREADY;
    case NB_ERR_NOT_SUPPORTED:
        return RTPS_ERR_UNSUPPORTED;
    case NB_ERR_CANCELED:
        return RTPS_ERR_CANCELED;
    case NB_ERR_OVERLOAD:
        return RTPS_ERR_OVERLOAD;
    case NB_ERR_BAD_MESSAGE:
        return RTPS_ERR_BAD_MESSAGE;
    case NB_ERR_BUSY:
        return RTPS_ERR_BUSY;
    default:
        return RTPS_ERR_INTERNAL;
    }
}

int rtps_api_size_ok(uint32_t struct_size, size_t expected)
{
    return struct_size >= expected;
}

void rtps_api_copy(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) {
        return;
    }
    (void)snprintf(dst, cap, "%s", src ? src : "");
}

const char *rtps_error_name(int error)
{
    switch (error) {
    case RTPS_OK: return "RTPS_OK";
    case RTPS_ERR_INVALID: return "RTPS_ERR_INVALID";
    case RTPS_ERR_NOMEM: return "RTPS_ERR_NOMEM";
    case RTPS_ERR_NOTFOUND: return "RTPS_ERR_NOTFOUND";
    case RTPS_ERR_TIMEOUT: return "RTPS_ERR_TIMEOUT";
    case RTPS_ERR_IO: return "RTPS_ERR_IO";
    case RTPS_ERR_STATE: return "RTPS_ERR_STATE";
    case RTPS_ERR_AGAIN: return "RTPS_ERR_AGAIN";
    case RTPS_ERR_TOOBIG: return "RTPS_ERR_TOOBIG";
    case RTPS_ERR_CLOSED: return "RTPS_ERR_CLOSED";
    case RTPS_ERR_ALREADY: return "RTPS_ERR_ALREADY";
    case RTPS_ERR_UNSUPPORTED: return "RTPS_ERR_UNSUPPORTED";
    case RTPS_ERR_CANCELED: return "RTPS_ERR_CANCELED";
    case RTPS_ERR_OVERLOAD: return "RTPS_ERR_OVERLOAD";
    case RTPS_ERR_BAD_MESSAGE: return "RTPS_ERR_BAD_MESSAGE";
    case RTPS_ERR_BUSY: return "RTPS_ERR_BUSY";
    case RTPS_ERR_PRECONDITION: return "RTPS_ERR_PRECONDITION";
    case RTPS_ERR_INTERNAL: return "RTPS_ERR_INTERNAL";
    default: return "RTPS_ERR_UNKNOWN";
    }
}

const char *rtps_strerror(int error)
{
    switch (error) {
    case RTPS_OK: return "success";
    case RTPS_ERR_INVALID: return "invalid argument";
    case RTPS_ERR_NOMEM: return "out of memory";
    case RTPS_ERR_NOTFOUND: return "not found";
    case RTPS_ERR_TIMEOUT: return "timed out";
    case RTPS_ERR_IO: return "I/O error";
    case RTPS_ERR_STATE: return "invalid state";
    case RTPS_ERR_AGAIN: return "temporarily unavailable";
    case RTPS_ERR_TOOBIG: return "value too large";
    case RTPS_ERR_CLOSED: return "object closed";
    case RTPS_ERR_ALREADY: return "already exists or started";
    case RTPS_ERR_UNSUPPORTED: return "not supported";
    case RTPS_ERR_CANCELED: return "canceled";
    case RTPS_ERR_OVERLOAD: return "resource limit reached";
    case RTPS_ERR_BAD_MESSAGE: return "malformed message";
    case RTPS_ERR_BUSY: return "busy";
    case RTPS_ERR_PRECONDITION: return "precondition failed";
    case RTPS_ERR_INTERNAL: return "internal error";
    default: return "unknown error";
    }
}

bool rtps_error_is_retryable(int error)
{
    return error == RTPS_ERR_AGAIN || error == RTPS_ERR_BUSY ||
           error == RTPS_ERR_OVERLOAD;
}

void rtps_sample_filter_init(rtps_sample_filter_t *filter)
{
    if (!filter) {
        return;
    }
    memset(filter, 0, sizeof(*filter));
    filter->sample_state_mask = RTPS_STATE_MASK_ANY;
    filter->view_state_mask = RTPS_STATE_MASK_ANY;
    filter->instance_state_mask = RTPS_STATE_MASK_ANY;
}

void rtps_sample_filter_set_instance(rtps_sample_filter_t *filter,
                                     const rtps_instance_handle_t *handle)
{
    if (!filter) {
        return;
    }
    if (!handle) {
        filter->has_instance_handle = 0;
        memset(&filter->instance_handle, 0, sizeof(filter->instance_handle));
        return;
    }
    filter->has_instance_handle = 1;
    filter->instance_handle = *handle;
}
