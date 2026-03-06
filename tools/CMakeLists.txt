# Find ImageMagick (optional)
find_package(ImageMagick COMPONENTS Magick++ QUIET)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(GTKMM4 QUIET IMPORTED_TARGET gtkmm-4.0)
endif()

# Use C++ compiler for cli since we use C++ headers from libjxl and ImageMagick++
set_source_files_properties(cli.cpp PROPERTIES LANGUAGE CXX)

# Always include stb_image files
set(SRCS cli.cpp tinyfiledialogs.c stb_image_write.h stb_image.h)

add_executable(cli ${SRCS})
set_target_properties(cli PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)

if (ImageMagick_Magick++_FOUND)
    target_link_libraries(cli PRIVATE jxl jxl_dec jxl_threads jxl_cms Threads::Threads ImageMagick::Magick++)
    target_compile_definitions(cli PRIVATE HAVE_IMAGEMAGICK)
    message(STATUS "ImageMagick found, using it for image I/O")
else()
    target_link_libraries(cli PRIVATE jxl jxl_dec jxl_threads jxl_cms Threads::Threads)
    target_compile_definitions(cli PRIVATE HAVE_STB_IMAGE)
    message(STATUS "ImageMagick not found, using stb_image for image I/O")
endif()

# Include directories
target_include_directories(cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib
    ${CMAKE_BINARY_DIR}/lib/include
)

if (ImageMagick_Magick++_FOUND AND GTKMM4_FOUND)
    add_executable(gui gui.cc)
    set_target_properties(gui PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
    target_link_libraries(gui PRIVATE
        jxl
        jxl_dec
        jxl_threads
        jxl_cms
        Threads::Threads
        ImageMagick::Magick++
        PkgConfig::GTKMM4
    )
    target_include_directories(gui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../lib
        ${CMAKE_BINARY_DIR}/lib/include
    )
    message(STATUS "gtkmm4 and ImageMagick found, building gui tool")
else()
    message(STATUS "Skipping gui tool (requires gtkmm-4.0 and ImageMagick++)")
endif()
