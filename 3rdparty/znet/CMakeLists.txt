cmake_minimum_required(VERSION 3.14)
project(znet C)

if(MSVC)
    add_compile_options(/utf-8)
endif()

set(ZN_VERSION_MAJOR 0)
set(ZN_VERSION_MINOR 8)
set(ZN_VERSION_PATCH 0)

if (NOT DEFINED ZN_BUILD_SHARED)
    option(ZN_BUILD_SHARED "Build shared library" OFF)
endif ()
if (NOT DEFINED ZN_ENABLE_EPOLL)
    option(ZN_ENABLE_EPOLL "Use epoll poller backend (Linux)" ON)
endif ()
if (NOT DEFINED ZN_ENABLE_WEPOLL)
    option(ZN_ENABLE_WEPOLL "Use wepoll poller backend (Win32)" ON)
endif ()
if (NOT DEFINED ZN_ENABLE_SELECT)
    option(ZN_ENABLE_SELECT "Use select poller backend (generic POSIX/RTOS)" ON)
endif ()
if (NOT DEFINED ZN_ALLOW_WIN32_STUB)
    option(ZN_ALLOW_WIN32_STUB "Build Win32 stub poller (no wepoll) for IDE check" OFF)
endif ()
option(ZN_DEBUG_AFFINITY "Assert poller owner-thread affinity violations" OFF)
if (NOT DEFINED ZN_ENABLE_OPENSSL)
    option(ZN_ENABLE_OPENSSL "Enable OpenSSL TLS client (zn_tls_client)" ON)
endif ()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ZNPlatform)

if (WIN32 AND NOT ZN_ALLOW_WIN32_STUB AND NOT ZN_ENABLE_WEPOLL)
    message(FATAL_ERROR
        "ZN on Windows needs -DZN_ENABLE_WEPOLL=ON (default) or "
        "-DZN_ALLOW_WIN32_STUB=ON. Full socket I/O still requires Linux/WSL.")
endif ()

if (ZN_ENABLE_EPOLL AND ZN_PLATFORM_LINUX)
    set(ZN_HAVE_EPOLL 1)
endif ()
if (ZN_ENABLE_WEPOLL AND ZN_PLATFORM_WIN32)
    set(ZN_HAVE_WEPOLL 1)
endif ()
if (ZN_ENABLE_SELECT AND ZN_PLATFORM_GENERIC AND ZN_HAVE_SELECT)
    set(ZN_HAVE_SELECT 1)
endif ()

find_package(Threads REQUIRED)

set(ZN_SOURCES
    src/core/zn_errno.c
    src/core/zn.c
    src/platform/platform.c
    src/util/log.c
    src/util/byte_buf.c
    src/util/buf.c
    src/util/mpsc.c
    src/util/timer.c
    src/net/socket.c
    src/net/tcp_server.c
    src/net/tcp_client.c
    src/net/udp_server.c
    src/net/udp_client.c
    src/poller/poller_task.c
    src/poller/poller_timer.c
    src/poller/poller_load.c
    src/poller/poller_pool.c
    src/poller/poller_buf.c
    src/thread/thread_pool.c
)

set(ZN_HAVE_OPENSSL 0)
if (ZN_ENABLE_OPENSSL)
    if (OPENSSL_ROOT_DIR)
        set(OPENSSL_ROOT_DIR "${OPENSSL_ROOT_DIR}" CACHE PATH "OpenSSL install prefix")
    endif ()
    find_package(OpenSSL QUIET)
    if (NOT OpenSSL_FOUND AND OPENSSL_ROOT_DIR)
        include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindOpenSSLWin.cmake")
    endif ()
    if (OpenSSL_FOUND)
        list(APPEND ZN_SOURCES src/net/ssl_ctx.c src/net/tls_client.c)
        set(ZN_HAVE_OPENSSL 1)
    else ()
        message(WARNING "ZN_ENABLE_OPENSSL=ON but OpenSSL not found; TLS disabled")
    endif ()
endif ()

configure_file(
    cmake/zn_config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/zn_config.h
    @ONLY
)

if (ZN_PLATFORM_LINUX)
    list(APPEND ZN_SOURCES
        src/platform/linux/platform_linux.c
        src/poller/poller_epoll.c
    )
elseif (ZN_PLATFORM_WIN32)
    list(APPEND ZN_SOURCES
        src/platform/win32/platform_win32.c
    )
    if (ZN_HAVE_WEPOLL)
        list(APPEND ZN_SOURCES
            src/platform/win32/wepoll/wepoll.c
            src/poller/win32/poller_wepoll.c
        )
    else ()
        list(APPEND ZN_SOURCES src/poller/poller_stub.c)
    endif ()
elseif (ZN_PLATFORM_GENERIC)
    if (ZN_HAVE_SELECT)
        list(APPEND ZN_SOURCES src/poller/poller_select.c)
    else ()
        list(APPEND ZN_SOURCES src/poller/poller_stub.c)
    endif ()
endif ()

if (ZN_BUILD_SHARED)
    add_library(znet SHARED ${ZN_SOURCES})
    target_compile_definitions(znet PRIVATE ZN_EXPORTS)
else ()
    if (NOT TARGET zbase)
    # 独立构建 znet 时自动带入同级 zbase；仓库整体构建由根 CMake 先 add_subdirectory。
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../zbase" "${CMAKE_CURRENT_BINARY_DIR}/zbase")
endif()
add_library(znet STATIC ${ZN_SOURCES})
endif ()

target_include_directories(znet
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/poller
)

target_compile_definitions(znet
    PRIVATE
        $<$<BOOL:${ZN_BUILD_SHARED}>:ZN_BUILD_SHARED>
        $<$<BOOL:${ZN_DEBUG_AFFINITY}>:ZN_DEBUG_AFFINITY>
)

target_link_libraries(znet PUBLIC zbase)
target_link_libraries(znet PUBLIC Threads::Threads)
if (ZN_HAVE_WEPOLL)
    target_include_directories(znet PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win32/wepoll"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win32/wepoll/sys"
    )
    target_link_libraries(znet PUBLIC ws2_32)
endif ()
if (ZN_HAVE_OPENSSL)
    target_compile_definitions(znet PUBLIC ZN_HAVE_OPENSSL=1)
    target_include_directories(znet PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/net")
    target_link_libraries(znet PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif ()

set_target_properties(znet PROPERTIES
    OUTPUT_NAME znet
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

if(MSVC)
    target_compile_definitions(znet PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
endif()

if (NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(ZN_HAVE_EPOLL    ${ZN_HAVE_EPOLL}    PARENT_SCOPE)
    set(ZN_HAVE_WEPOLL   ${ZN_HAVE_WEPOLL}   PARENT_SCOPE)
    set(ZN_HAVE_SELECT   ${ZN_HAVE_SELECT}    PARENT_SCOPE)
    set(ZN_HAVE_OPENSSL  ${ZN_HAVE_OPENSSL}   PARENT_SCOPE)
    set(ZN_PLATFORM_LINUX   ${ZN_PLATFORM_LINUX}   PARENT_SCOPE)
    set(ZN_PLATFORM_WIN32   ${ZN_PLATFORM_WIN32}   PARENT_SCOPE)
    set(ZN_PLATFORM_GENERIC ${ZN_PLATFORM_GENERIC} PARENT_SCOPE)
endif ()

install(TARGETS znet
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/zn DESTINATION include)
