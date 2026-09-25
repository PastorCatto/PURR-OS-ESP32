# esp32p4 (tab5): same as proximity/CMakeLists.txt — this rides on
# proximity's ESP-NOW transport, which cannot exist on the radio-less P4.
idf_build_get_property(purr_idf_target IDF_TARGET)
if("${purr_idf_target}" STREQUAL "esp32p4")
    idf_component_register()
    return()
endif()

idf_component_register(
    SRCS
        proximity_rpc.c
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../../kernel/catcalls
        ../proximity
        ../pairing
    REQUIRES esp_common driver freertos esp_hw_support proximity pairing
)
