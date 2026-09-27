# rnode — RNode-over-BLE mode. Every file here is unconditionally compiled
# (source/modules/* always is — see meshtastic's own CMakeLists.txt
# comment for why that matters); rnode_ble.c is what's internally gated on
# CONFIG_BT_NIMBLE_ENABLED, with a real linkable no-op #else branch, same
# shape as mesh_ble.c.
#
# esp_wifi/esp_netif — rnode_module.c's own radio-companion offload/
# reload sequence's esp_wifi_stop()/_deinit()/_init()/_start() calls,
# tearing the WiFi driver itself down and back up (not just the PURR
# modules riding on top of it — see that section's own header comment for
# why both are needed). Deliberately NOT "REQUIRES pairing" even though
# the same offload sequence needs pairing_force_logout_all() — routed
# through a generic purr_kernel.h callback slot instead (purr_kernel_
# notify_radio_offload_needed()) specifically so a build that never lists
# "pairing" in its own [modules] (rnode_test) never links pairing.c's
# object code at all — confirmed live: a direct REQUIRES here grew that
# profile's flash usage from 89% to 99% of its OTA partition, since every
# PURR_MODULE_REGISTER() struct is a non-static global the linker can
# never prove is dead, dragging pairing.c's entire implementation in for
# nothing. See purr_kernel.h's own doc comment on that callback slot.
idf_component_register(
    SRCS
        rnode_module.c
        rnode_proto.c
        rnode_kiss.c
        rnode_ble.c
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../../kernel/catcalls
    REQUIRES esp_common driver freertos nvs_flash esp_timer bt bt_mgr esp_wifi esp_netif
)
