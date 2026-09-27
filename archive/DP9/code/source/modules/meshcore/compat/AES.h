#pragma once
// Drop-in replacement for rweather/Crypto's AES128 class, backed by mbedtls.
// See ../vendor/VENDORED.md for why this exists instead of fetching rweather/Crypto.
//
// Method surface matches exactly what vendor/Utils.cpp calls: setKey(),
// encryptBlock(), decryptBlock() — ECB, one 16-byte block at a time (the
// vendored code handles chaining/padding itself).

#include <cstdint>
#include <cstddef>
#include <mbedtls/aes.h>

class AES128 {
public:
    AES128() {
        mbedtls_aes_init(&enc_ctx_);
        mbedtls_aes_init(&dec_ctx_);
    }
    ~AES128() {
        mbedtls_aes_free(&enc_ctx_);
        mbedtls_aes_free(&dec_ctx_);
    }

    void setKey(const uint8_t* key, size_t len) {
        mbedtls_aes_setkey_enc(&enc_ctx_, key, (unsigned int)(len * 8));
        mbedtls_aes_setkey_dec(&dec_ctx_, key, (unsigned int)(len * 8));
    }

    void encryptBlock(uint8_t* dest, const uint8_t* src) {
        mbedtls_aes_crypt_ecb(&enc_ctx_, MBEDTLS_AES_ENCRYPT, src, dest);
    }

    void decryptBlock(uint8_t* dest, const uint8_t* src) {
        mbedtls_aes_crypt_ecb(&dec_ctx_, MBEDTLS_AES_DECRYPT, src, dest);
    }

private:
    mbedtls_aes_context enc_ctx_;
    mbedtls_aes_context dec_ctx_;
};
