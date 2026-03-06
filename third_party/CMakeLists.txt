# Highway
set(HWY_FORCE_STATIC_LIBS ON CACHE INTERNAL "" FORCE)
set(HWY_ENABLE_CONTRIB OFF CACHE INTERNAL "" FORCE)
set(HWY_ENABLE_EXAMPLES OFF CACHE INTERNAL "" FORCE)
set(HWY_ENABLE_TESTS OFF CACHE INTERNAL "" FORCE)
set(HWY_ENABLE_INSTALL OFF CACHE INTERNAL "" FORCE)
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/highway/CMakeLists.txt" AND NOT JPEGXL_FORCE_SYSTEM_HWY)
  add_subdirectory(highway)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/highway/LICENSE" ${PROJECT_BINARY_DIR}/LICENSE.highway COPYONLY)
else()
  find_package(HWY 1.0.7)
  if (NOT HWY_FOUND)
    message(FATAL_ERROR
        "Highway library (hwy) not found. Install libhwy-dev or download it "
        "to third_party/highway from https://github.com/google/highway . "
        "Highway is required to build JPEG XL. You can run "
        "${PROJECT_SOURCE_DIR}/deps.sh to download this dependency.")
  endif()
  if(JPEGXL_DEP_LICENSE_DIR)
    configure_file("${JPEGXL_DEP_LICENSE_DIR}/libhwy-dev/copyright"
                   ${PROJECT_BINARY_DIR}/LICENSE.highway COPYONLY)
  endif()  # JPEGXL_DEP_LICENSE_DIR
endif()

# brotli
if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/brotli/c/include/brotli/decode.h" OR JPEGXL_FORCE_SYSTEM_BROTLI)
  find_package(Brotli)
  if (NOT Brotli_FOUND)
    message(FATAL_ERROR
        "Brotli not found, install brotli-dev or download brotli source code to"
        " third_party/brotli from https://github.com/google/brotli. You can use"
        " ${PROJECT_SOURCE_DIR}/deps.sh to download this dependency.")
  endif ()
  if(JPEGXL_DEP_LICENSE_DIR)
    configure_file("${JPEGXL_DEP_LICENSE_DIR}/libbrotli-dev/copyright" ${PROJECT_BINARY_DIR}/LICENSE.brotli COPYONLY)
  endif()  # JPEGXL_DEP_LICENSE_DIR
else()
  # Compile brotli from sources.
  set(BROTLI_BUILD_TOOLS OFF CACHE STRING "")
  set(BROTLI_BUNDLED_MODE ON CACHE INTERNAL "")
  set(BROTLI_DISABLE_TESTS ON CACHE STRING "Disable Brotli tests")
  add_subdirectory(brotli)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/brotli/LICENSE" ${PROJECT_BINARY_DIR}/LICENSE.brotli COPYONLY)
endif()

# *cms
include(skcms.cmake)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/skcms/LICENSE" ${PROJECT_BINARY_DIR}/LICENSE.skcms COPYONLY)
