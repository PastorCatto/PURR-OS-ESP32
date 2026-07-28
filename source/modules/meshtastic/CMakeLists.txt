# Vendors nanopb + the Meshtastic protobufs directly into this component
# (source/lib/lib_nanopb, source/lib/lib_mesh_pb) rather than registering
# them as separate ESP-IDF components — modulestrap's component discovery
# only scans source/modules/*/module.pcat and source/drivers/*/*/driver.pcat,
# so a bare source/lib/ folder wouldn't be picked up on its own. Keeping the
# vendored sources private to this module's SRCS/INCLUDE_DIRS avoids adding
# a new "lib" concept to the build system for one module's dependency.
# ── Gating ───────────────────────────────────────────────────────────────────
#
# Only meshtastic_module.c is compiled unconditionally, and that is deliberate,
# not an oversight: it carries an #else branch that provides real, linkable
# no-op symbols for every caller (meshdiag, milkbar, msn, msn_backend_meshtastic,
# meshcore, sx1262_rl and the T-Deck boots) when the feature is off. Dropping it
# entirely would break those at link rather than disabling a feature.
#
# Everything else — the radio, router and BLE companion, plus vendored nanopb and
# the whole Meshtastic protobuf set — is gated on CONFIG_PURR_FEATURE_MESHTASTIC.
# That is the bulk of this component, and previously every device compiled and
# linked all of it whether or not it had a LoRa radio or the feature enabled.
#
# Same shape as the UI backends (mochi/, cupcake/, tabby/) gating on their
# CONFIG_PURR_UI_BACKEND_* symbol.
if(CONFIG_PURR_FEATURE_MESHTASTIC)
    set(MESH_SRCS
        meshtastic_module.c
        mesh_ble.c
        mesh_radio.c
        mesh_router.c
        ../../lib/lib_nanopb/pb_common.c
        ../../lib/lib_nanopb/pb_encode.c
        ../../lib/lib_nanopb/pb_decode.c
        ../../lib/lib_mesh_pb/meshtastic/mesh.pb.c
        ../../lib/lib_mesh_pb/meshtastic/portnums.pb.c
        ../../lib/lib_mesh_pb/meshtastic/channel.pb.c
        ../../lib/lib_mesh_pb/meshtastic/config.pb.c
        ../../lib/lib_mesh_pb/meshtastic/device_ui.pb.c
        ../../lib/lib_mesh_pb/meshtastic/module_config.pb.c
        ../../lib/lib_mesh_pb/meshtastic/telemetry.pb.c
        ../../lib/lib_mesh_pb/meshtastic/xmodem.pb.c
    )
else()
    # Stub branches only — see above.
    set(MESH_SRCS meshtastic_module.c mesh_ble.c)
endif()

idf_component_register(
    SRCS ${MESH_SRCS}
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../../kernel/catcalls
        ../../lib/lib_nanopb
        ../../lib/lib_mesh_pb
    REQUIRES esp_common driver freertos nvs_flash esp_timer mbedtls bt bt_mgr
)
