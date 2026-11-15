#include "zb/os/platform.h"
#include <stdint.h>
#include <windows.h>

uint64_t zb_monotonic_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER counter;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart * 1000 / freq.QuadPart);
}

uint64_t zb_wall_ms(void)
{
    FILETIME ft;
    ULARGE_INTEGER t;
    GetSystemTimeAsFileTime(&ft);
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    /* 100ns since 1601 -> ms since 1970 */
    return (uint64_t)(t.QuadPart / 10000ULL - 11644473600000ULL);
}

void zb_sleep_ms(unsigned int ms)
{
    Sleep(ms);
}

uint64_t zb_thread_self_id(void)
{
    return (uint64_t)GetCurrentThreadId();
}
