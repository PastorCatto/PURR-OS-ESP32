idf_component_register(SRCS "main.c" "mini_lcd.c" "mini_touch.c" "mini_gfx.c" "mini_fonts.c" "mini_wifi.c" "mini_ui.c" "mini_link.c" "mini_health.c" "mini_px.c" "mini_setup.c" "../../Modules/proximity/purr_px_setup.c" "mini_logo.c"
                            "../../Modules/proximity/purr_prox.c" "../../Modules/proximity/purr_px_pair.c" "../../Modules/proximity/purr_px_crypto.c" "../../Modules/proximity/purr_px_link.c"
                            "../../Modules/proximity/uecc/px_uecc.c" "../../Modules/proximity/esp/purr_px_esp.c"
                            "../../Modules/vdisplay/purr_rdisp.c" "../../PurrOS/components/coreos/src/purr_cfg.c" "../../PurrOS/components/coreos/src/purr_util.c"
                       INCLUDE_DIRS "." "../../Modules/vdisplay" "../../Modules/proximity" "../../Modules/proximity/esp" "../../PurrOS/components/kernel/include" "../../PurrOS/components/coreos/include"
                       REQUIRES driver esp_wifi esp_netif esp_event nvs_flash lwip esp_timer esp_hw_support esp_partition esp_driver_spi esp_driver_i2c esp_driver_gpio)

