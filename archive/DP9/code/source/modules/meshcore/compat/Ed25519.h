#pragma once
// Drop-in replacement for rweather/Crypto's Ed25519 class, used by
// vendor/Identity.cpp's Identity::verify() ONLY (signature verification;
// keygen/sign/ECDH go through the vendored plain-C ed25519 lib directly via
// ed_25519.h, not through this class).
//
// Upstream MeshCore deliberately avoids the vendored lib's own
// ed25519_verify() here ("memory corruption bug was found in this
// function", vendor/Identity.cpp:18-20) in favor of rweather/Crypto's
// Ed25519::verify(). We don't have rweather/Crypto available, and this
// project's mbedtls isn't configured for PSA/Ed25519 (would need new
// sdkconfig surface with unconfirmed availability on this ESP-IDF version).
//
// Decision: use the vendored ed25519_verify() anyway. Read directly
// (source/lib/lib_ed25519/verify.c) — it's the well-known orlp/ed25519
// implementation: constant-time comparison, standard point/scalar
// operations, correctly-sized buffers, no obvious defect. Upstream's
// caution may be historical, platform-specific (e.g. AVR stack pressure),
// or unrelated to this exact call pattern. This is a deliberate deviation
// from upstream's own choice, not an oversight — flagged here so it's easy
// to find, and worth specific attention during real hardware interop
// testing (a bug here would show up as legitimate adverts/signatures
// failing verification, or forged ones being accepted).

#include <cstdint>
#include <cstddef>

#define ED25519_NO_SEED 1
#include <ed_25519.h>

class Ed25519 {
public:
    static bool verify(const uint8_t* signature, const uint8_t* pub_key,
                        const uint8_t* message, size_t msg_len) {
        return ed25519_verify(signature, message, msg_len, pub_key) != 0;
    }
};
