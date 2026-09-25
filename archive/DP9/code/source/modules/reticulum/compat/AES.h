#pragma once
// Drop-in replacement for rweather/Crypto's AES128/AES256 classes, backed
// by mbedtls — same approach as ../../meshcore/compat/AES.h (that shim only
// needed AES128; this one also needs AES256 for vendor/Cryptography/AES.h's
// AES_256_CBC, used by Token's MODE_AES_256_CBC path). Method surface
// matches what vendor/Cryptography/CBC.h's CBCCommon calls on a BlockCipher*:
// blockSize(), setKey(), encryptBlock(), decryptBlock(), clear().

#include "BlockCipher.h"
#include <cstring>
#include <mbedtls/aes.h>

namespace compat_detail {

template <size_t KEY_BYTES>
class AESBase : public BlockCipher {
public:
    AESBase() {
        mbedtls_aes_init(&enc_ctx_);
        mbedtls_aes_init(&dec_ctx_);
    }
    ~AESBase() override {
        mbedtls_aes_free(&enc_ctx_);
        mbedtls_aes_free(&dec_ctx_);
    }

    size_t keySize() const override { return KEY_BYTES; }
    size_t blockSize() const override { return 16; }

    bool setKey(const uint8_t *key, size_t len) override {
        if (len != KEY_BYTES) return false;
        return mbedtls_aes_setkey_enc(&enc_ctx_, key, (unsigned int)(KEY_BYTES * 8)) == 0 &&
               mbedtls_aes_setkey_dec(&dec_ctx_, key, (unsigned int)(KEY_BYTES * 8)) == 0;
    }

    void encryptBlock(uint8_t *output, const uint8_t *input) override {
        mbedtls_aes_crypt_ecb(&enc_ctx_, MBEDTLS_AES_ENCRYPT, input, output);
    }
    void decryptBlock(uint8_t *output, const uint8_t *input) override {
        mbedtls_aes_crypt_ecb(&dec_ctx_, MBEDTLS_AES_DECRYPT, input, output);
    }

    void clear() override {
        mbedtls_aes_free(&enc_ctx_);
        mbedtls_aes_free(&dec_ctx_);
        mbedtls_aes_init(&enc_ctx_);
        mbedtls_aes_init(&dec_ctx_);
    }

private:
    mbedtls_aes_context enc_ctx_;
    mbedtls_aes_context dec_ctx_;
};

} // namespace compat_detail

class AES128 : public compat_detail::AESBase<16> {};
class AES256 : public compat_detail::AESBase<32> {};
