# Find ImageMagick (optional)

option(JXLL_BUILD_WITH_IMAGEMAGICK "use imagemagick to for image i/o" ON)
option(JXLL_BUILD_WITH_GUI "try to build a gui tool" ON)

find_package(PkgConfig REQUIRED)

set(ARGS)
set(HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/../lib ${CMAKE_BINARY_DIR}/lib/include)
set(LIBS jxl jxl_dec jxl_threads jxl_cms Threads::Threads)

if (WIN32)
    list(APPEND LIBS Comdlg32 Ole32 Shlwapi)
endif()

set(USE_IMAGEMAGICK FALSE)
if (JXLL_BUILD_WITH_IMAGEMAGICK)
    pkg_check_modules(IMAGEMAGICK IMPORTED_TARGET magick++)
    if (IMAGEMAGICK_FOUND)
        list(APPEND ARGS JXLL_USE_IMAGEMAGICK)
        list(APPEND HEADERS ${IMAGEMAGICK_INCLUDE_DIRS})
        list(APPEND LIBS PkgConfig::IMAGEMAGICK)
        message(STATUS "using ImageMagick for image I/O")
        set(USE_IMAGEMAGICK TRUE)
    endif()
endif()

if (NOT USE_IMAGEMAGICK)
    list(APPEND ARGS JXLL_USE_STB)
    message(STATUS "using stb_image/stb_image_write for image I/O")
endif()

if (JXLL_BUILD_WITH_GUI)
    pkg_check_modules(GTKMM IMPORTED_TARGET gtkmm-4.0)
    if (GTKMM_FOUND)
        list(APPEND HEADERS ${GTKMM_INCLUDE_DIRS})
        list(APPEND LIBS PkgConfig::GTKMM)
        message(STATUS "building gui tool with gtkmm")
        add_executable(gui gui.gtk.cc)
        target_compile_definitions(gui PRIVATE ${ARGS})
        target_include_directories(gui PRIVATE ${HEADERS})
        target_link_libraries(gui PRIVATE ${LIBS})
    endif()
endif()

#
# CLI
add_executable(cli cli.cc tinyfiledialogs.c)
target_compile_definitions(cli PRIVATE ${ARGS})
target_include_directories(cli PRIVATE ${HEADERS})
target_link_libraries(cli PRIVATE ${LIBS})

# # set_target_properties(cli PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)

# if (IMAGEMAGICK_FOUND)

#     message(STATUS "ImageMagick found, using it for image I/O")
# else()
#     target_link_libraries(cli PRIVATE jxl jxl_dec jxl_threads jxl_cms Threads::Threads)
#     target_compile_definitions(cli PRIVATE JXLL_USE_STB)
#     message(STATUS "ImageMagick not found, using stb_image for image I/O")
# endif()

# if ("${ImageMagick_Magick++_FOUND}" AND "${GTKMM4_FOUND}")
    
#     set_target_properties(gui PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
#     target_link_libraries(gui PRIVATE
#         jxl
#         jxl_dec
#         jxl_threads
#         jxl_cms
#         Threads::Threads
#         ImageMagick::Magick++
#         PkgConfig::GTKMM4
#     )
#     target_include_directories(gui PRIVATE
#         ${CMAKE_CURRENT_SOURCE_DIR}/../lib
#         ${CMAKE_BINARY_DIR}/lib/include
#     )
#     message(STATUS "gtkmm4 and ImageMagick found, building gui tool")
# else()
#     message(STATUS "Skipping gui tool (requires gtkmm-4.0 and ImageMagick++)")
# endif()
