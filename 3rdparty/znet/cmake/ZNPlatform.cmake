# 平台检测：设置 ZN_PLATFORM_* 与可选特性
if (CMAKE_SYSTEM_NAME MATCHES "Linux|Android")
    set(ZN_PLATFORM_LINUX 1)
    set(ZN_PLATFORM_NAME "linux")
elseif (WIN32)
    set(ZN_PLATFORM_WIN32 1)
    set(ZN_PLATFORM_NAME "win32")
else ()
    set(ZN_PLATFORM_GENERIC 1)
    set(ZN_PLATFORM_NAME "generic")
endif ()

if (ZN_PLATFORM_LINUX)
    set(ZN_ENABLE_EPOLL ON)
endif ()

include(CheckSymbolExists)
check_symbol_exists(pipe2 "unistd.h" ZN_HAVE_PIPE2)
check_symbol_exists(sem_init "semaphore.h" ZN_HAVE_POSIX_SEM)

if (ZN_PLATFORM_GENERIC)
    check_symbol_exists(select "sys/select.h" ZN_HAVE_SELECT)
endif ()
