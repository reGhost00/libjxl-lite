# Copyright (c) the JPEG XL Project Authors. All rights reserved.
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file.
#
# jxlimg: a small unified image codec for JPEG and JPEG XL, with an
# stb_image like API.

if (NOT TARGET jpegli-static)
  message(FATAL_ERROR
    "wrap/ needs the in-tree JPEG codec (lib/jpegli/): enable "
    "JPEGXL_ENABLE_JPEGLI or JXLL_BUILD_WRAP.")
endif()

add_library(jxlimg STATIC
  jxlimg.cc
  jxlimg_jpeg.cc
  jxlimg_jxl.cc
)
add_library(jxlimg::jxlimg ALIAS jxlimg)

target_include_directories(jxlimg
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>"
)

target_link_libraries(jxlimg
  PUBLIC jxl
  PRIVATE jpegli-static
)

# JPEGXL_INTERNAL_FLAGS is defined in the scope of lib/, which is not
# visible here, so the wrapper uses its own set of warnings.
if (NOT MSVC)
  target_compile_options(jxlimg PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-missing-field-initializers
  )
endif()

#
# Tests
#
add_executable(jxlimg_test tests/jxlimg_test.cc)
target_link_libraries(jxlimg_test PRIVATE jxlimg)
if (NOT MSVC)
  target_compile_options(jxlimg_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME jxlimg_test COMMAND jxlimg_test)
