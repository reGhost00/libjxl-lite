# Copyright (c) the JPEG XL Project Authors. All rights reserved.
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file.

include(jxl_lists.cmake)

# Headers for exporting/importing public headers
include(GenerateExportHeader)

add_library(jxl_cms
  ${JPEGXL_INTERNAL_CMS_SOURCES}
)
target_compile_options(jxl_cms PRIVATE "${JPEGXL_INTERNAL_FLAGS}")
set_target_properties(jxl_cms PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN 1)
target_link_libraries(jxl_cms PUBLIC jxl_base)
target_include_directories(jxl_cms PRIVATE
  ${JXL_HWY_INCLUDE_DIRS}
)
generate_export_header(jxl_cms
  BASE_NAME JXL_CMS
  EXPORT_FILE_NAME include/jxl/jxl_cms_export.h)
target_compile_definitions(jxl_cms PUBLIC
  "$<$<NOT:$<BOOL:${BUILD_SHARED_LIBS}>>:JXL_CMS_STATIC_DEFINE>")
target_include_directories(jxl_cms BEFORE PUBLIC
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>")

set(JPEGXL_CMS_LIBRARY_REQUIRES "")

target_link_skcms(jxl_cms)
target_link_libraries(jxl_cms PRIVATE hwy)

set_target_properties(jxl_cms PROPERTIES
        VERSION ${JPEGXL_LIBRARY_VERSION}
        SOVERSION ${JPEGXL_LIBRARY_SOVERSION})

# Check whether the linker support excluding libs
set(LINKER_EXCLUDE_LIBS_FLAG "-Wl,--exclude-libs=ALL")
include(CheckCSourceCompiles)
list(APPEND CMAKE_REQUIRED_LINK_OPTIONS ${LINKER_EXCLUDE_LIBS_FLAG})
check_c_source_compiles("int main(){return 0;}" LINKER_SUPPORT_EXCLUDE_LIBS)
list(REMOVE_ITEM CMAKE_REQUIRED_LINK_OPTIONS ${LINKER_EXCLUDE_LIBS_FLAG})

if(LINKER_SUPPORT_EXCLUDE_LIBS)
  set_property(TARGET jxl_cms APPEND_STRING PROPERTY
      LINK_FLAGS " ${LINKER_EXCLUDE_LIBS_FLAG}")
endif()

if (BUILD_SHARED_LIBS)
  set(JPEGXL_REQUIRES_TYPE "Requires.private")
else()
  set(JPEGXL_REQUIRES_TYPE "Requires")
endif()
set(JPEGXL_CMS_PRIVATE_LIBS "-lm ${PKGCONFIG_CXX_LIB}")

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/jxl/libjxl_cms.pc.in" "libjxl_cms.pc" @ONLY)
