# Contract check: the RTPS transport provider must use only the public rtps_*
# interface. Run with:
#   cmake -DFILE=<path>/rtps_transport.c -P check_rtps_provider.cmake
if(NOT DEFINED FILE)
    message(FATAL_ERROR "FILE is not set")
endif()
if(NOT EXISTS "${FILE}")
    message(FATAL_ERROR "provider source not found: ${FILE}")
endif()

file(READ "${FILE}" content)

set(needles
    "#include \"rtps_"
    "nb_rtps_participant_"
    "nb_rtps_endpoint_info_t"
    "nb_rtps_guid_t"
    "nb_rtps_locator_t"
    "nb_rtps_status_event_t"
    "nb_rtps_dds_message_t"
)

foreach(needle IN LISTS needles)
    string(FIND "${content}" "${needle}" pos)
    if(NOT pos EQUAL -1)
        message(FATAL_ERROR
            "rtps provider uses a private engine symbol '${needle}'; use the public <rtps/rtps.h> API instead")
    endif()
endforeach()

string(FIND "${content}" "#include <rtps/rtps.h>" pos)
if(pos EQUAL -1)
    message(FATAL_ERROR "rtps provider does not include <rtps/rtps.h>")
endif()

message(STATUS "rtps provider uses only the public rtps_* API")
