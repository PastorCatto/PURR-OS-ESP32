#pragma once
// Drop-in replacement for rweather/Crypto's Curve25519 static-method class,
// backed by ESP-IDF's mbedtls (MBEDTLS_ECP_DP_CURVE25519) — the same raw
// X25519 primitive pairing_module.c's own Phase A handshake already uses
// in this codebase (mbedtls_ecp_gen_keypair/mbedtls_ecdh_compute_shared,
// confirmed live). One real difference from that code: pairing_module.c
// SHA-256's its raw ECDH output before using it as a shared secret — that
// is PAIRING's own hardening choice, not part of the X25519 primitive
// itself. Reticulum's own X25519PrivateKey::exchange() (vendor/
// Cryptography/X25519.h) expects the RAW 32-byte ECDH output — its own
// higher-level key derivation does its own hashing on top — so eval()/
// dh2() below deliberately do NOT hash the result; doing so would produce
// shared secrets incompatible with the real Reticulum wire protocol.

#include <cstdint>
#include <cstring>

// mbedtls_ecp_point's X/Y/Z fields are wrapped in MBEDTLS_PRIVATE() in this
// mbedtls version — direct field access (Q.X, Q.Z below) needs this
// defined before the include, same as pairing_module.c's and mesh_radio.c's
// own identical #define for the identical reason.
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include <mbedtls/ecp.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

namespace Curve25519 {

    namespace detail {

        struct Drbg {
            mbedtls_entropy_context entropy;
            mbedtls_ctr_drbg_context ctr_drbg;
            bool ok = false;

            Drbg() {
                mbedtls_entropy_init(&entropy);
                mbedtls_ctr_drbg_init(&ctr_drbg);
                static const char *pers = "purr_rns_x25519";
                ok = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                            (const unsigned char *)pers, strlen(pers)) == 0;
            }
            ~Drbg() {
                mbedtls_ctr_drbg_free(&ctr_drbg);
                mbedtls_entropy_free(&entropy);
            }
        };

    } // namespace detail

    // basepoint == nullptr: derive the public key for `secret` (result =
    // secret * G, the standard RFC7748 base point mbedtls_ecp_group_load()
    // already sets as grp.G). Otherwise: X25519 ECDH (result = secret *
    // basepoint), basepoint being the peer's raw 32-byte public key.
    inline bool eval(uint8_t result[32], const uint8_t secret[32], const uint8_t *basepoint) {
        detail::Drbg rng;
        if (!rng.ok) return false;

        mbedtls_ecp_group grp;
        mbedtls_mpi d;
        mbedtls_ecp_point Q, R;
        mbedtls_ecp_group_init(&grp);
        mbedtls_mpi_init(&d);
        mbedtls_ecp_point_init(&Q);
        mbedtls_ecp_point_init(&R);

        bool ok = false;
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) goto done;
        if (mbedtls_mpi_read_binary_le(&d, secret, 32) != 0) goto done;

        if (!basepoint) {
            if (mbedtls_ecp_mul(&grp, &R, &d, &grp.G, mbedtls_ctr_drbg_random, &rng.ctr_drbg) != 0) goto done;
            if (mbedtls_mpi_write_binary_le(&R.X, result, 32) != 0) goto done;
        } else {
            mbedtls_mpi shared;
            mbedtls_mpi_init(&shared);
            // Curve25519 points in mbedtls are projective (X, Z) — Y is
            // unused for the Montgomery x25519 ladder. Z=1 marks this as
            // an affine input point (same construction pairing_module.c's
            // own ecdh_compute_shared() already uses).
            if (mbedtls_mpi_read_binary_le(&Q.X, basepoint, 32) != 0) { mbedtls_mpi_free(&shared); goto done; }
            if (mbedtls_mpi_lset(&Q.Z, 1) != 0) { mbedtls_mpi_free(&shared); goto done; }
            int rc = mbedtls_ecdh_compute_shared(&grp, &shared, &Q, &d, mbedtls_ctr_drbg_random, &rng.ctr_drbg);
            if (rc == 0) rc = mbedtls_mpi_write_binary_le(&shared, result, 32);
            mbedtls_mpi_free(&shared);
            if (rc != 0) goto done;
        }
        ok = true;

    done:
        mbedtls_ecp_point_free(&R);
        mbedtls_ecp_point_free(&Q);
        mbedtls_mpi_free(&d);
        mbedtls_ecp_group_free(&grp);
        return ok;
    }

    // Generates a fresh random keypair — both outputs are written.
    inline bool dh1(uint8_t publicKey[32], uint8_t privateKey[32]) {
        detail::Drbg rng;
        if (!rng.ok) return false;

        mbedtls_ecp_group grp;
        mbedtls_mpi d;
        mbedtls_ecp_point Q;
        mbedtls_ecp_group_init(&grp);
        mbedtls_mpi_init(&d);
        mbedtls_ecp_point_init(&Q);

        bool ok = false;
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) goto done;
        if (mbedtls_ecp_gen_keypair(&grp, &d, &Q, mbedtls_ctr_drbg_random, &rng.ctr_drbg) != 0) goto done;
        if (mbedtls_mpi_write_binary_le(&d, privateKey, 32) != 0) goto done;
        if (mbedtls_mpi_write_binary_le(&Q.X, publicKey, 32) != 0) goto done;
        ok = true;

    done:
        mbedtls_ecp_point_free(&Q);
        mbedtls_mpi_free(&d);
        mbedtls_ecp_group_free(&grp);
        return ok;
    }

    // On entry, sharedSecret holds the peer's raw public key; on success
    // it's overwritten in place with the actual shared secret, and
    // privateKey is zeroed (matches rweather/Crypto's real dh2() contract
    // — the ephemeral private scalar isn't needed again after this).
    inline bool dh2(uint8_t sharedSecret[32], uint8_t privateKey[32]) {
        uint8_t peerPublic[32];
        memcpy(peerPublic, sharedSecret, 32);
        bool ok = eval(sharedSecret, privateKey, peerPublic);
        if (ok) memset(privateKey, 0, 32);
        return ok;
    }

}
