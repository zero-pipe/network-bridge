#ifndef ZN_PLATFORM_H
#define ZN_PLATFORM_H

#include "zn_export.h"
#include "zn_errno.h"
#include "zb/os/platform.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zn_platform_info {
    const char *name; /* "linux", "win32", ... */
    int poller_epoll;
    int poller_iocp; /* 预留 */
} zn_platform_info_t;

ZN_API void zn_platform_init(void);
ZN_API void zn_platform_fini(void);
ZN_API void zn_platform_get_info(zn_platform_info_t *out);

/** 单调时钟，毫秒 */
ZN_API uint64_t zn_monotonic_ms(void);

ZN_API void zn_sleep_ms(unsigned int ms);

#ifdef __cplusplus
}
#endif

#endif /* ZN_PLATFORM_H */
