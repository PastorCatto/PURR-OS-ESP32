#include "testcrypto.h"

#include "uECC.h"

static uint32_t rng_state = 0x12345678u;

void tc_seed(uint32_t seed)
{
    rng_state = seed ? seed : 1u;
}

/* xorshift32: reproducible test keys. Not for real key material. */
static int test_rng(uint8_t *dest, unsigned size)
{
    for (unsigned i = 0; i < size; i++) {
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 17;
        rng_state ^= rng_state << 5;
        dest[i] = (uint8_t)(rng_state >> 8);
    }
    return 1;
}

void tc_make_key(uint8_t pub[64], uint8_t priv[32])
{
    uECC_set_rng(test_rng);
    uECC_make_key(pub, priv, uECC_secp256r1());
}

void tc_sign(const uint8_t priv[32], const uint8_t hash[32], uint8_t sig[64])
{
    uECC_set_rng(test_rng);
    uECC_sign(priv, hash, 32, sig, uECC_secp256r1());
}

static int uecc_verify(const uint8_t pub[64], const uint8_t hash[32], const uint8_t sig[64])
{
    return uECC_verify(pub, hash, 32, sig, uECC_secp256r1());
}

const purr_crypto_t purr_crypto_uecc = {uecc_verify};
