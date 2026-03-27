
option(JXLL_BUILD_WITH_WIC "use wic to for image i/o (windows)" ON)
option(JXLL_BUILD_WITH_FREEIMAGE "use freeimage to for image i/o" ON)
option(JXLL_BUILD_WITH_IMAGEMAGICK "use imagemagick to for image i/o" ON)
option(JXLL_BUILD_WITH_GUI "try to build a gui tool" ON)

# set(JXLL_BUILD_WITH_WIC OFF)
# set(JXLL_BUILD_WITH_FREEIMAGE ON)
#set(JXLL_BUILD_WITH_IMAGEMAGICK ON)

if (WIN32)
    include(CMakeLists.window.txt)
else()
    set(JXLL_BUILD_WITH_FREEIMAGE OFF)
    include(CMakeLists.linux.txt)
endif()
