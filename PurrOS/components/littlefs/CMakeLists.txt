idf_component_register(SRCS "lfs.c" "lfs_util.c"
                       INCLUDE_DIRS ".")

# No logging from inside the library (it would need stdio and adds size).
target_compile_definitions(${COMPONENT_LIB} PRIVATE LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=all -Wno-unused-function)
