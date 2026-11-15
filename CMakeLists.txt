cmake_minimum_required(VERSION 3.14)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
project(network_bridge
    VERSION 0.5.0
    DESCRIPTION "Heterogeneous network communication framework (RPC, pub/sub, streaming)"
    HOMEPAGE_URL "https://github.com/zero-pipe/network_bridge"
    LANGUAGES C)

include(GNUInstallDirs)

if(MSVC)
    add_compile_options(/utf-8)
    # Bundled Windows GoogleTest archives use the static MSVC runtime.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

enable_testing()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 基础依赖：znet（网络/线程/定时器）+ zbase（OS 抽象与容器）。
# 关闭 OpenSSL：本仓库不需要 TLS，省掉外部依赖。
if(WIN32)
    # 防止 windows.h 拉入旧 winsock.h，与 znet 的 winsock2.h 定义冲突。
    add_compile_definitions(WIN32_LEAN_AND_MEAN)
endif()
set(ZN_ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/znet)
add_subdirectory(3rdparty/elog)

# Micro-CDR: vendored CDR (de)serialization used by the RTPS codec.
add_subdirectory(3rdparty/micro-cdr)

# Public options use the NB_ prefix so they can be embedded with
# add_subdirectory() without colliding with the parent project.
option(NB_BUILD_TESTS "Build tests" ON)
option(NB_BUILD_TOOLS "Build benchmark tools" ON)
option(NB_BUILD_EXAMPLES "Build examples" ON)
option(NB_BUILD_GTESTS "Build GoogleTest module tests" OFF)
option(NB_BUILD_BENCHMARKS "Build Google Benchmark microbenchmarks" OFF)

set(NB_SANITIZE "" CACHE STRING "Sanitizer set: address|undefined|thread|address,undefined")
if(NB_SANITIZE)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        message(FATAL_ERROR
            "NB_SANITIZE=${NB_SANITIZE} is not supported by MSVC; "
            "use GCC/Clang (e.g. on Linux) or clang-cl for sanitizer builds.")
    endif()
    add_compile_options(-fsanitize=${NB_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${NB_SANITIZE})
endif()

# 纯静态依赖，没有运行库要拷贝；保留该钩子供测试/示例调用。
function(nb_copy_runtime_deps target)
endfunction()

if(NB_BUILD_GTESTS OR NB_BUILD_BENCHMARKS)
    enable_language(CXX)
endif()

add_subdirectory(src)

if(NB_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(NB_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(NB_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(NB_BUILD_GTESTS OR NB_BUILD_BENCHMARKS)
    add_subdirectory(tests/google)
endif()

# Unified quality gates. Build the target first, then the gate runs:
#   cmake --build build --target gate-unit
#   cmake --build build --target gate-e2e
#   cmake --build build --target gate-perf
#   cmake --build build --target gate-stability
#   cmake --build build --target gate-all
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    set(NB_GATE_SCRIPT ${CMAKE_SOURCE_DIR}/tools/gate.py)
    set(NB_GATE_ARGS --build-dir ${CMAKE_BINARY_DIR} --no-build)
    if(CMAKE_CONFIGURATION_TYPES)
        list(APPEND NB_GATE_ARGS --config $<CONFIG>)
    endif()

    if(TARGET test_framing)
        add_custom_target(gate-unit
            COMMAND ${Python3_EXECUTABLE} ${NB_GATE_SCRIPT} unit ${NB_GATE_ARGS}
            DEPENDS test_framing test_config test_metrics test_registry
                    test_reliable test_poller_alloc test_iobuf test_headers test_buffer
                    test_buf_concurrency test_slotpool test_poolset test_contracts
            USES_TERMINAL VERBATIM)
    endif()

    if(TARGET bench_server AND TARGET loadgen AND TARGET test_loopback)
        add_custom_target(gate-e2e
            COMMAND ${Python3_EXECUTABLE} ${NB_GATE_SCRIPT} e2e ${NB_GATE_ARGS}
            DEPENDS test_loopback test_stream test_api test_worker test_semantics
                    test_move bench_server loadgen
                    $<$<TARGET_EXISTS:test_dds_local>:test_dds_local>
                    $<$<TARGET_EXISTS:test_dds_remote>:test_dds_remote>
            USES_TERMINAL VERBATIM)
    endif()

    if(TARGET bench_server AND TARGET loadgen)
        add_custom_target(gate-perf
            COMMAND ${Python3_EXECUTABLE} ${NB_GATE_SCRIPT} perf ${NB_GATE_ARGS}
            DEPENDS bench_server loadgen
            USES_TERMINAL VERBATIM)
        add_custom_target(gate-stability
            COMMAND ${Python3_EXECUTABLE} ${NB_GATE_SCRIPT} stability ${NB_GATE_ARGS}
            DEPENDS bench_server loadgen
            USES_TERMINAL VERBATIM)
    endif()

    if(TARGET gate-unit AND TARGET gate-e2e AND TARGET gate-perf AND TARGET gate-stability)
        add_custom_target(gate-all
            COMMAND ${Python3_EXECUTABLE} ${NB_GATE_SCRIPT} all ${NB_GATE_ARGS}
            DEPENDS test_framing test_config test_metrics test_registry
                    test_reliable test_poller_alloc test_iobuf test_headers test_buffer
                    test_buf_concurrency test_slotpool test_poolset test_contracts
                    test_loopback test_stream test_api test_move test_worker test_semantics
                    bench_server loadgen
            USES_TERMINAL VERBATIM)
    endif()
endif()
