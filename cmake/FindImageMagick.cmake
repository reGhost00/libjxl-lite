# Copyright (c) the JPEG XL Project Authors. All rights reserved.
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file.

# FindImageMagick
# --------------
# Find the ImageMagick library (Magick++)
#
# This module defines
#  ImageMagick_FOUND        - True if ImageMagick was found
#  ImageMagick_INCLUDE_DIR  - Include directories for ImageMagick
#  ImageMagick_LIBRARY      - Library for ImageMagick
#  ImageMagick_VERSION      - Version of ImageMagick found
#
# The following variables are also defined for backward compatibility:
#  ImageMagick_INCLUDE_DIRS (same as ImageMagick_INCLUDE_DIR)
#  ImageMagick_LIBRARIES    (same as ImageMagick_LIBRARY)

# Try pkg-config first (primary method on most systems)
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
  # Try different possible pkg-config names for Magick++
  foreach(_pkg_name IN ITEMS "Magick++" "magick++" "ImageMagick++" "Magick++-7.Q16HDRI")
    pkg_check_modules(PC_IMAGEMAGICK QUIET ${_pkg_name})
    if (PC_IMAGEMAGICK_FOUND)
      break()
    endif()
  endforeach()

  # Also check for MagickCore and MagickWand (dependencies)
  pkg_check_modules(PC_MAGICKCORE QUIET MagickCore MagickCore-7.Q16HDRI)
  pkg_check_modules(PC_MAGICKWAND QUIET MagickWand MagickWand-7.Q16HDRI)

  set(ImageMagick_VERSION ${PC_IMAGEMAGICK_VERSION})
endif()

# ImageMagick include directory
find_path(ImageMagick_INCLUDE_DIR
  NAMES Magick++.h
  HINTS
    ${PC_IMAGEMAGICK_INCLUDEDIR}
    ${PC_IMAGEMAGICK_INCLUDE_DIRS}
    /usr/include
    /usr/include/ImageMagick-7
    /usr/local/include
    /usr/local/include/ImageMagick-7
  PATH_SUFFIXES ImageMagick-7 ImageMagick
)

# ImageMagick library (Magick++)
find_library(ImageMagick_LIBRARY
  NAMES
    Magick++
    magick++
    Magick++-7.Q16HDRI
  HINTS
    ${PC_IMAGEMAGICK_LIBDIR}
    ${PC_IMAGEMAGICK_LIBRARY_DIRS}
    /usr/lib
    /usr/lib64
    /usr/local/lib
)

# MagickCore library (required dependency)
find_library(MAGICKCORE_LIBRARY
  NAMES
    MagickCore
    MagickCore-7.Q16HDRI
  HINTS
    ${PC_MAGICKCORE_LIBDIR}
    ${PC_MAGICKCORE_LIBRARY_DIRS}
    /usr/lib
    /usr/lib64
    /usr/local/lib
)

# MagickWand library (optional dependency)
find_library(MAGICKWAND_LIBRARY
  NAMES
    MagickWand
    MagickWand-7.Q16HDRI
  HINTS
    ${PC_MAGICKWAND_LIBDIR}
    ${PC_MAGICKWAND_LIBRARY_DIRS}
    /usr/lib
    /usr/lib64
    /usr/local/lib
)

# Detect version from header if not found via pkg-config
if (ImageMagick_INCLUDE_DIR AND NOT ImageMagick_VERSION)
  # Try to read version from Magick++.h
  if (EXISTS "${ImageMagick_INCLUDE_DIR}/Magick++/Include.h")
    file(READ "${ImageMagick_INCLUDE_DIR}/Magick++/Include.h" IMAGEMAGICK_H_CONTENT)
    # Look for version define
    string(REGEX MATCH "#define[ \t]+MagickPPLibVersion[ \t]+([0-9]+)" _VER "${IMAGEMAGICK_H_CONTENT}")
    if (CMAKE_MATCH_1)
      # Version is stored as hex, e.g., 0x10E02 for 7.0.2
      math(EXPR _vmaj "(${CMAKE_MATCH_1} >> 24) & 0xFF")
      math(EXPR _vmin "(${CMAKE_MATCH_1} >> 16) & 0xFF")
      math(EXPR _vrev "(${CMAKE_MATCH_1} >> 8) & 0xFF")
      set(ImageMagick_VERSION "${_vmaj}.${_vmin}.${_vrev}")
    endif()
  endif()
endif()

# Handle REQUIRED_VARS and VERSION_VAR
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ImageMagick
  REQUIRED_VARS
    ImageMagick_LIBRARY
    ImageMagick_INCLUDE_DIR
  VERSION_VAR ImageMagick_VERSION
)

# Create imported target if library was found
if (ImageMagick_LIBRARY AND NOT TARGET ImageMagick::Magick++)
  add_library(ImageMagick::Magick++ UNKNOWN IMPORTED GLOBAL)
  set_target_properties(ImageMagick::Magick++ PROPERTIES
    IMPORTED_LOCATION "${ImageMagick_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${ImageMagick_INCLUDE_DIR}"
  )

  # Add MagickCore as dependency
  if (MAGICKCORE_LIBRARY)
    set_target_properties(ImageMagick::Magick++ PROPERTIES
      IMPORTED_LOCATION "${ImageMagick_LIBRARY}"
    )
    target_link_libraries(ImageMagick::Magick++ INTERFACE ${MAGICKCORE_LIBRARY})
  endif()

  # Add MagickWand as dependency if found
  if (MAGICKWAND_LIBRARY)
    target_link_libraries(ImageMagick::Magick++ INTERFACE ${MAGICKWAND_LIBRARY})
  endif()

  # Add compile options if available from pkg-config
  if (PC_IMAGEMAGICK_CFLAGS_OTHER)
    target_compile_options(ImageMagick::Magick++ INTERFACE ${PC_IMAGEMAGICK_CFLAGS_OTHER})
  endif()

  # Add link flags if available from pkg-config
  if (PC_IMAGEMAGICK_LDFLAGS_OTHER)
    target_link_options(ImageMagick::Magick++ INTERFACE ${PC_IMAGEMAGICK_LDFLAGS_OTHER})
  endif()
endif()

# Set output variables
if (ImageMagick_FOUND)
  set(ImageMagick_INCLUDE_DIRS ${ImageMagick_INCLUDE_DIR})
  set(ImageMagick_LIBRARIES ${ImageMagick_LIBRARY})

  # Add dependencies to the libraries list
  if (MAGICKCORE_LIBRARY)
    list(APPEND ImageMagick_LIBRARIES ${MAGICKCORE_LIBRARY})
  endif()
  if (MAGICKWAND_LIBRARY)
    list(APPEND ImageMagick_LIBRARIES ${MAGICKWAND_LIBRARY})
  endif()

  # For backward compatibility
  set(ImageMagick_INCLUDE_DIRS ${ImageMagick_INCLUDE_DIR})
endif()

mark_as_advanced(ImageMagick_INCLUDE_DIR ImageMagick_LIBRARY MAGICKCORE_LIBRARY MAGICKWAND_LIBRARY)
