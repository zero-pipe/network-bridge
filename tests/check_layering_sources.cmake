# Contract check for the source tree layering.
#   cmake -DSRC_DIR=<repo>/src -P check_layering_sources.cmake
#
# The spines are independent at the source level too, not only in public
# headers:
#   - src/base   must not reach into RPC or DDS (shared foundation only)
#   - src/rpc    must not reach into DDS or the RTPS engine
#   - src/dds    must not reach into the RPC spine
#
# Shared foundation code (buf/error/log/metrics/json) belongs in src/base; a
# file that needs something from the other spine is a layering bug.
if(NOT DEFINED SRC_DIR)
    message(FATAL_ERROR "SRC_DIR is not set")
endif()

if(NOT EXISTS "${SRC_DIR}")
    message(FATAL_ERROR "source directory not found: ${SRC_DIR}")
endif()

# Matches both angle-bracket public includes and quoted internal includes.
function(nb_scan_tree tree)
    file(GLOB_RECURSE files
        "${SRC_DIR}/${tree}/*.c"
        "${SRC_DIR}/${tree}/*.h")
    if(NOT files)
        return()
    endif()
    foreach(file IN LISTS files)
        file(READ "${file}" content)
        string(REPLACE "\\" "/" rel "${file}")
        foreach(needle IN LISTS ARGN)
            string(FIND "${content}" "${needle}" pos)
            if(NOT pos EQUAL -1)
                message(FATAL_ERROR
                    "${rel} references '${needle}'; this crosses a layer boundary")
            endif()
        endforeach()
    endforeach()
    message(STATUS "${tree}: no cross-layer references")
endfunction()

# base: shared foundation, no spine knowledge at all.
nb_scan_tree(base
    "#include <nb/rpc/"
    "#include <nb/dds/"
    "#include <nb/config.h>"
    "#include \"rtps_"
    "#include \"envelope.h\"")

# rpc: no DDS spine, no RTPS engine.
nb_scan_tree(rpc
    "#include <nb/dds/"
    "#include <rtps/"
    "nb_rtps_"
    "rtps_")

# dds: no RPC spine. The RTPS engine is allowed (it is a DDS backend).
nb_scan_tree(dds
    "#include <nb/rpc/"
    "#include <nb/config.h>"
    "nb_node_on_message")

message(STATUS "source tree layering is intact")
