/*
 * purr_pbkdf2.h - HMAC-SHA256 and PBKDF2-HMAC-SHA256, for password hashing
 * (Users/SPEC.md section 2). Plain C, built on purr_sha256, no dependencies.
 */
#ifndef PURR_PBKDF2_H
#define PURR_PBKDF2_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void purr_hmac_sha256(const uint8_t *key, size_t key_len,
                      const uint8_t *data, size_t data_len, uint8_t out[32]);

/* One 32-byte block of PBKDF2-HMAC-SHA256: enough for a password hash to compare
 * against, not a general key-derivation function (no requested output length). */
void purr_pbkdf2_hmac_sha256(const uint8_t *password, size_t password_len,
                             const uint8_t *salt, size_t salt_len,
                             uint32_t iterations, uint8_t out[32]);

#ifdef __cplusplus
}
#endif

#endif /* PURR_PBKDF2_H */
