# Find ImageMagick (optional)
find_package(ImageMagick COMPONENTS Magick++ QUIET)

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
