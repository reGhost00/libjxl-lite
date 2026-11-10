#
#   jxlw
#   a small unified image codec for JPEG and JPEG XL
#   with an stb_image like api
#
# The library wraps libjxl (JXL) and lib/jpegli (JPEG) behind a single pure C
# API. See jxlw.h for the public interface.

if(WIN32)
    set(JXLW_ARGS WIN32_LEAN_AND_MEAN NOMINMAX)
    if (JXLW_USE_STD_CALL)
        list(APPEND JXLW_ARGS JXLW_API_STDCALL)
    endif()
else()
    set(JXLW_ARGS)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND JXLW_ARGS JXLW_DEBUG)
endif()

file(GLOB JXLW_SOURCES "./srcs/*.c" "./srcs/*.cc")

add_library(jxlw STATIC ${JXLW_SOURCES})
target_compile_definitions(jxlw PRIVATE ${JXLW_ARGS})
# The public header lives in this directory.
target_include_directories(jxlw PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# The jpegli headers are included as "lib/jpegli/...", so the project root has
# to be on the include path (private, it must not leak to the users).
target_include_directories(jxlw PRIVATE ${PROJECT_SOURCE_DIR})

# target_link_libraries(jxlw PUBLIC jxl)
target_link_libraries(jxlw PRIVATE jxl jpegli-static jxl_threads)


if (JXLW_BUILD_WITH_TOOLS)
    add_subdirectory(tools)
endif()
