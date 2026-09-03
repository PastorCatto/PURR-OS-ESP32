# reticulum — vendors microReticulum (a C++ port of the Reticulum Network
# Stack) privately into this component. Same rationale as sx1262_rl's own
# CMakeLists.txt and meshcore's own CMakeLists.txt: modulestrap's component
# discovery only scans source/modules/*/module.pcat and source/drivers/*/*/
# driver.pcat, so bare source/lib/ or in-component vendor/thirdparty/ folders
# wouldn't be picked up on their own — everything below is pulled in as SRCS
# of this one component instead of registered as separate components.
#
# Stage 1 of the plan doc (vendor + compile, no radio interface, no app
# yet): Provisioning/*.cpp is excluded — RNS_USE_PROVISIONING is off, and
# wiring that subsystem up is deferred. Persistence/*.cpp IS included:
# Identity.h/Destination.h need those types unconditionally (IdentityEntry/
# DestinationEntry back onto microStore's HeapStore variant when RNS_USE_FS
# is off, not a real filesystem), not just when persistence is actually
# enabled.
#
# See vendor/VENDORED.md for exactly what's vendored where and why.

idf_component_register(
    SRCS
        reticulum_module.cpp
        rns_radio_adapter.cpp

        compat/RNG.cpp
        compat/nanosleep.cpp

        vendor/Bytes.cpp
        vendor/Channel.cpp
        vendor/Destination.cpp
        vendor/Identity.cpp
        vendor/Interface.cpp
        vendor/Link.cpp
        vendor/Log.cpp
        vendor/Packet.cpp
        vendor/Resource.cpp
        vendor/Reticulum.cpp
        vendor/Transport.cpp

        vendor/Cryptography/CBC.cpp
        vendor/Cryptography/Ed25519.cpp
        vendor/Cryptography/Fernet.cpp
        vendor/Cryptography/Hashes.cpp
        vendor/Cryptography/HKDF.cpp
        vendor/Cryptography/Token.cpp
        vendor/Cryptography/X25519.cpp

        vendor/Persistence/DestinationEntry.cpp
        vendor/Persistence/IdentityEntry.cpp

        vendor/Utilities/Compress.cpp
        vendor/Utilities/Crc.cpp
        vendor/Utilities/Memory.cpp
        vendor/Utilities/OS.cpp
        vendor/Utilities/Persistence.cpp

        ../../lib/lib_ed25519/add_scalar.c
        ../../lib/lib_ed25519/fe.c
        ../../lib/lib_ed25519/ge.c
        ../../lib/lib_ed25519/key_exchange.c
        ../../lib/lib_ed25519/keypair.c
        ../../lib/lib_ed25519/sc.c
        ../../lib/lib_ed25519/seed.c
        ../../lib/lib_ed25519/sha512.c
        ../../lib/lib_ed25519/sign.c
        ../../lib/lib_ed25519/verify.c
    INCLUDE_DIRS
        .
        compat
        vendor
        thirdparty/ArduinoJson/src
        thirdparty/MsgPack
        thirdparty/ArxContainer
        thirdparty/ArxTypeTraits
        thirdparty/DebugLog
        thirdparty/microStore/include
        ../../lib/lib_ed25519
        ../../kernel/core
        ../../kernel/catcalls
    REQUIRES esp_common driver freertos nvs_flash esp_hw_support mbedtls
)

# NATIVE: microReticulum's own Utilities/OS.h branches on this (matching its
# own native Linux/macOS test/example targets) for the non-ARDUINO code
# path — ESP-IDF-native is that same "not Arduino" branch, on real hardware
# instead of a desktop OS. RNS_LOG_LEVEL: default is RNS_LOG_LEVEL_VERBOSE
# (vendor/Log.h) if left unset, which is a lot of console spam for a Stage-
# 1 selftest; NOTICE is enough to see the selftest's own ESP_LOGI lines
# plus real warnings/errors from the stack itself.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    NATIVE
    # DEBUG (not NOTICE) for now — this stage's own two-board Announce
    # test wants to see Transport::inbound()'s "Adding destination ...
    # to path table" trace line directly, not just infer it from a size
    # delta. Dial back to NOTICE once the round trip is confirmed working.
    RNS_LOG_LEVEL=RNS_LOG_LEVEL_DEBUG
    # microStore::Adapters::NoopFileSystem — Transport::start() throws
    # ("FileSystem has not been registered") if NOTHING is registered at
    # all, even with RNS_USE_FS off (confirmed live: this is why the
    # Stage 2 radio test never got past "Transport::inbound: No
    # identity!" — the exception aborted transport identity setup before
    # it ever ran). A real flash-backed adapter is Stage 3's job (the
    # plan doc's own "adapter onto this device's actual VFS" item); this
    # unblocks Transport::start() with an always-empty, always-false
    # filesystem in the meantime — every op it makes already has a
    # graceful "doesn't exist / can't persist" fallback path.
    USTORE_USE_NOOPFS
)

# Suppress warnings from upstream/vendored sources (not our code to fix) —
# same suppression sx1262_rl's and meshcore's own CMakeLists.txt apply to
# their own vendored trees.
#
# -fpermissive: vendor/Transport.cpp has several `catch (const std::
# exception&) { ... } catch (const std::bad_alloc&) { ... }` blocks — a
# catch-all positioned before a more specific handler, which is ill-formed
# C++ that GCC treats as a hard error by default (this exact xtensa-esp-elf
# GCC 13.2.0 toolchain, confirmed live) unless downgraded. Not our code to
# fix (byte-identical vendoring, see vendor/VENDORED.md) — a real upstream
# bug this particular compiler happens to be strict about, not something
# introduced by vendoring it here.
target_compile_options(${COMPONENT_LIB} PRIVATE -w -fpermissive)
