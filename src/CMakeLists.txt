set(NB_BASE_SOURCES
    base/alloc.c
    base/buf.c
    base/poolset.c
    base/slotpool.c
    base/iobuf.c
    base/poller_alloc.c
    base/crc32.c
)

add_library(nb_base STATIC ${NB_BASE_SOURCES})
add_library(nb::base ALIAS nb_base)

target_include_directories(nb_base
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
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

set(NB_SOURCES
    # shared runtime: node lifecycle, inbound dispatch, binding assembly
    runtime/node.c
    runtime/node_access.c
    runtime/transport_factory.c
    runtime/dispatch.c
    runtime/codec.c
    runtime/metadata.c
    runtime/message_view.c
    runtime/payload.c
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
    # pub/sub spine
    pubsub/pubsub.c
    pubsub/sample_queue.c
    pubsub/waitset.c
    pubsub/discovery_view.c
    utils/error.c
    utils/metrics.c
    utils/util.c
    utils/endpoint.c
    utils/qos.c
    utils/json/codec_json.c
    message/frame.c
    message/envelope.c
    router/router.c
    registry/registry.c
    discovery/discovery.c
    config/config.c
    transport/common/framing.c
    transport/common/dds_bridge.c
    transport/common/transport.c
    transport/common/provider.c
    transport/common/rx_window.c
    transport/common/reasm.c
    transport/common/tx_ring.c
    transport/udp/udp.c
    transport/tcp/tcp.c
    transport/rtps/rtps_transport.c
    transport/shm/shm.c
    transport/shm/shm_segment.c
    ${CMAKE_SOURCE_DIR}/3rdparty/cjson/cJSON.c
)

if(NB_ENABLE_DDS)
    list(APPEND NB_SOURCES
        transport/dds/cyclone_dds.c
        transport/dds/cy_dds_message.c)
endif()

# ---- RTPS engine and public API -----------------------------------------
# log.c is shared by the framework and the RTPS engine. Keeping it in a small
# library lets the engine build without pulling in the framework: rtps_core
# depends on nb_base, ntk, microcdr and nb_log only.
add_library(nb_log STATIC utils/log.c)
target_include_directories(nb_log
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/utils)
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

add_library(nb STATIC ${NB_SOURCES})
add_library(nb::nb ALIAS nb)
# Backward-compatible target name for downstreams that still link `rpc`.
add_library(rpc ALIAS nb)

target_include_directories(nb
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/base
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc
        ${CMAKE_CURRENT_SOURCE_DIR}/pubsub
        ${CMAKE_CURRENT_SOURCE_DIR}/utils
        ${CMAKE_CURRENT_SOURCE_DIR}/utils/json
        ${CMAKE_CURRENT_SOURCE_DIR}/message
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/common
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/dds
        ${CMAKE_CURRENT_SOURCE_DIR}/router
        ${CMAKE_CURRENT_SOURCE_DIR}/registry
        ${CMAKE_CURRENT_SOURCE_DIR}/discovery
        ${CMAKE_CURRENT_SOURCE_DIR}/rtps
        ${CMAKE_CURRENT_SOURCE_DIR}/config
        ${CMAKE_SOURCE_DIR}/3rdparty/uthash
        ${CMAKE_SOURCE_DIR}/3rdparty/cjson
)

install(TARGETS nb
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
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
