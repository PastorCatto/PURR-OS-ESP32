# Hand-customized (modulestrap's own auto-generated template doesn't forward
# module.pcat's [build] requires field, and the real IDF component name for
# vendored LVGL is lvgl__lvgl, not lvgl) — mirrors cupcake/CMakeLists.txt's
# same CONFIG_PURR_UI_BACKEND_* guard pattern, minus the icon vendoring
# (Nougat has no chrome/icons yet — Phase 2).
if(CONFIG_PURR_UI_BACKEND_NOUGAT)
    set(NOUGAT_SRCS
        nougat_module.c
        nougat_hal.c
        nougat_win.c
    )
    set(NOUGAT_REQUIRES esp_common driver freertos nvs_flash esp_timer lvgl__lvgl app_manager)
else()
    set(NOUGAT_SRCS "")
    set(NOUGAT_REQUIRES "")
endif()

idf_component_register(
    SRCS ${NOUGAT_SRCS}
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../../kernel/catcalls
    REQUIRES esp_common driver freertos nvs_flash esp_timer lvgl__lvgl app_manager
)
