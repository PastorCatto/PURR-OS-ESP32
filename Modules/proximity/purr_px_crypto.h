/*
 * purr_px_crypto.h - the small symmetric primitives of the link (RemoteDisplay/SPEC.md section 4, plan1.md "Proximity rebuild"): SHA-256, HMAC-SHA-256, HKDF-SHA-256 and the ChaCha20-Poly1305
 * AEAD of RFC 8439. Portable C with no dependencies (no libc beyond memcpy and memset, which a freestanding module has), so the module, the Mini and the host tools run the same code and
 * the host tests check it against the published test vectors. (The elliptic-curve part, ECDH and ECDSA on P-256, is micro-ecc, supplied through purr_pair.h's table.)
 *
 * ChaCha20-Poly1305 rather than AES-GCM: no tables, no timing leaks from a software AES, and the same speed on an ESP32 and an S3 without leaning on a hardware block a module cannot reach.
 */
#ifndef PURR_PX_CRYPTO_H
#define PURR_PX_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    int n;
} purr_px_sha256_t;

void purr_px_sha256_init(purr_px_sha256_t *c);
void purr_px_sha256_update(purr_px_sha256_t *c, const void *data, size_t n);
void purr_px_sha256_final(purr_px_sha256_t *c, uint8_t out[32]);
void purr_px_sha256(const void *data, size_t n, uint8_t out[32]);

void purr_px_hmac_sha256(const uint8_t *key, size_t klen, const void *data, size_t n, uint8_t out[32]);

/* RFC 5869. out_len at most 255 * 32. */
void purr_px_hkdf_sha256(const uint8_t *salt, size_t slen, const uint8_t *ikm, size_t ilen, const uint8_t *info, size_t infolen, uint8_t *out, size_t out_len);

#define PURR_PX_AEAD_KEY 32
#define PURR_PX_AEAD_NONCE 12
#define PURR_PX_AEAD_TAG 16

/* ChaCha20 keystream XORed over data (RFC 8439 section 2.4), counter being the block counter to start from. */
void purr_px_chacha20_xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, const uint8_t *in, uint8_t *out, size_t n);
/* Poly1305 (RFC 8439 section 2.5). */
void purr_px_poly1305(const uint8_t key[32], const uint8_t *msg, size_t n, uint8_t tag[16]);

/* RFC 8439 section 2.8: out holds the ciphertext (pt_len bytes) followed by the 16-byte tag: pt_len + 16 bytes in all. */
void purr_px_aead_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t pt_len, uint8_t *out);
/* 0 and the plaintext (ct_len - 16 bytes) if the tag is right; -1 and nothing written otherwise. The tag is compared without stopping at the first different byte. */
int purr_px_aead_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t ct_len, uint8_t *pt);

#ifdef __cplusplus
}
#endif

#endif /* PURR_PX_CRYPTO_H */
