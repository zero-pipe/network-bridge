# Micro-CDR 2.0.2 - https://github.com/eProsima/Micro-CDR
# Copyright 2017 Proyectos y Sistemas de Mantenimiento SL (eProsima).
# Licensed under the Apache License, Version 2.0 (see LICENSE).
#
# Vendored subset: only the library sources, the public headers and the build
# rules needed to compile them as part of this project. The upstream tests,
# examples, CI, packaging and documentation have been removed.

cmake_minimum_required(VERSION 3.15)

project(microcdr VERSION 2.0.2 LANGUAGES C)

option(UCDR_BIG_ENDIANNESS "Build for a big-endian machine." OFF)

if(UCDR_BIG_ENDIANNESS)
    set(CONFIG_MACHINE_ENDIANNESS UCDR_BIG_ENDIANNESS)
else()
    set(CONFIG_MACHINE_ENDIANNESS UCDR_LITTLE_ENDIANNESS)
endif()

add_library(microcdr STATIC
    src/c/common.c
    src/c/types/basic.c
    src/c/types/string.c
    src/c/types/array.c
    src/c/types/sequence.c
    )

set_target_properties(microcdr PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    POSITION_INDEPENDENT_CODE ON
    )

# config.h only carries the version and the machine endianness; the upstream
# template is kept so the third-party sources still see the same header.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ucdr/config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/ucdr/config.h
    )

target_include_directories(microcdr
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    )
