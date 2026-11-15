#include "zn/platform.h"
#include "zn_config.h"
#include "zb/os/platform.h"
#include <pthread.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static zn_platform_info_t s_info = {
    .name = "linux",
    .poller_epoll = 0,
    .poller_iocp = 0
};

void zn_platform_init(void)
{
#if defined(ZN_HAVE_EPOLL)
    s_info.poller_epoll = 1;
#endif
}

void zn_platform_fini(void)
{
}

void zn_platform_get_info(zn_platform_info_t *out)
{
    if (!out)
        return;
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

