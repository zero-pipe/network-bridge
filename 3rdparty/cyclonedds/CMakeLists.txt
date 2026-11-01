#
# Copyright(c) 2006 to 2022 ZettaScale Technology and others
#
# This program and the accompanying materials are made available under the
# terms of the Eclipse Public License v. 2.0 which is available at
# http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
# v. 1.0 which is available at
# http://www.eclipse.org/org/documents/edl-v10.php.
#
# SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
#
# 源码形式的 Cyclone DDS（11.0.1，上游 3a552631）。
# 上游顶层 CMakeLists.txt 的精简版：只保留编译 ddsc 所需的部分，
# 详见 README.md。
cmake_minimum_required(VERSION 3.16)
project(CycloneDDS VERSION 11.0.1 LANGUAGES C)

# 普通变量（非 cache），子目录的 option() 会沿用（CMP0077），
# 也不会污染父工程的 cache。需要别的配置就在 add_subdirectory() 前覆盖。
set(BUILD_SHARED_LIBS OFF)      # 静态库，直接链进 network_bridge
set(BUILD_TESTING OFF)
set(BUILD_IDLC OFF)
set(BUILD_IDLC_XTESTS OFF)
set(BUILD_DDSPERF OFF)
set(BUILD_EXAMPLES OFF)
set(ENABLE_FAKEUDP OFF)
set(ENABLE_SECURITY OFF)        # 默认不引入 OpenSSL
set(ENABLE_SSL OFF)
set(ENABLE_TCP_TLS OFF)
set(ENABLE_ICEORYX OFF)         # psmx_iox 源码保留，默认不编译
set(ENABLE_ICEORYX2 OFF)
set(ENABLE_LTO OFF)
set(INSTALL_XMLTYPE OFF)
set(INSTALL_PDB OFF)
set(APPEND_PROJECT_NAME_TO_INCLUDEDIR OFF)

option(WERROR "Treat compiler warnings as errors" OFF)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake/Modules")

# Provides add_coverage(), used by src/core/CMakeLists.txt.
find_package(codecov QUIET)

# We require C99, test code can benefit from C23 (upstream logic).
if(CMAKE_VERSION VERSION_LESS "3.21" OR MSVC)
  set(CMAKE_C_STANDARD 99)
else()
  set(CMAKE_C_STANDARD 23)
  set(CMAKE_C_STANDARD_REQUIRED FALSE)
endif()

if(MSVC_VERSION LESS 1920)
  # "restrict" is supported since Visual Studio 16 2019.
  add_definitions("-Drestrict=")
endif()

# Reasonably strict warnings for clang, gcc and msvc (upstream defaults).
if("${CMAKE_C_COMPILER_ID}" STREQUAL "Clang" OR
   "${CMAKE_C_COMPILER_ID}" STREQUAL "AppleClang")
  add_compile_options(-Wall -Wextra -Wconversion -Wunused
                      -Winfinite-recursion -Wassign-enum -Wcomma
                      -Wdocumentation -Wstrict-prototypes
                      -Wconditional-uninitialized -Wshadow)
  add_compile_options($<$<COMPILE_LANGUAGE:C>:-Wmissing-prototypes>)
  if(WERROR)
    add_compile_options(-Werror)
    add_link_options(-Werror)
  endif()
elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
  add_compile_options(-Wall -Wextra -Wconversion)
  add_compile_options($<$<COMPILE_LANGUAGE:C>:-Wmissing-prototypes>)
  if(WERROR)
    add_compile_options(-Werror)
    add_link_options(-Werror)
  endif()
elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
  add_compile_options(/W3)
  if(WERROR)
    add_compile_options(/WX)
    add_link_options(/WX)
  endif()
endif()

# 产物放在本目录的构建树里，不和 network_bridge 的输出混在一起。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")

include(GNUInstallDirs)

# 生成文件的路径。源码里已带生成结果；docs/ 没有搬过来，
# 重新生成时只写构建目录，且这些目标不参与默认构建。
set(generated_cyclonedds_rnc "${CMAKE_CURRENT_SOURCE_DIR}/etc/cyclonedds.rnc")
set(generated_cyclonedds_xsd "${CMAKE_CURRENT_SOURCE_DIR}/etc/cyclonedds.xsd")
set(generated_options_md     "${CMAKE_CURRENT_BINARY_DIR}/generated/options.md")
set(generated_options_rst    "${CMAKE_CURRENT_BINARY_DIR}/generated/config_file_reference.rst")
set(generated_defconfig_src  "${CMAKE_CURRENT_SOURCE_DIR}/src/core/ddsi/defconfig.c")

add_subdirectory(compat)
add_subdirectory(src)

add_custom_target(check_generated_sources)
if(TARGET _confgen_check)
  add_dependencies(check_generated_sources _confgen_check)
endif()
if(TARGET ddsi_xtypes_generated_check)
  add_dependencies(check_generated_sources ddsi_xtypes_generated_check)
endif()
