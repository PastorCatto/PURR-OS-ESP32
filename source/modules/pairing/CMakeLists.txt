# esp32p4 (tab5): same as proximity/CMakeLists.txt — pairing rides on
# proximity's ESP-NOW transport, which cannot exist on the radio-less P4.
idf_build_get_property(purr_idf_target IDF_TARGET)
if("${purr_idf_target}" STREQUAL "esp32p4")
    idf_component_register()
    return()
endif()

idf_component_register(
    SRCS
        pairing_module.c
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../../kernel/catcalls
        ../proximity
        ../proximity_rpc
        ../user_mgr
    # mbedtls — X25519 ECDH for the per-pairing shared secret (Phase A of
    # the remote-login work) + SHA-256 for the human-facing pairing code +
    # AES-GCM for the one-time Phase B access-request/key-registration
    # messages. Same primitives meshtastic's mesh_radio.c already uses for
    # its own PKI key exchange (mbedtls_ecp_group/mbedtls_ecdh_compute_
    # shared) — mirrored here rather than reused directly, since pairing
    # must not depend on meshtastic being present at all (see pairing.h's
    # own "not Heltec-specific" header comment).
    #
    # proximity_rpc — Phase B/C's USERAUTH_* actions ride on the same
    # request/response RPC layer app_manager_remote.c already uses for
    # Remote Apps, rather than pairing owning a second ESP-NOW frame type.
    #
    # user_mgr — Phase B/C need real account data (salt/hash/verify) and
    # create a USER_ACCOUNT_REMOTE record on a successful login; see
    # user_mgr.h's own "Remote-login support" section.
    REQUIRES esp_common driver freertos nvs_flash esp_hw_support proximity proximity_rpc user_mgr mbedtls
)
