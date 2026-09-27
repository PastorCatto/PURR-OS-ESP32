idf_component_register(
    SRCS
        m5tab5_bsp.c
    INCLUDE_DIRS
        .
        ../../../kernel/core
        ../../../kernel/catcalls
    REQUIRES esp_common driver freertos esp_timer esp_lcd sdmmc fatfs
)
