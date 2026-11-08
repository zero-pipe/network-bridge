# Contract check: the stub is an internal call engine under proxy; it must not
# reappear as a public header or be referenced by any public <nb/...> header.
#   cmake -DHEADER_DIR=<include/nb> -P check_nb_headers.cmake
if(NOT DEFINED HEADER_DIR)
    message(FATAL_ERROR "HEADER_DIR is not set")
endif()

if(EXISTS "${HEADER_DIR}/stub.h")
    message(FATAL_ERROR
        "${HEADER_DIR}/stub.h is public; stub is internal (src/rpc/stub_internal.h)")
endif()

file(GLOB headers "${HEADER_DIR}/*.h")
if(NOT headers)
    message(FATAL_ERROR "no public nb headers found in ${HEADER_DIR}")
endif()

foreach(header IN LISTS headers)
    file(READ "${header}" content)

    string(FIND "${content}" "nb_stub" pos)
    if(NOT pos EQUAL -1)
        message(FATAL_ERROR
            "${header} references the internal nb_stub layer; use nb_proxy_* instead")
    endif()

    # Internal facilities (poolset/slotpool/iobuf/poller_alloc/...) live under
    # include/nb/internal and must not leak through a public header.
    string(FIND "${content}" "#include <nb/internal/" internal_pos)
    if(NOT internal_pos EQUAL -1)
        message(FATAL_ERROR
            "${header} includes an internal nb header; public headers must not depend on include/nb/internal")
    endif()
endforeach()

# ---- Spines -------------------------------------------------------------
# base.h stays model-neutral, and nb.h re-exports both spines. This is what
# keeps the umbrella header from growing into a union of RPC and pub/sub APIs;
# see docs/architecture-split.md.
foreach(spine rpc pubsub)
    if(NOT EXISTS "${HEADER_DIR}/${spine}.h")
        message(FATAL_ERROR "missing spine header ${HEADER_DIR}/${spine}.h")
    endif()
endforeach()

file(READ "${HEADER_DIR}/base.h" base_content)
foreach(forbidden
        node service proxy config
        qos sample waitset status discovery_view
        publish subscribe dds rtps shm transport_provider)
    string(FIND "${base_content}" "#include <nb/${forbidden}.h>" forbidden_pos)
    if(NOT forbidden_pos EQUAL -1)
        message(FATAL_ERROR
            "nb/base.h includes nb/${forbidden}.h; the base header must stay model-neutral")
    endif()
endforeach()

file(READ "${HEADER_DIR}/nb.h" umbrella_content)
foreach(spine rpc pubsub)
    string(FIND "${umbrella_content}" "#include <nb/${spine}.h>" umbrella_pos)
    if(umbrella_pos EQUAL -1)
        message(FATAL_ERROR
            "nb/nb.h does not include nb/${spine}.h; the umbrella header must re-export both spines")
    endif()
endforeach()

# The RPC spine must not drag in pub/sub-only types: reader history is a
# pub/sub concept (nb_reader_t), not part of the service API.
file(READ "${HEADER_DIR}/service.h" service_content)
string(FIND "${service_content}" "#include <nb/sample.h>" service_sample)
if(NOT service_sample EQUAL -1)
    message(FATAL_ERROR
        "nb/service.h includes nb/sample.h; reader history belongs to nb/pubsub.h")
endif()

file(READ "${HEADER_DIR}/rpc.h" rpc_content)
foreach(forbidden_header sample waitset status discovery_view
        service_history publish subscribe dds rtps shm)
    string(FIND "${rpc_content}" "#include <nb/${forbidden_header}.h>" rpc_pos)
    if(NOT rpc_pos EQUAL -1)
        message(FATAL_ERROR
            "nb/rpc.h includes nb/${forbidden_header}.h; that header belongs to the pub/sub spine")
    endif()
endforeach()

message(STATUS "public nb headers do not expose the internal stub layer")
message(STATUS "nb/base.h stays model-neutral and nb/nb.h re-exports both spines")
message(STATUS "nb/rpc.h does not pull pub/sub-only headers")
