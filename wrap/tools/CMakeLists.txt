add_executable(jxlw_cli main.c tinyfiledialogs.c)

target_link_libraries(jxlw_cli PRIVATE jxlw)

if (WIN32)
    target_link_libraries(jxlw_cli PRIVATE comdlg32)
endif()

