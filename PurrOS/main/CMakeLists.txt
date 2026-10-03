idf_component_register(SRCS "main.c" "commands.c" "recovery_loader.c" "login.c"
                       REQUIRES kernel coreos littlefs esp_partition esp_system esp_timer esp_wifi
                                mbedtls esp_http_client app_update heap esp_mm
                                esp_driver_usb_serial_jtag vfs)
