#pragma once
// Drop-in replacement for rweather/Crypto's Ed25519 static-method class,
// backed by source/lib/lib_ed25519 (orlp/ed25519) — deliberately NOT
// mbedtls. See ../vendor/VENDORED.md: this exact ESP-IDF/mbedtls build has
// no classic (non-PSA) Ed25519 sign/verify API, and PSA crypto with
// twisted-Edwards support isn't enabled in this project's mbedtls config.
// lib_ed25519 is already vendored and proven in this tree — MeshCore's own
// Identity.cpp (source/modules/meshcore/vendor/Identity.cpp) is its first
// consumer; this is the second, not a new dependency.
//
// Method surface matches exactly what vendor/Cryptography/Ed25519.h calls:
// generatePrivateKey()/derivePublicKey()/sign()/verify(). Argument ORDER
// differs from lib_ed25519's own C functions (msg/msglen land in different
// positions) — mapped explicitly below, not just forwarded positionally.

#include <cstdint>
#include <cstddef>
#include "ed_25519.h"

namespace Ed25519 {

    // ed25519_create_seed() is lib_ed25519's own RNG-backed key generator
    // (uses the platform's arc4random/getrandom internally); orlp's
    // "private key" IS the seed here (this library doesn't distinguish
    // seed from expanded private key — same "no generating keys from a
    // seed" limitation vendor/Cryptography/Ed25519.h's own top comment
    // already notes about upstream's original rweather-based wrapper).
    inline void generatePrivateKey(uint8_t privateKey[32]) {
        ed25519_create_seed(privateKey);
    }

    inline void derivePublicKey(uint8_t publicKey[32], const uint8_t privateKey[32]) {
        ed25519_derive_pub(publicKey, privateKey);
    }

    inline void sign(uint8_t signature[64], const uint8_t privateKey[32], const uint8_t publicKey[32],
                      const uint8_t *message, size_t messageLen) {
        ed25519_sign(signature, message, messageLen, publicKey, privateKey);
    }

    inline bool verify(const uint8_t signature[64], const uint8_t publicKey[32],
                        const uint8_t *message, size_t messageLen) {
        return ed25519_verify(signature, message, messageLen, publicKey) != 0;
    }

}
