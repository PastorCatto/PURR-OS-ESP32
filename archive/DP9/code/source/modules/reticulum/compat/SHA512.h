#pragma once
// Drop-in replacement for rweather/Crypto's SHA512 class, backed by
// mbedtls — same shape as SHA256.h in this directory. CONFIG_MBEDTLS_
// SHA512_C is confirmed enabled in this project's resolved sdkconfig.

#include "Hash.h"
#include <cstring>
#include <mbedtls/md.h>

class SHA512 : public Hash {
public:
    SHA512() {
        mbedtls_md_init(&ctx_);
        mbedtls_md_setup(&ctx_, mbedtls_md_info_from_type(MBEDTLS_MD_SHA512), 1 /* hmac-capable */);
        mbedtls_md_starts(&ctx_);
    }
    ~SHA512() override {
        mbedtls_md_free(&ctx_);
    }

    size_t hashSize() const override { return 64; }

    void reset() override {
        mbedtls_md_starts(&ctx_);
    }

    void update(const void *data, size_t len) override {
        mbedtls_md_update(&ctx_, (const unsigned char *)data, len);
    }

    void finalize(uint8_t *hash, size_t len) override {
        uint8_t full[64];
        mbedtls_md_finish(&ctx_, full);
        memcpy(hash, full, len < sizeof(full) ? len : sizeof(full));
    }

    void resetHMAC(const uint8_t *key, size_t keyLen) override {
        mbedtls_md_hmac_starts(&ctx_, key, keyLen);
    }

    void finalizeHMAC(const uint8_t * /*key*/, size_t /*keyLen*/, uint8_t *hash, size_t len) override {
        uint8_t full[64];
        mbedtls_md_hmac_finish(&ctx_, full);
        memcpy(hash, full, len < sizeof(full) ? len : sizeof(full));
    }

private:
    mbedtls_md_context_t ctx_;
};
