# Contract check for the public headers.
#   cmake -DHEADER_DIR=<include/nb> -P check_nb_headers.cmake
#
# Layout: include/nb/base/*, include/nb/rpc/*, include/nb/dds/* and the three
# umbrella headers base.h / rpc.h / dds.h.
if(NOT DEFINED HEADER_DIR)
    message(FATAL_ERROR "HEADER_DIR is not set")
endif()

if(EXISTS "${HEADER_DIR}/stub.h")
    message(FATAL_ERROR
        "${HEADER_DIR}/stub.h is public; stub is internal (src/rpc/stub_internal.h)")
endif()

file(GLOB_RECURSE headers "${HEADER_DIR}/*.h")
if(NOT headers)
    message(FATAL_ERROR "no public nb headers found in ${HEADER_DIR}")
endif()

function(nb_forbid_includes header forbidden)
    file(READ "${header}" content)
    string(FIND "${content}" "${forbidden}" pos)
    if(NOT pos EQUAL -1)
        message(FATAL_ERROR "${header} includes ${forbidden}; the spines must stay independent")
    endif()
endfunction()

foreach(header IN LISTS headers)
    file(READ "${header}" content)

    string(FIND "${content}" "nb_stub" pos)
    if(NOT pos EQUAL -1)
        message(FATAL_ERROR
            "${header} references the internal nb_stub layer; use nb_proxy_* instead")
    endif()

    string(FIND "${content}" "#include <nb/internal/" internal_pos)
    if(NOT internal_pos EQUAL -1)
        message(FATAL_ERROR
            "${header} includes an internal nb header; public headers must not depend on include/nb/internal")
    endif()

    # Spine independence at the header level.
    get_filename_component(dir "${header}" DIRECTORY)
    get_filename_component(dirname "${dir}" NAME)
    if(dirname STREQUAL "rpc")
        nb_forbid_includes("${header}" "#include <nb/dds/")
    elseif(dirname STREQUAL "dds")
        nb_forbid_includes("${header}" "#include <nb/rpc/")
    elseif(dirname STREQUAL "base")
        nb_forbid_includes("${header}" "#include <nb/rpc/")
        nb_forbid_includes("${header}" "#include <nb/dds/")
    endif()
endforeach()

# ---- Umbrellas ----------------------------------------------------------
foreach(spine base rpc dds)
    if(NOT EXISTS "${HEADER_DIR}/${spine}.h")
        message(FATAL_ERROR "missing spine header ${HEADER_DIR}/${spine}.h")
    endif()
endforeach()

foreach(forbidden
        "#include <nb/rpc/" "#include <nb/dds/" "#include <nb/config.h>"
        "#include <nb/internal/")
    nb_forbid_includes("${HEADER_DIR}/base.h" "${forbidden}")
endforeach()

file(READ "${HEADER_DIR}/nb.h" umbrella_content)
foreach(spine base rpc dds)
    string(FIND "${umbrella_content}" "#include <nb/${spine}.h>" umbrella_pos)
    if(umbrella_pos EQUAL -1)
        message(FATAL_ERROR
            "nb/nb.h does not include nb/${spine}.h; the umbrella header must re-export all spines")
    endif()
endforeach()

message(STATUS "public nb headers do not expose the internal stub layer")
message(STATUS "base/rpc/dds headers do not cross-import each other")
message(STATUS "nb/nb.h re-exports all spines")
