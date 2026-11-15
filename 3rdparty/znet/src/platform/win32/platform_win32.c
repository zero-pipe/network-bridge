#include "zn/platform.h"
#include "zn_config.h"
#include "zb/os/platform.h"
#include "../../net/internal/sock_platform.h"
#include <stdint.h>
#include <windows.h>

static zn_platform_info_t s_info = {
    .name = "win32",
    .poller_epoll = 0,
    .poller_iocp = 0
};

void zn_platform_init(void)
{
    if (zn_sockplat_init() == ZN_OK) {
#if defined(ZN_HAVE_WEPOLL)
        s_info.poller_epoll = 1;
#endif
    }
}

void zn_platform_fini(void)
{
    zn_sockplat_fini();
}

void zn_platform_get_info(zn_platform_info_t *out)
{
    if (out)
        *out = s_info;
}

uint64_t zn_monotonic_ms(void)
{
    return zb_monotonic_ms();
}

void zn_sleep_ms(unsigned int ms)
{
    zb_sleep_ms(ms);
}

