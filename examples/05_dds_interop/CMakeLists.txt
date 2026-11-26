# network-bridge side always builds (needs only libnb).
nb_add_example(example_05_nb_pub nb_pub.c)
nb_add_example(example_05_nb_sub nb_sub.c)

# Cyclone side builds only when a Cyclone DDS toolchain (library + idlc) is
# found, so this directory stays buildable on machines without it. The
# generated Trade.c/Trade.h are produced by idlc from Trade.idl at build time.
find_package(CycloneDDS QUIET)
find_program(CYCLONEDDS_IDLC idlc
    HINTS $ENV{CYCLONEDDS_HOME}/bin $ENV{CYCLONEDDS_HOME})

if(CycloneDDS_FOUND AND CYCLONEDDS_IDLC)
    set(_gen ${CMAKE_CURRENT_BINARY_DIR}/gen)
    file(MAKE_DIRECTORY ${_gen})
    add_custom_command(
        OUTPUT ${_gen}/Trade.c ${_gen}/Trade.h
        COMMAND ${CYCLONEDDS_IDLC} -l c -o ${_gen}/ ${CMAKE_CURRENT_SOURCE_DIR}/Trade.idl
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Trade.idl
        COMMENT "idlc: generating Trade types from Trade.idl")

    add_library(example_05_trade_types STATIC ${_gen}/Trade.c)
    target_include_directories(example_05_trade_types PUBLIC ${_gen})
    target_link_libraries(example_05_trade_types PUBLIC CycloneDDS::ddsc)

    add_executable(example_05_cyl_pub cyl_pub.c)
    add_executable(example_05_cyl_sub cyl_sub.c)
    foreach(_t example_05_cyl_pub example_05_cyl_sub)
        target_link_libraries(${_t} PRIVATE example_05_trade_types)
    endforeach()
    message(STATUS "05_dds_interop: Cyclone examples enabled (idlc: ${CYCLONEDDS_IDLC})")
else()
    message(STATUS "05_dds_interop: Cyclone DDS not found; building network-bridge side only")
endif()
