# esp32p4 (tab5): same as pairing/proximity/proximity_rpc's own CMakeLists —
# this rides on proximity's ESP-NOW transport, which cannot exist on the
# radio-less P4.
idf_build_get_property(purr_idf_target IDF_TARGET)
if("${purr_idf_target}" STREQUAL "esp32p4")
    idf_component_register()
    return()
endif()

idf_component_register(
    SRCS
        server_mgr.c
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../pairing
        ../proximity_rpc
        ../claw_loader
        ../app_manager
        ../user_mgr
        ../wifi_mgr
    # mbedtls — AES-256-GCM for the WiFi-credential push, SHA256 for its
    # own key derivation from pairing's shared secret (same primitives
    # pairing_module.c already uses for USERAUTH/OOBE — mirrored here, not
    # reused directly, so this module doesn't need pairing_module.c's own
    # internal statics exposed beyond what pairing.h already publishes).
    #
    # claw_loader — personal-space storage (claw_loader_personal_root(),
    # source/modules/claw_loader/) for pushed-app staging/approval.
    # claw_loader_init() itself does nothing (see its own comment — no
    # real state to initialize) so this works whether or not a given
    # device also lists claw_loader in its own [modules] section.
    #
    # app_manager — app_manager_scan() after an approved app lands in the
    # real, scanned personal dir.
    #
    # user_mgr — user_mgr_default_username(), whose account an approved
    # app's personal space belongs to.
    #
    # wifi_mgr — REQUIRED to link (every symbol server_mgr.c references
    # must resolve), but NOT required to be functionally wired up on every
    # device: server_mgr.c only ever calls into it after confirming
    # purr_kernel_get_module("wifi_mgr") is non-NULL (i.e. that device's
    # own [modules] section actually lists it AND its init() ran) — see
    # server_mgr.h's own top comment on why that's still false for Heltec
    # today (a real WiFi-mode initialization-ordering hazard against
    # proximity_module.c's own lazy STA bring-up, not yet resolved).
    REQUIRES esp_common driver freertos mbedtls pairing proximity_rpc claw_loader app_manager user_mgr wifi_mgr
)
# Every personal_root()-based path in this file is built from
# claw_loader_personal_root()'s `const char *` return — genuinely always
# short in practice (either "/sdcard/personal" or "/flash/personal",
# claw_loader.c's own two hardcoded roots), but opaque to GCC's interprocedural
# string-length analysis, which has no way to know that and sees an
# unbounded pointer feeding a fixed-size snprintf() destination. No buffer
# size here would ever satisfy that check — same class of case milkbar's
# own CMakeLists.txt already documents this exact suppression for.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
