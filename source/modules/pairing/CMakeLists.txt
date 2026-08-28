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
    # mbedtls — X25519 ECDH for the per-pairing shared secret (Phase A of
    # the remote-login work) + SHA-256 for the human-facing pairing code.
    # Same primitives meshtastic's mesh_radio.c already uses for its own
    # PKI key exchange (mbedtls_ecp_group/mbedtls_ecdh_compute_shared) —
    # mirrored here rather than reused directly, since pairing must not
    # depend on meshtastic being present at all (see pairing.h's own "not
    # Heltec-specific" header comment).
    REQUIRES esp_common driver freertos nvs_flash esp_hw_support proximity mbedtls
)
