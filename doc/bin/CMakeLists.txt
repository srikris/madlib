# ------------------------------------------------------------------------------
# MADlib Documentation Executable Files
# ------------------------------------------------------------------------------

set(BIN_FILES
    doxypy.py
)
add_files(BIN_TARGET_FILES . "${CMAKE_CURRENT_BINARY_DIR}" ${BIN_FILES})
add_custom_target(doxyBinFiles ALL DEPENDS ${BIN_TARGET_FILES})

list(APPEND
    M4_ARGUMENTS "\"-DMADLIB_SCHEMA=MADlib\""
)

join_strings(_M4_ARGUMENTS " " "${M4_ARGUMENTS}")
configure_file(py_filter.sh.in py_filter.sh @ONLY)
configure_file(sql_filter.sh.in sql_filter.sh @ONLY)