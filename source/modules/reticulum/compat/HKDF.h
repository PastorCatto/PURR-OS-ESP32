#pragma once
// Drop-in replacement for rweather/Crypto's HKDF<T> template class (RFC
// 5869), backed by whatever Hash-family type T is instantiated with (this
// tree only ever instantiates HKDF<SHA256> — see ../vendor/Cryptography/
// HKDF.cpp). Method surface matches exactly what that file calls:
// setKey(ikm, ikmLen) / setKey(ikm, ikmLen, salt, saltLen) to latch the
// input key material (and optional salt), then extract(out, outLen) to
// run the full HKDF-Extract-then-Expand and write the derived key —
// despite the name, rweather's real extract() does both RFC 5869 steps
// in one call (there's no separate expand() used anywhere in this tree),
// so that's what's implemented here. `info` (RFC 5869's optional
// application-context octets) is always empty — nothing in the vendored
// tree ever sets one.

#include <cstdint>
#include <cstring>
#include <vector>

template <typename HashType>
class HKDF {
public:
    void setKey(const uint8_t *ikm, size_t ikmLen) {
        ikm_.assign(ikm, ikm + ikmLen);
        salt_.clear();
    }

    void setKey(const uint8_t *ikm, size_t ikmLen, const uint8_t *salt, size_t saltLen) {
        ikm_.assign(ikm, ikm + ikmLen);
        salt_.assign(salt, salt + saltLen);
    }

    void extract(uint8_t *out, size_t outLen) {
        const size_t hashLen = HashType().hashSize();

        // RFC 5869 step 1 (Extract): PRK = HMAC-Hash(salt, IKM). An empty
        // salt is replaced with hashLen zero bytes, per the RFC.
        std::vector<uint8_t> effectiveSalt = salt_;
        if (effectiveSalt.empty()) effectiveSalt.assign(hashLen, 0);

        std::vector<uint8_t> prk(hashLen);
        {
            HashType h;
            h.resetHMAC(effectiveSalt.data(), effectiveSalt.size());
            h.update(ikm_.data(), ikm_.size());
            h.finalizeHMAC(effectiveSalt.data(), effectiveSalt.size(), prk.data(), hashLen);
        }

        // RFC 5869 step 2 (Expand): OKM = T(1) | T(2) | ... — info is
        // always empty here (see this file's own top comment).
        std::vector<uint8_t> prev;   // T(0) = empty
        size_t written = 0;
        uint8_t counter = 1;
        while (written < outLen) {
            HashType h;
            h.resetHMAC(prk.data(), prk.size());
            if (!prev.empty()) h.update(prev.data(), prev.size());
            h.update(&counter, 1);

            std::vector<uint8_t> block(hashLen);
            h.finalizeHMAC(prk.data(), prk.size(), block.data(), hashLen);

            size_t take = (outLen - written) < hashLen ? (outLen - written) : hashLen;
            memcpy(out + written, block.data(), take);
            written += take;
            prev.swap(block);
            counter++;
        }
    }

private:
    std::vector<uint8_t> ikm_;
    std::vector<uint8_t> salt_;
};
