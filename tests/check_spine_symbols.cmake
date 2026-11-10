# Contract check: a spine archive must not export symbols owned by the other
# spine or by a backend it must not link.
#   cmake -DNM=<nm> -DARCHIVE=<lib> -DFORBIDDEN="rtps_;dds_" -P check_spine_symbols.cmake
#
# When no NM is available (for example some Windows toolchains) the check is
# skipped rather than failed.
if(NOT DEFINED NM OR NM STREQUAL "")
    message(STATUS "check_spine_symbols: NM not set, skipping")
    return()
endif()
if(NOT DEFINED ARCHIVE OR NOT EXISTS "${ARCHIVE}")
    message(FATAL_ERROR "check_spine_symbols: archive not found: ${ARCHIVE}")
endif()

execute_process(
    COMMAND "${NM}" -g --defined-only "${ARCHIVE}"
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "nm failed on ${ARCHIVE}: ${error}")
endif()

string(REPLACE "\r\n" "\n" output "${output}")
string(REPLACE "\n" ";" lines "${output}")
foreach(line IN LISTS lines)
    string(STRIP "${line}" line)
    if(line STREQUAL "")
        continue()
    endif()
    string(REGEX REPLACE "^[0-9A-Fa-f]+ +[A-Za-z] +(.+)$" "\\1" name "${line}")
    if(name STREQUAL line)
        continue()
    endif()
    foreach(prefix IN LISTS FORBIDDEN)
        string(LENGTH "${prefix}" plen)
        string(SUBSTRING "${name}" 0 ${plen} head)
        if(head STREQUAL prefix)
            message(FATAL_ERROR
                "${ARCHIVE} exports ${name}, which belongs to another spine (${prefix}*)")
        endif()
    endforeach()
endforeach()

message(STATUS "${ARCHIVE}: no forbidden spine symbols")
