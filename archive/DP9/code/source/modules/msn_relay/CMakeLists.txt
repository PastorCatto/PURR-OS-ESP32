# esp32p4 (tab5): same as proximity_rpc/CMakeLists.txt — this rides on
# proximity_rpc, which cannot exist on the radio-less P4.
idf_build_get_property(purr_idf_target IDF_TARGET)
if("${purr_idf_target}" STREQUAL "esp32p4")
    idf_component_register()
    return()
endif()

idf_component_register(
    SRCS
        msn_relay.c
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../../kernel/catcalls
        ../meshtastic
        ../proximity_rpc
        ../pairing
    REQUIRES esp_common driver freertos meshtastic proximity_rpc pairing
)
