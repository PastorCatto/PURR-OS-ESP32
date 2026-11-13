set(main_srcs "main.c" "commands.c" "recovery_loader.c" "recovery_screen.c" "login.c" "purr_slot_trailer.c" "purr_trust_os.c" "purr_resetnote_os.c"
              "purr_catrt.c" "purr_catrt_storage.c" "purr_settings.c" "purr_power_plat.c" "purr_catrt_power.c" "purr_catrt_files.c" "purr_battery.c" "purr_radio_bars.c" "purr_clock.c" "purr_bootupdate.c" "purr_kbdlight.c"
              "oobe_job.c" "oobe_ui.c" "recovery_app.c" "purr_hwscan_plat.c" "purr_hwsetup.c" "purr_netsettle.c" "purr_sysupdate.c" "oobe_firstboot.c" "purr_lan_discover.c" "purr_recovery_src.c" "purr_drvcmds.c" "purr_catrt_devices.c" "purr_store.c" "purr_builtin_apps.c" "purr_sd.c")
set(main_reqs kernel coreos littlefs esp_partition esp_system esp_timer esp_wifi
             mbedtls esp_http_client app_update heap esp_mm
             esp_driver_usb_serial_jtag vfs fatfs)

# LVGL (purr_lvgl.c, proof-of-life chunk): color-LCD T-Deck boards only for now -- their
# 16 MB flash has room for it (purros.bin: ~1.09 -> ~1.28 MB), waveshare154's 8 MB layout
# does not (found for real: a 0x120000-byte kittenos/loader partition overflowed by
# 0x19580 bytes once LVGL became reachable through commands.c's "lvgldemo"). Fitting it
# there is its own later chunk (shrinking something else, or a leaner LVGL config), not
# something to force through now.
if(CONFIG_PURR_BOARD_COLOR_LCD)
    list(APPEND main_srcs "purr_catrt_msg.c" "purr_notify.c" "purr_lvgl.c" "purr_ui_config.c" "purr_ui_assets.c" "purr_ui_service.c"
                          # the setup wizard (recovery loader and KittenOS first boot) and what it is made of: the theme and
                          # the Settings widgets, in every profile
                          "purr_w10m_theme.c" "purr_w10m_widgets.c" "purr_oobe.c" "purr_wallpaper.c")
    # The Windows 10 Mobile style shell (and the larger fonts it uses) is left out of the recovery
    # profile: KittenOS's slot is 1.5 MB and the recovery build had no room for it.
    if(NOT CONFIG_PURR_PROFILE_RECOVERY)
        # shared by every official shell: Settings, Developer Portal, task manager, permission prompt, the ui catcall windows
        list(APPEND main_srcs "purr_shell.c" "purr_shell_abi.c" "purr_w10m_settings.c" "purr_repo_dev.c"
                              "purr_w10m_devportal.c" "purr_catrt_ui.c" "purr_w10m_perm.c" "purr_w10m_tasks.c" "purr_systasks.c" "purr_catrt_game.c")
        # The Android style shell is built into every image: it is the one that always works. Any other shell (the Windows 10 Mobile one, purr_w10m.c) is a loadable
        # module built by `purrstrap shells build` from its own source and installed under /ui/shells/ (plan7.md); it is not part of the kernel.
        if(CONFIG_PURR_SHELL_TYPE)
            # The Type shell (plan7.md "The Type shell"): the same entry points, its own files. The Android shell's files are left out of this image, not changed.
            list(APPEND main_srcs "purr_tp.c" "purr_tp_home.c" "purr_tp_lock.c" "purr_tp_slot.c")
            # the typefaces (UI/fonts/README.md): generated bitmap fonts, one file per size
            file(GLOB tp_font_srcs CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/tpfonts/*.c")
            list(APPEND main_srcs ${tp_font_srcs})
        else()
            list(APPEND main_srcs "purr_android.c" "purr_android_home.c" "purr_android_shade.c" "purr_android_lock.c")
        endif()
    endif()
    # The d-pad navigation and the key grid (the on-screen keyboard): the Type shell uses them for every sheet, and a board with a gamepad uses them in every image
    # that has no shell above it (KittenOS's recovery app, the internet loader, the setup wizard). purr_tp_shim.c holds the weak answers for what a shell would give.
    if(CONFIG_PURR_SHELL_TYPE OR CONFIG_PURR_BOARD_PLAYTIME)
        list(APPEND main_srcs "purr_tp_kbd.c" "purr_tp_nav.c" "purr_tp_shim.c")
    endif()
    list(APPEND main_reqs lvgl)
endif()

idf_component_register(SRCS ${main_srcs} REQUIRES ${main_reqs})

# The Android shell's own picture pack (UI/android-assets, the Froyo art) is built here and embedded in the full OS image, next to the code that asks for the names, so a
# kernel always carries the pictures it needs: no separate file to go missing after an update or a reset (plan7.md "The Froyo pictures travel with the kernel").
# It sits in flash and is read in place (no internal RAM, no copy). A theme's pack still goes first (purr_ui_assets.c).
if(CONFIG_PURR_PROFILE_FULL AND (CONFIG_PURR_BOARD_COLOR_LCD))
    set(android_assets_dir "${CMAKE_CURRENT_LIST_DIR}/../../UI/android-assets")
    set(android_pack "${CMAKE_CURRENT_BINARY_DIR}/android_pack.bin")
    file(GLOB android_pngs CONFIGURE_DEPENDS "${android_assets_dir}/*.png")
    add_custom_command(OUTPUT "${android_pack}"
        COMMAND ${PYTHON} "${CMAKE_CURRENT_LIST_DIR}/../../purrstrap/purrstrap.py" uiassets build --board tdeck_plus --assets-dir "${android_assets_dir}" --out "${android_pack}" --alpha
        DEPENDS ${android_pngs} "${CMAKE_CURRENT_LIST_DIR}/../../purrstrap/scripts/uiassets.py"
        COMMENT "Packing the Android shell's pictures"
        VERBATIM)
    add_custom_target(android_pack_target DEPENDS "${android_pack}")
    add_dependencies(${COMPONENT_LIB} android_pack_target)
    target_add_binary_data(${COMPONENT_LIB} "${android_pack}" BINARY)
endif()
