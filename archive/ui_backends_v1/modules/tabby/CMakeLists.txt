# tabby — keyboard-first launcher backend.
#
# Sources are gated on CONFIG_PURR_UI_BACKEND_TABBY so devices running a
# different backend don't carry Tabby's code (or drag LVGL in on a build that
# doesn't otherwise need it) — same shape as cupcake/CMakeLists.txt.
#
# The blackpurr icon .c files are vendored here by relative path for the same
# reason Cupcake vendors them: modulestrap's component discovery only scans
# source/modules/*/module.pcat and source/drivers/*/*/driver.pcat, so a bare
# source/assets/ folder is never auto-discovered as a component. Tabby's own
# launcher is text-first and uses none of them directly, but the system UI's
# Recents cards ask the host for a per-app icon (purr_systemui_host_t's
# icon_for_app), and these are the shared set every LVGL backend answers with.
if(CONFIG_PURR_UI_BACKEND_TABBY)
    set(TABBY_SRCS
        tabby_module.c
        tabby_hal.c
        tabby_shell.c
        tabby_win.c
        ../../assets/icons/generated/48/settings.c
        ../../assets/icons/generated/48/about.c
        ../../assets/icons/generated/48/terminal.c
        ../../assets/icons/generated/48/fileman.c
        ../../assets/icons/generated/48/calculator.c
        ../../assets/icons/generated/48/tools.c
    )
else()
    set(TABBY_SRCS "")
endif()

idf_component_register(
    SRCS ${TABBY_SRCS}
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../../kernel/catcalls
        ../../assets/icons
        ../systemui
    REQUIRES esp_common driver freertos nvs_flash lvgl__lvgl app_manager systemui
)
