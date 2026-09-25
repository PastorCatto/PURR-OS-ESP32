idf_component_register(
    SRCS
        "drv_8086.c"
    INCLUDE_DIRS "."
    REQUIRES lib_purr_dos_ipc
)

# 8086tiny uses intentional operator precedence tricks, computed-but-unused
# expressions, and fall-through macro patterns. Suppress those warnings for
# this component — they are all intentional in the original source.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-parentheses
    -Wno-unused-value
    -Wno-misleading-indentation
    -Wno-implicit-fallthrough
    -Wno-address
    -Wno-error=parentheses
    -Wno-error=unused-value
    -Wno-error=misleading-indentation
    -Wno-error=implicit-fallthrough
    -Wno-error=address
)
