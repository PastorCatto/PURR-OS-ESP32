#pragma once
// Drop-in replacement for rweather/Crypto's Hash base class — just enough
// for vendor/Cryptography/HMAC.h, which holds a std::unique_ptr<Hash> and
// calls update()/resetHMAC()/finalizeHMAC()/hashSize() through it, and for
// vendor/Cryptography/Hashes.cpp, which calls reset()/update()/finalize()
// directly on a concrete SHA256/SHA512 instance.

#include <cstdint>
#include <cstddef>

class Hash {
public:
    virtual ~Hash() {}

    virtual size_t hashSize() const = 0;

    virtual void reset() = 0;
    virtual void update(const void *data, size_t len) = 0;
    virtual void finalize(uint8_t *hash, size_t len) = 0;

    virtual void resetHMAC(const uint8_t *key, size_t keyLen) = 0;
    virtual void finalizeHMAC(const uint8_t *key, size_t keyLen, uint8_t *hash, size_t len) = 0;
};
