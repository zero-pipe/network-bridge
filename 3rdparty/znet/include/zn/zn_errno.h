#ifndef ZN_ERRNO_H
#define ZN_ERRNO_H

#include "zn_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zn_err {
    ZN_OK = 0,
    ZN_ERR_NOMEM = -1,
    ZN_ERR_INVALID = -2,
    ZN_ERR_IO = -3,
    ZN_ERR_TIMEOUT = -4,
    ZN_ERR_AGAIN = -5,
    ZN_ERR_NOT_IMPL = -6,
    ZN_ERR_PLATFORM = -7,
    ZN_ERR_STATE = -8,
    ZN_ERR_BUFFER_TOO_SMALL = -9,
    /** 鉴权失败（token 验签不通过） */
    ZN_ERR_DENIED = -10
} zn_err_t;

/** 线程局部或全局最后一次错误的补充描述（只读，勿 free） */
ZN_API const char *zn_strerror(zn_err_t err);
ZN_API void zn_set_last_error(const char *msg);
ZN_API const char *zn_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* ZN_ERRNO_H */
