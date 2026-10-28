# Copyright (c) the JPEG XL Project Authors. All rights reserved.
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file.
#
# Builds the in-tree JPEG codec (lib/jpegli/) as the static library
# `jpegli-static`.
#
# This is a fork-owned replacement for the upstream file `lib/jpegli.cmake`:
#   * the gtest-based test targets are dropped (this fork has no googletest),
#   * the libjpeg.so ABI-compatibility shared library is not built (it is not
#     needed by the wrap/ wrapper and is not supported on Windows),
#   * the C++ standard used for the jpegli sources is pinned, independently of
#     the standard used by the rest of the project.
#
# Include this from lib/CMakeLists.txt so that the relative source paths in
# JPEGXL_INTERNAL_JPEGLI_SOURCES resolve against lib/.

include(${PROJECT_SOURCE_DIR}/lib/jxl_lists.cmake)

set(JPEGLI_INTERNAL_LIBS
  hwy
  Threads::Threads
  ${ATOMICS_LIBRARIES}
)

# JPEGLIB setup
set(BITS_IN_JSAMPLE 8)
set(MEM_SRCDST_SUPPORTED 1)

# JPEG_LIB_VERSION selects which parts of jpeglib.h are visible to the jpegli
# sources; 62 is the libjpeg62 ABI that jpegli implements.
set(JPEG_LIB_VERSION 62)
# Substituted into third_party/libjpeg-turbo/jconfig.h.in.
set(VERSION "2.1.5.1")
set(LIBJPEG_TURBO_VERSION_NUMBER 2105005)

configure_file(
  ${PROJECT_SOURCE_DIR}/third_party/libjpeg-turbo/jconfig.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/jpegli/jconfig.h)
configure_file(
  ${PROJECT_SOURCE_DIR}/third_party/libjpeg-turbo/jpeglib.h
  ${CMAKE_CURRENT_BINARY_DIR}/include/jpegli/jpeglib.h COPYONLY)
configure_file(
  ${PROJECT_SOURCE_DIR}/third_party/libjpeg-turbo/jmorecfg.h
  ${CMAKE_CURRENT_BINARY_DIR}/include/jpegli/jmorecfg.h COPYONLY)

add_library(jpegli-static STATIC ${JPEGXL_INTERNAL_JPEGLI_SOURCES})
target_compile_options(jpegli-static PRIVATE "${JPEGXL_INTERNAL_FLAGS}")
set_property(TARGET jpegli-static PROPERTY POSITION_INDEPENDENT_CODE ON)
target_include_directories(jpegli-static PRIVATE
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>"
  "${JXL_HWY_INCLUDE_DIRS}"
)
target_include_directories(jpegli-static PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include/jpegli>"
)
target_link_libraries(jpegli-static PUBLIC ${JPEGLI_INTERNAL_LIBS})

# lib/jpegli/ predates C++20; keep it on C++17 independently of the standard
# used by the rest of the project.
set_target_properties(jpegli-static PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
)
