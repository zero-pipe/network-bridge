# Contract check: the stub is an internal call engine under proxy; it must not
# reappear as a public header or be referenced by any public <nb/...> header.
#   cmake -DHEADER_DIR=<include/nb> -P check_nb_headers.cmake
if(NOT DEFINED HEADER_DIR)
    message(FATAL_ERROR "HEADER_DIR is not set")
endif()

if(EXISTS "${HEADER_DIR}/stub.h")
    message(FATAL_ERROR
        "${HEADER_DIR}/stub.h is public; stub is internal (src/core/stub_internal.h)")
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
endforeach()

message(STATUS "public nb headers do not expose the internal stub layer")
