set(main_srcs "main.c" "commands.c" "recovery_loader.c" "login.c" "purr_slot_trailer.c" "purr_trust_os.c")
set(main_reqs kernel coreos littlefs esp_partition esp_system esp_timer esp_wifi
             mbedtls esp_http_client app_update heap esp_mm
             esp_driver_usb_serial_jtag vfs)

# LVGL (purr_lvgl.c, proof-of-life chunk): color-LCD T-Deck boards only for now -- their
# 16 MB flash has room for it (purros.bin: ~1.09 -> ~1.28 MB), waveshare154's 8 MB layout
# does not (found for real: a 0x120000-byte kittenos/loader partition overflowed by
# 0x19580 bytes once LVGL became reachable through commands.c's "lvgldemo"). Fitting it
# there is its own later chunk (shrinking something else, or a leaner LVGL config), not
# something to force through now.
if(CONFIG_PURR_BOARD_TDECK OR CONFIG_PURR_BOARD_TDECK_PLUS)
    list(APPEND main_srcs "purr_lvgl.c" "purr_ui_config.c" "purr_ui_assets.c" "purr_ui_service.c"
                          "purr_card_shell.c")
    list(APPEND main_reqs lvgl)
endif()

idf_component_register(SRCS ${main_srcs} REQUIRES ${main_reqs})
