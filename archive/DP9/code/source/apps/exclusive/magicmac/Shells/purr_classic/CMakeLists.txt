idf_component_register(
    SRCS
        "purr_classic.cpp"
    INCLUDE_DIRS "."
    REQUIRES drv_umac lib_purr_ipc drv_display drv_touch
)
