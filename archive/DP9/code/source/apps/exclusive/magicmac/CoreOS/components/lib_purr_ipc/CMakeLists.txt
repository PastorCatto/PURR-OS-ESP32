idf_component_register(
    SRCS
        "purr_ipc.c"
    INCLUDE_DIRS "."
    REQUIRES drv_lora drv_wifi drv_bt
)
