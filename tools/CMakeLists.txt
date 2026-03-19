
option(JXLL_BUILD_WITH_WIC "use wic to for image i/o (windows)" ON)
option(JXLL_BUILD_WITH_FREEIMAGE "use freeimage to for image i/o" ON)
option(JXLL_BUILD_WITH_IMAGEMAGICK "use imagemagick to for image i/o" ON)
option(JXLL_BUILD_WITH_GUI "try to build a gui tool" ON)

set(JXLL_BUILD_WITH_WIC ON)
# set(JXLL_BUILD_WITH_FREEIMAGE OFF)
# set(JXLL_BUILD_WITH_IMAGEMAGICK OFF)

if (WIN32)
    include(CMakeLists.window.txt)
else()
    include(CMakeLists.linux.txt)
endif()
