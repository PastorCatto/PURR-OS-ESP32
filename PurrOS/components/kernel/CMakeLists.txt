set(srcs
    "src/purr_kernel.c"
    "src/purr_driver.c"
    "src/st7789.c"
    "src/epd1in54.c"
    "src/purr_gfx.c"
    "src/purr_console.c"
    "src/purr_cfgstore.c"
    "src/purr_fs.c"
    "src/purr_fs_flash.c"
    "src/purr_net.c"
    "src/purr_netrec.c"
    "src/purr_fetch.c"
    "src/purr_crypto_mbedtls.c"
    "src/purr_userstore.c"
    "src/purr_font_data.c")

if(CONFIG_PURR_BOARD_TDECK_PLUS)
    list(APPEND srcs "boards/tdeck_plus.c")
endif()

if(CONFIG_PURR_BOARD_WAVESHARE154)
    list(APPEND srcs "boards/waveshare154.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       REQUIRES coreos littlefs esp_partition esp_wifi esp_netif esp_event nvs_flash
                                mbedtls esp_http_client esp-tls esp_timer
                                driver esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_ledc heap log)
