set(srcs
    "src/purr_kernel.c"
    "src/st7789.c"
    "src/purr_gfx.c"
    "src/purr_console.c"
    "src/purr_cfgstore.c"
    "src/purr_fs.c"
    "src/purr_fs_flash.c"
    "src/purr_net.c"
    "src/purr_netrec.c"
    "src/purr_fetch.c"
    "src/purr_crypto_mbedtls.c"
    "src/purr_font_data.c")

if(CONFIG_PURR_BOARD_TDECK_PLUS)
    list(APPEND srcs "boards/tdeck_plus.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       REQUIRES coreos littlefs esp_partition esp_wifi esp_netif esp_event nvs_flash
                                mbedtls esp_http_client esp-tls
                                driver esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_ledc heap log)
