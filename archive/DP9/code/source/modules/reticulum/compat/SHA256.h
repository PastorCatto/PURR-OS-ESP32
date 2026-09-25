#pragma once
// Drop-in replacement for rweather/Crypto's SHA256 class, backed by
// mbedtls — extends ../../meshcore/compat/SHA256.h's approach with
// reset()/hashSize() (vendor/Cryptography/Hashes.cpp calls digest.reset()
// before update(); vendor/Cryptography/HMAC.h calls _hash->hashSize()).
// mbedtls's HMAC-capable md context supports both plain and HMAC framing
// on one context — resetHMAC()/finalizeHMAC() don't need the key handed
// back in, it's already latched by mbedtls_md_hmac_starts(). HMAC-SHA256
// is a fully specified standard (RFC 2104); this only needs to be
// conformant, not byte-identical to rweather/Crypto's internals, to
// interoperate with real Reticulum nodes.

#include "Hash.h"
#include <cstring>
#include <mbedtls/md.h>

class SHA256 : public Hash {
public:
    SHA256() {
        mbedtls_md_init(&ctx_);
        mbedtls_md_setup(&ctx_, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1 /* hmac-capable */);
        mbedtls_md_starts(&ctx_);
    }
    ~SHA256() override {
        mbedtls_md_free(&ctx_);
    }

    size_t hashSize() const override { return 32; }

    void reset() override {
        mbedtls_md_starts(&ctx_);
    }

    void update(const void *data, size_t len) override {
        mbedtls_md_update(&ctx_, (const unsigned char *)data, len);
    }

    void finalize(uint8_t *hash, size_t len) override {
        uint8_t full[32];
        mbedtls_md_finish(&ctx_, full);
        memcpy(hash, full, len < sizeof(full) ? len : sizeof(full));
    }

    void resetHMAC(const uint8_t *key, size_t keyLen) override {
        mbedtls_md_hmac_starts(&ctx_, key, keyLen);
    }

    void finalizeHMAC(const uint8_t * /*key*/, size_t /*keyLen*/, uint8_t *hash, size_t len) override {
        uint8_t full[32];
        mbedtls_md_hmac_finish(&ctx_, full);
        memcpy(hash, full, len < sizeof(full) ? len : sizeof(full));
    }

private:
    mbedtls_md_context_t ctx_;
};
