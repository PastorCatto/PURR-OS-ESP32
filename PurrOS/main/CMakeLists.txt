set(main_srcs "main.c" "commands.c" "recovery_loader.c" "recovery_screen.c" "login.c" "purr_slot_trailer.c" "purr_trust_os.c"
              "purr_catrt.c" "purr_catrt_storage.c" "purr_settings.c" "purr_power_plat.c" "purr_catrt_power.c" "purr_catrt_files.c" "purr_battery.c" "purr_clock.c" "purr_bootupdate.c" "purr_kbdlight.c"
              "oobe_job.c" "oobe_ui.c" "recovery_app.c" "purr_hwscan_plat.c" "purr_hwsetup.c" "purr_sysupdate.c" "oobe_firstboot.c" "purr_lan_discover.c" "purr_recovery_src.c" "purr_drvcmds.c" "purr_catrt_devices.c")
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
                          "purr_card_shell.c"
                          # the setup wizard (recovery loader and KittenOS first boot) and what it is made of: the theme and
                          # the Settings widgets, in every profile
                          "purr_w10m_theme.c" "purr_w10m_widgets.c" "purr_oobe.c")
    # The Windows 10 Mobile style shell (and the larger fonts it uses) is left out of the recovery
    # profile: KittenOS's slot is 1.5 MB and the recovery build had no room for it.
    if(NOT CONFIG_PURR_PROFILE_RECOVERY)
        list(APPEND main_srcs "purr_w10m.c" "purr_w10m_settings.c" "purr_repo_dev.c"
                              "purr_w10m_devportal.c" "purr_catrt_ui.c" "purr_w10m_perm.c")
    endif()
    list(APPEND main_reqs lvgl)
endif()

idf_component_register(SRCS ${main_srcs} REQUIRES ${main_reqs})
