# Curated C base library (ireader/sdk subset, MIT).
# zbase = OSAL (thread/mutex/sem/time) + containers/string/codec/URI/time.
# znet (poller/socket/TLS) depends on this layer; zbase must not include zn/*.

set(ZB_ROOT "${CMAKE_CURRENT_LIST_DIR}")

include(CheckSymbolExists)

set(ZB_SOURCES
    ${ZB_ROOT}/src/darray.c
    ${ZB_ROOT}/src/hashmap.c
    ${ZB_ROOT}/src/rbtree.c
    ${ZB_ROOT}/src/heap.c
    ${ZB_ROOT}/src/bitmap.c
    ${ZB_ROOT}/src/bits.c
    ${ZB_ROOT}/src/bsearch.c
    ${ZB_ROOT}/src/hweight.c
    ${ZB_ROOT}/src/base64.c
    ${ZB_ROOT}/src/uri-parse.c
    ${ZB_ROOT}/src/uri-query.c
    ${ZB_ROOT}/src/urlcodec.c
    ${ZB_ROOT}/src/time64.c
    ${ZB_ROOT}/src/ntp-time.c
    ${ZB_ROOT}/src/string/strsplit.c
    ${ZB_ROOT}/src/string/strtrim.c
    ${ZB_ROOT}/src/string/strtoken.c
    ${ZB_ROOT}/src/digest/crc32.c
    ${ZB_ROOT}/src/digest/md5.c
    ${ZB_ROOT}/src/digest/sha.c
    ${ZB_ROOT}/src/digest/sha1.c
    ${ZB_ROOT}/src/digest/sha224-256.c
    ${ZB_ROOT}/src/digest/sha384-512.c
    ${ZB_ROOT}/src/digest/hmac.c
    ${ZB_ROOT}/src/digest/hkdf.c
)

if(WIN32)
    list(APPEND ZB_SOURCES
        ${ZB_ROOT}/src/string/strndup.c
        ${ZB_ROOT}/src/os/win32/zb_platform_win32.c
        ${ZB_ROOT}/src/os/win32/zb_thread_win32.c
        ${ZB_ROOT}/src/os/win32/zb_thread_priority_win32.c
        ${ZB_ROOT}/src/os/win32/zb_sync_win32.c
        ${ZB_ROOT}/src/os/win32/zb_sem_win32.c
        ${ZB_ROOT}/src/os/win32/zb_sock_win32.c)
else()
    list(APPEND ZB_SOURCES
        ${ZB_ROOT}/src/string/strrev.c
        ${ZB_ROOT}/src/os/linux/zb_platform_linux.c
        ${ZB_ROOT}/src/os/linux/zb_thread_pthread.c
        ${ZB_ROOT}/src/os/linux/zb_thread_priority_linux.c
        ${ZB_ROOT}/src/os/linux/zb_sync_pthread.c
        ${ZB_ROOT}/src/os/linux/zb_sem_pthread.c
        ${ZB_ROOT}/src/os/linux/zb_sock_linux.c)
endif()
if(NOT APPLE)
    # macOS libc already provides strlcpy/strlcat.
    list(APPEND ZB_SOURCES
        ${ZB_ROOT}/src/string/strlcpy.c
        ${ZB_ROOT}/src/string/strlcat.c)
endif()

if(UNIX)
    check_symbol_exists(sem_init "semaphore.h" ZB_HAVE_POSIX_SEM)
endif()
configure_file(
    ${ZB_ROOT}/cmake/zb_config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/zb_config.h
    @ONLY)

find_package(Threads REQUIRED)

add_library(zbase STATIC ${ZB_SOURCES})
target_include_directories(zbase
    PUBLIC
        ${ZB_ROOT}/include
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated)

# 平台宏必须对依赖方可见：zb/os 头文件按 OS_WINDOWS/OS_LINUX 选择平台实现
if(WIN32)
    target_compile_definitions(zbase PUBLIC OS_WINDOWS=1)
elseif(APPLE)
    target_compile_definitions(zbase PUBLIC OS_MAC=1)
else()
    target_compile_definitions(zbase PUBLIC OS_LINUX=1)
endif()

target_link_libraries(zbase PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(zbase PUBLIC ws2_32)
endif()

if(UNIX AND NOT APPLE)
    # darray uses sqrt(); static libs do not propagate libm automatically.
    target_link_libraries(zbase PUBLIC m)
endif()

set_target_properties(zbase PROPERTIES
    OUTPUT_NAME zbase
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(zbase PRIVATE /wd4018 /wd4996 /wd4244)
else()
    target_compile_options(zbase PRIVATE -w)
endif()
