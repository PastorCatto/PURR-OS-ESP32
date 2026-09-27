#pragma once
// Drop-in replacement for rweather/Crypto's top-level Crypto.h umbrella
// header — vendor/Cryptography/CBC.cpp (byte-identical vendored code, see
// ../vendor/VENDORED.md) only actually calls one thing from it: clean(),
// a secure-zero helper for sensitive buffers (clean(iv); clean(temp);
// inside CBCCommon's destructor/clear()). A plain memset() can legally be
// optimized away by the compiler once the buffer's last real use has
// passed — volatile writes here are what rweather's own real
// implementation uses this exact trick for, and it's the actual point of
// calling clean() instead of memset() at all.

#include <cstdint>
#include <cstddef>

inline void clean(void *data, size_t len) {
    volatile uint8_t *p = (volatile uint8_t *)data;
    while (len--) *p++ = 0;
}

template <size_t N>
inline void clean(uint8_t (&data)[N]) {
    clean((void *)data, N);
}
