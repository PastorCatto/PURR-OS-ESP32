#pragma once
// Drop-in replacement for rweather/Crypto's BlockCipher base class — see
// Cipher.h's own comment. vendor/Cryptography/CBC.h's CBCCommon holds a
// BlockCipher* and calls blockSize()/setKey()/encryptBlock()/decryptBlock()/
// clear() through it; that's the entire surface this needs to provide.

#include "Cipher.h"

class BlockCipher : public Cipher {
public:
    virtual size_t blockSize() const = 0;

    virtual void encryptBlock(uint8_t *output, const uint8_t *input) = 0;
    virtual void decryptBlock(uint8_t *output, const uint8_t *input) = 0;

    // Cipher's stream-style encrypt()/decrypt() aren't meaningful for a
    // bare block cipher on its own (CBC<T> is what actually chains
    // blocks) — AES128/AES256 below never call these; only encryptBlock()/
    // decryptBlock() are used, via CBCCommon.
    void encrypt(uint8_t *output, const uint8_t *input, size_t len) override {
        for (size_t off = 0; off + blockSize() <= len; off += blockSize()) {
            encryptBlock(output + off, input + off);
        }
    }
    void decrypt(uint8_t *output, const uint8_t *input, size_t len) override {
        for (size_t off = 0; off + blockSize() <= len; off += blockSize()) {
            decryptBlock(output + off, input + off);
        }
    }
};
