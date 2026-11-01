# Contract check: the public rtps library must only export rtps_* symbols.
#   cmake -DNM=<nm> -DARCHIVE=<lib rtps> -P check_rtps_symbols.cmake
#
# When no NM is available (for example some Windows toolchains) the check is
# skipped rather than failed.
if(NOT DEFINED NM OR NM STREQUAL "")
    message(STATUS "check_rtps_symbols: NM not set, skipping")
    return()
endif()
if(NOT DEFINED ARCHIVE OR NOT EXISTS "${ARCHIVE}")
    message(FATAL_ERROR "check_rtps_symbols: archive not found: ${ARCHIVE}")
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
    # Format: "<value> <type> <name>"
    string(REGEX REPLACE "^[0-9A-Fa-f]+ +[A-Za-z] +(.+)$" "\\1" name "${line}")
    if(name STREQUAL line)
        continue() # header line or unusual format
    endif()
    string(SUBSTRING "${name}" 0 2 prefix)
    if(prefix STREQUAL "__")
        continue()
    endif()
    string(SUBSTRING "${name}" 0 5 prefix)
    if(NOT prefix STREQUAL "rtps_")
        message(FATAL_ERROR
            "rtps library exports a non-rtps_ symbol: ${name}")
    endif()
endforeach()

message(STATUS "rtps library exports only rtps_* symbols")
