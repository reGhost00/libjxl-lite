# Copyright (c) the JPEG XL Project Authors. All rights reserved.
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file.

# 精简版 libjxl 构建配置
# 仅构建编码/解码器，仅支持 x86 msys2 / gcc / clang
# 使用内置 skcms，不需要安装和测试

cmake_minimum_required(VERSION 3.10)

option(JXLL_BUILD_WITH_TOOLS "build tools" ON)

# 强制使用内置库
set(JPEGXL_ENABLE_SKCMS ON CACHE BOOL "" FORCE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

project(LIBJXL LANGUAGES C CXX)

message(STATUS "CMake version: ${CMAKE_VERSION}")
message(STATUS "Compiler: C:${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}, C++:${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "System: ${CMAKE_SYSTEM_NAME}, Processor: ${CMAKE_SYSTEM_PROCESSOR}")

# 静态库
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_SKIP_INSTALL_RULES ON)

# 检查编译器是否支持的目标
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-mavx2" CXX_MAVX2_SUPPORTED)
check_cxx_compiler_flag("-mf16c" CXX_MF16C_SUPPORTED)
check_cxx_compiler_flag("-mavx512f" CXX_MAVX512F_SUPPORTED)
check_cxx_compiler_flag("-mavx512dq" CXX_MAVX512DQ_SUPPORTED)
check_cxx_compiler_flag("-mavx512cd" CXX_MAVX512CD_SUPPORTED)
check_cxx_compiler_flag("-mavx512bw" CXX_MAVX512BW_SUPPORTED)
check_cxx_compiler_flag("-mavx512vl" CXX_MAVX512VL_SUPPORTED)

check_cxx_compiler_flag(-std=c++17 LIB_HAVE_CXX17_FLAG)
if (LIB_HAVE_CXX17_FLAG)
  set(CMAKE_CXX_STANDARD 17)
  check_cxx_compiler_flag(-std=c++26 LIB_HAVE_CXX26_FLAG)
  if (LIB_HAVE_CXX26_FLAG)
    set(CMAKE_CXX_STANDARD 26)
  endif()
  set(CMAKE_CXX_STANDARD_REQUIRED YES)
endif()

# 线程支持
set(THREADS_PREFER_PTHREAD_FLAG YES)
find_package(Threads REQUIRED)

# Atomics 支持
find_package(Atomics REQUIRED)

# CMAKE_EXPORT_COMPILE_COMMANDS
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 编译标志

add_definitions(
  -D__DATE__="redacted"
  -D__TIMESTAMP__="redacted"
  -D__TIME__="redacted"
)
add_compile_options(
  -funwind-tables
  -fno-omit-frame-pointer
  -Wall
  -Wno-builtin-macro-redefined
)

# 第三方库
add_subdirectory(third_party)

# Highway include directories for lib targets.
set(JXL_HWY_INCLUDE_DIRS "$<BUILD_INTERFACE:$<TARGET_PROPERTY:$<IF:$<TARGET_EXISTS:hwy::hwy>,hwy::hwy,hwy>,INTERFACE_INCLUDE_DIRECTORIES>>")

# lib 目录
add_subdirectory(lib)

if (JXLL_BUILD_WITH_TOOLS)
  add_subdirectory(tools)
endif()
