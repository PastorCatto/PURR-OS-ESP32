# ST7123 (M5Stack Tab5) — esp32p4 only. The MIPI-DSI esp_lcd API, the PPA, and
# esp_driver_ppa itself exist only for the P4, so on every other target this
# registers an empty component instead of failing the build (the component is
# discovered by modulestrap's manifest for all devices; only tab5's glue
# references purr_module_st7123, so nothing links to it elsewhere).
# (idf_build_get_property, not ${IDF_TARGET} — the plain variable is unset
# during IDF's early requirements-expansion pass, which is when the
# esp_driver_ppa REQUIRES below would fail to resolve on non-P4 targets.)
idf_build_get_property(purr_idf_target IDF_TARGET)
if(NOT "${purr_idf_target}" STREQUAL "esp32p4")
    idf_component_register()
    return()
endif()

# Vendors lib_st7123 (esp_lcd_st7123 panel + esp_lcd_touch + esp_lcd_touch_st7123,
# Apache-2.0 Espressif) privately into this component — same pattern
# sx1262_rl/CMakeLists.txt uses for source/lib/lib_radiolib.
idf_component_register(
    SRCS
        st7123.c
        ../../../lib/lib_st7123/esp_lcd_st7123.c
        ../../../lib/lib_st7123/esp_lcd_touch.c
        ../../../lib/lib_st7123/esp_lcd_touch_st7123.c
    INCLUDE_DIRS
        .
        ../../../lib/lib_st7123/include
        ../../../kernel/core
        ../../../kernel/catcalls
    REQUIRES esp_common driver freertos esp_lcd esp_mm esp_hw_support esp_driver_ppa esp_timer
)

# The vendored esp_lcd_touch sources normally get these from their registry
# components' Kconfig / component-manager version stamping — supply them here.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CONFIG_ESP_LCD_TOUCH_MAX_POINTS=5
    ESP_LCD_TOUCH_ST7123_VER_MAJOR=1
    ESP_LCD_TOUCH_ST7123_VER_MINOR=0
    ESP_LCD_TOUCH_ST7123_VER_PATCH=2
)
