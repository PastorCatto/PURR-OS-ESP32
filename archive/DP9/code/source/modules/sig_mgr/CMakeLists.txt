# sig_mgr — artifact signing/verification. lib_ed25519 sources listed
# directly (same pattern meshtastic's CMakeLists.txt uses for lib_nanopb/
# lib_mesh_pb — nothing under source/lib/ is its own IDF component).
#
# seed.c deliberately excluded: its ed25519_create_seed() is unusable on
# ESP32 either way it branches (#ifdef _WIN32 CryptGenRandom / else
# fopen("/dev/urandom") — no such VFS path on bare-metal FreeRTOS) and
# would be dead code at best; sig_mgr.c uses esp_fill_random() instead.
# key_exchange.c/add_scalar.c excluded too — not needed for sign/verify.
idf_component_register(
    SRCS
        sig_mgr.c
        ../../lib/lib_ed25519/fe.c
        ../../lib/lib_ed25519/ge.c
        ../../lib/lib_ed25519/keypair.c
        ../../lib/lib_ed25519/sc.c
        ../../lib/lib_ed25519/sha512.c
        ../../lib/lib_ed25519/sign.c
        ../../lib/lib_ed25519/verify.c
    INCLUDE_DIRS
        .
        ../../kernel/core
        ../../kernel/catcalls
        ../../lib/lib_ed25519
    REQUIRES
        esp_common
        freertos
        nvs_flash
        esp_hw_support
        mbedtls
)
