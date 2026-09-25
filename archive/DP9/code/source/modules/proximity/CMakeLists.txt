# esp32p4 (tab5): the P4 has no native radio — ESP-NOW does not exist there
# at all (esp_wifi.h's config macros don't even preprocess without the WiFi
# Kconfig symbols), so this module can never run on it. Register empty; no
# P4 device.pcat lists proximity (purrstrap's radio-companion auto-add only
# fires when [radio] wifi = true).
idf_build_get_property(purr_idf_target IDF_TARGET)
if("${purr_idf_target}" STREQUAL "esp32p4")
    idf_component_register()
    return()
endif()

idf_component_register(
    SRCS
        proximity_module.c
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../../kernel/catcalls
    REQUIRES esp_common driver freertos esp_wifi esp_event esp_netif
)
