set(srcs
    "src/purr_kernel.c"
    "src/st7789.c"
    "src/purr_gfx.c"
    "src/purr_console.c"
    "src/purr_cfgstore.c"
    "src/purr_fs.c"
    "src/purr_fs_flash.c"
    "src/purr_font_data.c")

if(CONFIG_PURR_BOARD_TDECK_PLUS)
    list(APPEND srcs "boards/tdeck_plus.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       REQUIRES coreos littlefs esp_partition driver esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_ledc heap log)
