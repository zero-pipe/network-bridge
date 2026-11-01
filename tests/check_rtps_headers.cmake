# Contract check: the public rtps_* headers must be self-contained and must not
# mention the framework's nb_* layer.
#   cmake -DHEADER_DIR=<include/rtps> -P check_rtps_headers.cmake
if(NOT DEFINED HEADER_DIR)
    message(FATAL_ERROR "HEADER_DIR is not set")
endif()

file(GLOB headers "${HEADER_DIR}/*.h")
if(NOT headers)
    message(FATAL_ERROR "no public rtps headers found in ${HEADER_DIR}")
endif()

foreach(header IN LISTS headers)
    file(READ "${header}" content)

    string(FIND "${content}" "nb_" pos)
    if(NOT pos EQUAL -1)
        message(FATAL_ERROR
            "${header} mentions a framework nb_* symbol; the public RTPS API must be independent")
    endif()

    # Every declaration carrying RTPS_API must belong to the rtps_ namespace.
    # Checked line by line: CMake regexes do not treat \n as a newline class.
    file(STRINGS "${header}" lines)
    foreach(line IN LISTS lines)
        string(STRIP "${line}" line)
        string(FIND "${line}" "RTPS_API" pos)
        if(pos EQUAL 0)
            string(FIND "${line}" "rtps_" name_pos)
            if(name_pos EQUAL -1)
                message(FATAL_ERROR
                    "${header}: RTPS_API declaration is not named rtps_*: ${line}")
            endif()
        endif()
    endforeach()
endforeach()

message(STATUS "rtps public headers are nb-free and rtps_-namespaced")
