#pragma once
// Drop-in replacement for rweather/Crypto's Cipher base class — just enough
// of the interface for vendor/Cryptography/CBC.h's CBCCommon to override
// (see ../vendor/VENDORED.md for why this exists instead of fetching
// rweather/Crypto). CBC.h/CBC.cpp are themselves vendored byte-identical;
// this only has to satisfy what they actually call through a Cipher*/
// BlockCipher* pointer.

#include <cstdint>
#include <cstddef>

class Cipher {
public:
    virtual ~Cipher() {}

    virtual size_t keySize() const = 0;
    virtual bool setKey(const uint8_t *key, size_t len) = 0;

    virtual size_t ivSize() const { return 0; }
    virtual bool setIV(const uint8_t *iv, size_t len) { (void)iv; (void)len; return false; }

    virtual void encrypt(uint8_t *output, const uint8_t *input, size_t len) = 0;
    virtual void decrypt(uint8_t *output, const uint8_t *input, size_t len) = 0;

    virtual void clear() = 0;
};
