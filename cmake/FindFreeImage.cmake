# Copyright (c) the JPEG XL Project Authors. All rights reserved.
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file.

# FindFreeImage
# ----------
# Find the FreeImage library

# Try pkg-config first (in case a .pc file is provided)
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
  # Allow user to specify additional pkg-config search paths via FREEIMAGE_PKG_CONFIG_PATH
  # Can be either CMake variable or environment variable
  if(DEFINED FREEIMAGE_PKG_CONFIG_PATH)
    set(_pkg_config_path "${FREEIMAGE_PKG_CONFIG_PATH}")
  elseif(DEFINED ENV{FREEIMAGE_PKG_CONFIG_PATH})
    set(_pkg_config_path "$ENV{FREEIMAGE_PKG_CONFIG_PATH}")
  endif()
  
  if(_pkg_config_path)
    # Save original PKG_CONFIG_PATH
    set(_original_pkg_config_path "$ENV{PKG_CONFIG_PATH}")
    # Prepend custom path to PKG_CONFIG_PATH
    if(_original_pkg_config_path)
      set(ENV{PKG_CONFIG_PATH} "${_pkg_config_path};${_original_pkg_config_path}")
    else()
      set(ENV{PKG_CONFIG_PATH} "${_pkg_config_path}")
    endif()
  endif()
  
  pkg_check_modules(PC_FREEIMAGE QUIET freeimage)
  if(PC_FREEIMAGE_FOUND)
    set(FreeImage_VERSION ${PC_FREEIMAGE_VERSION})
    set(FreeImage_INCLUDE_DIR ${PC_FREEIMAGE_INCLUDEDIR})
    set(FreeImage_LIBRARY ${PC_FREEIMAGE_LIBRARIES})
  endif()
  
  # Restore original PKG_CONFIG_PATH
  if(DEFINED _original_pkg_config_path)
    set(ENV{PKG_CONFIG_PATH} "${_original_pkg_config_path}")
  elseif(_pkg_config_path)
    set(ENV{PKG_CONFIG_PATH})
  endif()
endif()

# Determine search paths based on environment variables
# On Windows (CMAKE_SYSTEM_NAME == "Windows"), we try to find MSYS2 from PATH
# On other systems, we use the default Unix paths

# Priority:
# 1. User-specified FREEIMAGE_ROOT (environment variable or CMake variable)
# 2. On Windows: Auto-detect MSYS2 from PATH environment variable
# 3. Default Unix paths (for Linux/macOS compatibility)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  # We're on Windows - try to find MSYS2 from PATH
  
  # First check if user specified FREEIMAGE_ROOT directly
  if(DEFINED ENV{FREEIMAGE_ROOT})
    set(_FREEIMAGE_ROOT "$ENV{FREEIMAGE_ROOT}")
    message(STATUS "FreeImage: Using user-specified FREEIMAGE_ROOT: ${_FREEIMAGE_ROOT}")
  elseif(NOT PC_FREEIMAGE_FOUND)
    # Only auto-detect if pkg-config didn't find FreeImage
    # Try to find MSYS2 ucrt64 in PATH
    # Look for common MSYS2 paths in the PATH environment variable
    string(REPLACE "\\" "/" _path "$ENV{PATH}")
    string(REPLACE ";" ";" _path_list "${_path}")
    
    foreach(_path_entry IN LISTS _path_list)
      # Check if this path contains ucrt64/bin (MSYS2 UCRT64 environment)
      if(_path_entry MATCHES "ucrt64[/\\]?bin$")
        # Extract the MSYS2 root (parent of ucrt64)
        get_filename_component(_msys2_root "${_path_entry}" DIRECTORY)
        if(_msys2_root AND EXISTS "${_msys2_root}")
          set(_FREEIMAGE_ROOT "${_msys2_root}")
          message(STATUS "FreeImage: Auto-detected MSYS2 UCRT64 at: ${_FREEIMAGE_ROOT}")
          break()
        endif()
      endif()
    endforeach()
  endif()
else()
  # On non-Windows systems, check for FREEIMAGE_ROOT but don't auto-detect
  if(DEFINED ENV{FREEIMAGE_ROOT})
    set(_FREEIMAGE_ROOT "$ENV{FREEIMAGE_ROOT}")
  endif()
endif()

