#pragma once
// Drop-in replacement for rweather/Crypto's SHA256 class, backed by mbedtls.
// See ../vendor/VENDORED.md for why this exists instead of fetching rweather/Crypto.
//
// Method surface matches exactly what vendor/Utils.cpp and vendor/Packet.cpp
// call: update()/finalize() for plain (truncatable) SHA-256, and
// resetHMAC()/finalizeHMAC() for HMAC-SHA256 (also truncatable). mbedtls's
// HMAC-capable md context supports both operation styles on one context, so
// finalizeHMAC() doesn't need the key again — it's already latched by
// resetHMAC() via mbedtls_md_hmac_starts(). HMAC-SHA256 is a fully specified
// standard (RFC 2104); this only needs to be conformant, not byte-identical
// to rweather/Crypto's internals, to interoperate with real MeshCore nodes.

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <mbedtls/md.h>

class SHA256 {
public:
    SHA256() {
        mbedtls_md_init(&ctx_);
        mbedtls_md_setup(&ctx_, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1 /* hmac-capable */);
        mbedtls_md_starts(&ctx_);
    }
    ~SHA256() {
        mbedtls_md_free(&ctx_);
    }

    // const void*, not const uint8_t* — matches rweather/Crypto's real
    // signature (confirmed: vendor/Packet.cpp calls sha.update(&path_len,
    // sizeof(path_len)) where path_len is uint16_t, which only compiles
    // against upstream because it accepts any pointer type via void*).
    void update(const void* data, size_t len) {
        mbedtls_md_update(&ctx_, (const unsigned char*)data, len);
    }

    void finalize(uint8_t* hash, size_t hash_len) {
        uint8_t full[32];
        mbedtls_md_finish(&ctx_, full);
        memcpy(hash, full, hash_len < sizeof(full) ? hash_len : sizeof(full));
    }

    void resetHMAC(const uint8_t* key, size_t key_len) {
        mbedtls_md_hmac_starts(&ctx_, key, key_len);
    }

    void finalizeHMAC(const uint8_t* /*key*/, size_t /*key_len*/, uint8_t* hash, size_t hash_len) {
        uint8_t full[32];
        mbedtls_md_hmac_finish(&ctx_, full);
        memcpy(hash, full, hash_len < sizeof(full) ? hash_len : sizeof(full));
    }

private:
    mbedtls_md_context_t ctx_;
};
