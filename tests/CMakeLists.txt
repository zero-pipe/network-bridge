# Test taxonomy (also used by tools/gate.py):
#   unit = pure module / codec / config logic, no sockets
#   e2e  = end-to-end loopback / streaming with real threads and sockets
#
# Run one category:
#   ctest --test-dir build -L unit
#   ctest --test-dir build -L e2e
# Or use the unified gate:
#   python3 tools/gate.py unit
#   python3 tools/gate.py e2e

set(NB_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# nb_add_test(<target> <ctest-name>
#             [SOURCES <file>...] [LIBRARIES <lib>...] [INCLUDES <dir>...]
#             [LABELS <label>...] [TIMEOUT <seconds>])
#
# The source defaults to <target>.c; libraries default to nb.
function(nb_add_test target test_name)
    cmake_parse_arguments(ARG "" "TIMEOUT" "SOURCES;LIBRARIES;INCLUDES;LABELS" ${ARGN})

    if(NOT ARG_SOURCES)
        set(ARG_SOURCES ${target}.c)
    endif()
    if(NOT ARG_LIBRARIES)
        set(ARG_LIBRARIES nb)
    endif()
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 120)
    endif()

    add_executable(${target} ${ARG_SOURCES})
    target_include_directories(${target} PRIVATE ${NB_SRC_DIR}/base ${ARG_INCLUDES})
    target_link_libraries(${target} PRIVATE ${ARG_LIBRARIES})
    nb_copy_runtime_deps(${target})

    add_test(NAME ${test_name} COMMAND ${target})
    set_tests_properties(${test_name} PROPERTIES
        LABELS "${ARG_LABELS}"
        TIMEOUT ${ARG_TIMEOUT})
endfunction()

# ---- unit ---------------------------------------------------------------
nb_add_test(test_framing unit.framing
    LABELS "unit;framing"
    INCLUDES ${NB_SRC_DIR}/transport/common ${NB_SRC_DIR}/message)

nb_add_test(test_config unit.config LABELS "unit;config")
nb_add_test(test_metrics unit.metrics
    LABELS "unit;metrics"
    INCLUDES ${NB_SRC_DIR}/utils)
nb_add_test(test_registry unit.registry
    LABELS "unit;registry"
    INCLUDES ${NB_SRC_DIR}/registry)
nb_add_test(test_reliable unit.reliable
    LABELS "unit;reliable"
    INCLUDES ${NB_SRC_DIR}/transport/common)
nb_add_test(test_poller_alloc unit.poller_alloc
    LABELS "unit;poller_alloc"
    INCLUDES
        ${NB_SRC_DIR}/core
        ${NB_SRC_DIR}/utils
        ${NB_SRC_DIR}/discovery
        ${NB_SRC_DIR}/message
        ${NB_SRC_DIR}/registry
        ${NB_SRC_DIR}/router
        ${NB_SRC_DIR}/transport/common
        ${CMAKE_SOURCE_DIR}/3rdparty/uthash)
nb_add_test(test_iobuf unit.iobuf
    LABELS "unit;iobuf"
    INCLUDES ${NB_SRC_DIR}/core ${NB_SRC_DIR}/utils)
nb_add_test(test_rtps_wire unit.rtps_wire
    LABELS "unit;rtps"
    INCLUDES ${NB_SRC_DIR}/rtps/internal)
nb_add_test(test_rtps_cdr unit.rtps_cdr
    LABELS "unit;rtps"
    INCLUDES ${NB_SRC_DIR}/rtps/internal)
nb_add_test(test_rtps_sedp unit.rtps_sedp
    LABELS "unit;rtps"
    INCLUDES ${NB_SRC_DIR}/rtps/internal)
nb_add_test(test_rtps_reliable unit.rtps_reliable
    LABELS "unit;rtps"
    INCLUDES ${NB_SRC_DIR}/rtps/internal)
nb_add_test(test_rtps_participant e2e.rtps_participant
    LABELS "e2e;rtps"
    INCLUDES ${NB_SRC_DIR}/rtps/internal
    TIMEOUT 60)

nb_add_test(test_ntk_discovery unit.ntk_discovery
    LABELS "unit;discovery"
    LIBRARIES ntk_discovery)

