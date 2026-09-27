/* testcrypto.h - the host P-256 backend (micro-ecc) and signing helpers for tests. */
#ifndef TESTCRYPTO_H
#define TESTCRYPTO_H

#include <stdint.h>

#include "purr_verify.h"

/* The crypto backend the verification code is given, backed by micro-ecc. */
extern const purr_crypto_t purr_crypto_uecc;

/* Test-only key generation and signing. The RNG is deterministic, never for real keys. */
void tc_seed(uint32_t seed);
void tc_make_key(uint8_t pub[64], uint8_t priv[32]);
void tc_sign(const uint8_t priv[32], const uint8_t hash[32], uint8_t sig[64]);

#endif /* TESTCRYPTO_H */
