idf_component_register(
    SRCS
        "main.c"
        "kitt.c"
        "device_config.c"
        "display_axs.c"
        "font8x8_basic.c"
        "touch_axs.c"
        "wifi_mgr.c"
        "power_mgr.c"
    INCLUDE_DIRS "."
    REQUIRES
        json
        nvs_flash
        spiffs
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        esp_hw_support
        driver
        esp_driver_i2c
        esp_driver_spi
        esp_driver_gpio
        esp_lcd
        espressif__esp_lcd_axs15231b
        espressif__esp_lcd_touch
)

# Build + flash the SPIFFS image (carries /system/kernel/device.json)
spiffs_create_partition_image(spiffs "${CMAKE_CURRENT_SOURCE_DIR}/../spiffs_image" FLASH_IN_PROJECT)
