/**
 * @file zb_errno.h
 * @brief zbase 公共错误码（数值与 znet 保持兼容）。
 */
#ifndef ZB_ERRNO_H
#define ZB_ERRNO_H

#include "zb_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zb_err {
    ZB_OK = 0,
    ZB_ERR_NOMEM = -1,
    ZB_ERR_INVALID = -2,
    ZB_ERR_IO = -3,
    ZB_ERR_TIMEOUT = -4,
    ZB_ERR_AGAIN = -5,
    ZB_ERR_NOT_IMPL = -6,
    ZB_ERR_PLATFORM = -7,
    ZB_ERR_STATE = -8,
    ZB_ERR_BUFFER_TOO_SMALL = -9
} zb_err_t;

#ifdef __cplusplus
}
#endif

#endif /* ZB_ERRNO_H */