# Contract: the rtps transport provider may only use the public rtps_* API.
add_test(NAME contract.rtps_provider_api
    COMMAND ${CMAKE_COMMAND}
            -DFILE=${CMAKE_SOURCE_DIR}/src/transport/rtps/rtps_transport.c
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_rtps_provider.cmake)
set_tests_properties(contract.rtps_provider_api PROPERTIES
    LABELS "unit;contract;rtps"
    TIMEOUT 30)

# The public headers must be nb-free and only declare rtps_* symbols.
add_test(NAME contract.rtps_headers
    COMMAND ${CMAKE_COMMAND}
            -DHEADER_DIR=${CMAKE_SOURCE_DIR}/include/rtps
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_rtps_headers.cmake)
set_tests_properties(contract.rtps_headers PROPERTIES
    LABELS "unit;contract;rtps"
    TIMEOUT 30)

# The stub is an internal call engine; public nb headers must not expose it.
add_test(NAME contract.nb_headers
    COMMAND ${CMAKE_COMMAND}
            -DHEADER_DIR=${CMAKE_SOURCE_DIR}/include/nb
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_nb_headers.cmake)
set_tests_properties(contract.nb_headers PROPERTIES
    LABELS "unit;contract"
    TIMEOUT 30)

# The public library must only export rtps_* symbols. Skipped when no nm.
add_test(NAME contract.rtps_symbols
    COMMAND ${CMAKE_COMMAND}
            -DNM=${CMAKE_NM}
            -DARCHIVE=$<TARGET_FILE:rtps>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_rtps_symbols.cmake)
set_tests_properties(contract.rtps_symbols PROPERTIES
    LABELS "unit;contract;rtps"
    TIMEOUT 30)

nb_add_test(test_rtps_api e2e.rtps_api
    LABELS "e2e;rtps;api"
    TIMEOUT 60)

nb_add_test(test_rtps_transport e2e.rtps_transport
    LABELS "e2e;rtps"
    INCLUDES ${NB_SRC_DIR}/rtps/internal
    TIMEOUT 60)

nb_add_test(test_buffer unit.buffer LABELS "unit;buffer" LIBRARIES nb_base)
nb_add_test(test_buf_concurrency unit.buf_concurrency
    LABELS "unit;concurrency"
    LIBRARIES ntk nb_base
    TIMEOUT 300)
nb_add_test(test_slotpool unit.slotpool LABELS "unit;slotpool" LIBRARIES nb_base)
nb_add_test(test_poolset unit.poolset LABELS "unit;poolset" TIMEOUT 60)
nb_add_test(test_headers unit.headers LABELS "unit;headers" TIMEOUT 30)
nb_add_test(test_contracts unit.contracts LABELS "unit;contracts")

# ---- end-to-end ---------------------------------------------------------
nb_add_test(test_loopback e2e.loopback LABELS "e2e;loopback" TIMEOUT 300)
nb_add_test(test_stream e2e.stream LABELS "e2e;stream" TIMEOUT 300)
nb_add_test(test_api e2e.api LABELS "e2e;api")
nb_add_test(test_sample_queue e2e.sample_queue
    LABELS "e2e;dds"
    INCLUDES ${NB_SRC_DIR}/core
    TIMEOUT 120)
nb_add_test(test_move e2e.move LABELS "e2e;move")
nb_add_test(test_worker e2e.worker LABELS "e2e;worker")
nb_add_test(test_semantics e2e.semantics LABELS "e2e;semantics")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    nb_add_test(test_shm e2e.shm LABELS "e2e;shm")
    nb_add_test(test_shmseg unit.shmseg
        LABELS "unit;shm"
        INCLUDES ${NB_SRC_DIR}/transport/shm)
    nb_add_test(test_shm_mesh e2e.shm_mesh LABELS "e2e;shm")
endif()

if(NB_ENABLE_DDS)
    nb_add_test(test_dds_loan e2e.dds_loan LABELS "e2e;dds")
    nb_add_test(test_dds_local e2e.dds_local LABELS "e2e;dds")
    nb_add_test(test_dds_remote e2e.dds_remote
        LABELS "e2e;dds"
        INCLUDES ${NB_SRC_DIR}/transport/dds
        TIMEOUT 30)
    nb_add_test(test_rtps_cyclone e2e.rtps_cyclone
        LABELS "e2e;dds;rtps"
        INCLUDES ${NB_SRC_DIR}/rtps/internal ${NB_SRC_DIR}/transport/dds
        TIMEOUT 180)
endif()