# FreeImage include directory (only if not found via pkg-config)
if(NOT FreeImage_INCLUDE_DIR)
  find_path(FreeImage_INCLUDE_DIR
    NAMES FreeImage.h
    HINTS
      ${PC_FREEIMAGE_INCLUDEDIR}
      ${PC_FREEIMAGE_INCLUDE_DIRS}
      ${_FREEIMAGE_ROOT}/include
      /usr/include
      /usr/local/include
    PATH_SUFFIXES FreeImage
  )
endif()

# FreeImage library (only if not found via pkg-config)
if(NOT FreeImage_LIBRARY)
  find_library(FreeImage_LIBRARY
    NAMES
      freeimage
      FreeImage
      libfreeimage
    HINTS
      ${PC_FREEIMAGE_LIBDIR}
      ${PC_FREEIMAGE_LIBRARY_DIRS}
      ${_FREEIMAGE_ROOT}/lib
      /usr/lib
      /usr/lib64
      /usr/local/lib
    PATH_SUFFIXES
  )
endif()

# FreeImagePlus library
find_library(FreeImagePlus_LIBRARY
  NAMES
    freeimageplus
    FreeImagePlus
    libfreeimageplus
  HINTS
    ${PC_FREEIMAGE_LIBDIR}
    ${PC_FREEIMAGE_LIBRARY_DIRS}
    ${_FREEIMAGE_ROOT}/lib
    /usr/lib
    /usr/lib64
    /usr/local/lib
  PATH_SUFFIXES
)

# Detect version from header if not found via pkg-config
if (FreeImage_INCLUDE_DIR AND NOT FreeImage_VERSION)
  if (EXISTS "${FreeImage_INCLUDE_DIR}/FreeImage.h")
    file(READ "${FreeImage_INCLUDE_DIR}/FreeImage.h" FREEIMAGE_H_CONTENT)
    string(REGEX MATCH "#define[ \t]+FREEIMAGE_MAJOR_VERSION[ \t]+([0-9]+)" _VER_MAJOR "${FREEIMAGE_H_CONTENT}")
    string(REGEX MATCH "#define[ \t]+FREEIMAGE_MINOR_VERSION[ \t]+([0-9]+)" _VER_MINOR "${FREEIMAGE_H_CONTENT}")
    string(REGEX MATCH "#define[ \t]+FREEIMAGE_RELEASE_VERSION[ \t]+([0-9]+)" _VER_RELEASE "${FREEIMAGE_H_CONTENT}")
    if (CMAKE_MATCH_1 AND CMAKE_MATCH_2 AND CMAKE_MATCH_3)
      set(FreeImage_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
    endif()
  endif()
endif()

# Handle REQUIRED_VARS and VERSION_VAR
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FreeImage
  REQUIRED_VARS
    FreeImage_LIBRARY
    FreeImage_INCLUDE_DIR
  VERSION_VAR FreeImage_VERSION
)

# Create imported target
if (FreeImage_LIBRARY AND NOT TARGET FreeImage::FreeImage)
  add_library(FreeImage::FreeImage UNKNOWN IMPORTED GLOBAL)
  set_target_properties(FreeImage::FreeImage PROPERTIES
    IMPORTED_LOCATION "${FreeImage_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${FreeImage_INCLUDE_DIR}"
  )
endif()

# Set output variables (both uppercase and lowercase)
if (FreeImage_FOUND)
  set(FreeImage_INCLUDE_DIRS ${FreeImage_INCLUDE_DIR})
  set(FreeImage_LIBRARIES ${FreeImage_LIBRARY})

  if (FreeImagePlus_LIBRARY)
    list(APPEND FreeImage_LIBRARIES ${FreeImagePlus_LIBRARY})
  endif()

  # Also set uppercase versions for compatibility
  set(FREEIMAGE_FOUND ${FreeImage_FOUND})
  set(FREEIMAGE_INCLUDE_DIR ${FreeImage_INCLUDE_DIR})
  set(FREEIMAGE_LIBRARY ${FreeImage_LIBRARY})
  set(FREEIMAGE_INCLUDE_DIRS ${FreeImage_INCLUDE_DIRS})
  set(FREEIMAGE_LIBRARIES ${FreeImage_LIBRARIES})
  set(FREEIMAGE_VERSION ${FreeImage_VERSION})
endif()

mark_as_advanced(FreeImage_INCLUDE_DIR FreeImage_LIBRARY FreeImagePlus_LIBRARY)
