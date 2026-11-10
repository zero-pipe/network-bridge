set(NB_BASE_SOURCES
    base/alloc.c
    base/buf.c
    base/poolset.c
    base/slotpool.c
    base/iobuf.c
    base/poller_alloc.c
    base/crc32.c
    base/error.c
    base/metrics.c
    base/payload.c
)

add_library(nb_base STATIC ${NB_BASE_SOURCES})
add_library(nb::base ALIAS nb_base)

target_include_directories(nb_base
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_SOURCE_DIR}/3rdparty/cjson
)

set_target_properties(nb_base PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    OUTPUT_NAME nb_base
)

install(TARGETS nb_base
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Shared JSON reader for both config parsers (cJSON lives here once).
add_library(nb_json STATIC base/json.c ${CMAKE_SOURCE_DIR}/3rdparty/cjson/cJSON.c)
add_library(nb::json ALIAS nb_json)
target_include_directories(nb_json
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/base
        ${CMAKE_SOURCE_DIR}/3rdparty/cjson
)
target_link_libraries(nb_json PUBLIC nb_base PRIVATE nb_log)
set_target_properties(nb_json PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    OUTPUT_NAME nb_json)
install(TARGETS nb_json
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

set(NB_RPC_SOURCES
    # RPC runtime: node lifecycle, inbound dispatch, binding assembly
    rpc/runtime/node.c
    rpc/runtime/node_access.c
    rpc/runtime/transport_factory.c
    rpc/runtime/dispatch.c
    rpc/runtime/codec.c
    rpc/runtime/metadata.c
    rpc/endpoint.c
    rpc/message_view.c
    rpc/util.c
    # RPC spine
    rpc/service.c
    rpc/service_dispatch.c
    rpc/service_stream.c
    rpc/call_cancel.c
    rpc/proxy.c
    rpc/stub.c
    rpc/reply.c
    rpc/loan.c
    rpc/stream.c
    rpc/service_table.c
    rpc/sync.c
    rpc/pending.c
    rpc/dedup.c
    rpc/message/frame.c
    rpc/message/envelope.c
    rpc/router/router.c
    rpc/registry/registry.c
    rpc/discovery/discovery.c
    rpc/config/rpc_config.c
    rpc/config/rpc_config_defaults.c
    rpc/transport/common/framing.c
    rpc/transport/common/transport.c
    rpc/transport/common/provider.c
    rpc/transport/common/rx_window.c
    rpc/transport/common/reasm.c
    rpc/transport/common/tx_ring.c
    rpc/transport/udp/udp.c
    rpc/transport/tcp/tcp.c
    rpc/transport/shm/shm.c
    rpc/transport/shm/shm_segment.c
)

set(NB_DDS_SOURCES
    dds/sample_queue.c
    dds/dds_qos.c
    dds/dds_backend.c
    dds/dds_participant.c
    dds/dds_topic.c
    dds/dds_writer.c
    dds/dds_reader.c
    dds/dds_waitset.c
    dds/rtps_backend.c
    dds/config/dds_config.c
)

if(NB_ENABLE_DDS)
    list(APPEND NB_DDS_SOURCES
        dds/cyclone_backend.c
        dds/cyclone/cy_dds_message.c)
endif()

# ---- RTPS engine and public API -----------------------------------------
# log.c is shared by the framework and the RTPS engine. Keeping it in a small
# library lets the engine build without pulling in the framework: rtps_core
# depends on nb_base, ntk, microcdr and nb_log only.
add_library(nb_log STATIC base/log.c)
target_include_directories(nb_log
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/base)
target_link_libraries(nb_log PRIVATE elog)
set_target_properties(nb_log PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    OUTPUT_NAME nb_log)
install(TARGETS nb_log
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Standalone discovery library: depends on ntk only.
add_library(ntk_discovery STATIC ntk_discovery/ntk_discovery.c)
add_library(nb::ntk_discovery ALIAS ntk_discovery)
target_include_directories(ntk_discovery
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(ntk_discovery PUBLIC ntk)
set_target_properties(ntk_discovery PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    OUTPUT_NAME ntk_discovery)
install(TARGETS ntk_discovery
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

set(NB_RTPS_CORE_SOURCES
    rtps/rtps_types.c
    rtps/rtps_cdr.c
    rtps/rtps_param.c
    rtps/rtps_message.c
    rtps/rtps_spdp.c
    rtps/rtps_sedp.c
    rtps/rtps_data.c
    rtps/rtps_reliable.c
    rtps/rtps_tcp.c
    rtps/rtps_participant.c
)
add_library(rtps_core STATIC ${NB_RTPS_CORE_SOURCES})
add_library(nb::rtps_core ALIAS rtps_core)
target_include_directories(rtps_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/rtps/internal
        ${CMAKE_CURRENT_SOURCE_DIR}/base)
target_link_libraries(rtps_core
    PUBLIC ntk nb_base microcdr
    PRIVATE nb_log)
set_target_properties(rtps_core PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    OUTPUT_NAME rtps_core)
install(TARGETS rtps_core
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

set(NB_RTPS_API_SOURCES
    rtps/api/rtps_api_error.c
    rtps/api/rtps_api_qos.c
    rtps/api/rtps_api_entity.c
    rtps/api/rtps_api_participant.c
    rtps/api/rtps_api_topic.c
    rtps/api/rtps_api_writer.c
    rtps/api/rtps_api_reader.c
    rtps/api/rtps_api_history.c
    rtps/api/rtps_api_waitset.c
)
add_library(rtps STATIC ${NB_RTPS_API_SOURCES})
add_library(nb::rtps ALIAS rtps)
target_include_directories(rtps
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/rtps/internal)
target_link_libraries(rtps
    PUBLIC rtps_core ntk nb_base
    PRIVATE nb_log)
set_target_properties(rtps PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    OUTPUT_NAME rtps)
install(TARGETS rtps
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_library(nb_rpc STATIC ${NB_RPC_SOURCES})
add_library(nb::rpc ALIAS nb_rpc)
target_include_directories(nb_rpc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/base
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/runtime
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/message
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/router
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/registry
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/discovery
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/transport/common
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/transport/tcp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/transport/udp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/transport/shm
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/config
        ${CMAKE_CURRENT_SOURCE_DIR}/utils
        ${CMAKE_SOURCE_DIR}/3rdparty/uthash
        ${CMAKE_SOURCE_DIR}/3rdparty/cjson
)
target_link_libraries(nb_rpc PUBLIC ntk nb_base PRIVATE nb_json nb_log ntk_discovery)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(nb_rpc PUBLIC rt)
endif()
set_target_properties(nb_rpc PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    OUTPUT_NAME nb_rpc)
install(TARGETS nb_rpc
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_library(nb_dds STATIC ${NB_DDS_SOURCES})
add_library(nb::dds ALIAS nb_dds)
target_include_directories(nb_dds
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/base
        ${CMAKE_CURRENT_SOURCE_DIR}/dds
        ${CMAKE_CURRENT_SOURCE_DIR}/dds/config
        ${CMAKE_CURRENT_SOURCE_DIR}/dds/cyclone
        ${CMAKE_SOURCE_DIR}/3rdparty/cjson
)
target_link_libraries(nb_dds PUBLIC ntk nb_base rtps PRIVATE nb_json nb_log)
if(NB_ENABLE_DDS)
    target_link_libraries(nb_dds PUBLIC nb_cyclonedds)
endif()
set_target_properties(nb_dds PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    OUTPUT_NAME nb_dds)
install(TARGETS nb_dds
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Umbrella archive: keeps `nb` linkable while the real code lives in the two
# spine libraries.
add_library(nb STATIC base/version.c)
add_library(nb::nb ALIAS nb)
target_include_directories(nb
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(nb PUBLIC nb_rpc nb_dds)
set_target_properties(nb PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    OUTPUT_NAME nb)
install(TARGETS nb
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    # Internal facilities (poolset/slotpool/iobuf/poller_alloc) are compile-time
    # details of the library, not part of the installed SDK.
    PATTERN "internal" EXCLUDE)

# elog 只是 log.c 的实现细节：静态库最终仍需链接，但不向使用者传播头文件路径�?
target_link_libraries(nb PUBLIC ntk nb_base microcdr rtps PRIVATE nb_log ntk_discovery)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # shm_open()/sem_timedwait() live in librt on older glibc.
    target_link_libraries(nb PUBLIC rt)
endif()

if(NB_ENABLE_DDS)
    target_link_libraries(nb PUBLIC nb_cyclonedds)
endif()

set_target_properties(nb PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    OUTPUT_NAME nb
)

if(MSVC)
    target_compile_definitions(nb PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
endif()
