// The bootloader's P-256 backend for purr_verify, over micro-ecc (the same library ESP-IDF's
// own secure boot v2 uses, already vendored as the "micro-ecc" component).
#include "purr_verify.h"
#include "uECC.h"

static int verify_p256(const uint8_t pub[PURR_PUBKEY_LEN], const uint8_t hash[PURR_SHA256_LEN],
                       const uint8_t sig[PURR_SIG_LEN])
{
    return uECC_verify(pub, hash, PURR_SHA256_LEN, sig, uECC_secp256r1());
}

const purr_crypto_t purr_crypto_uecc = {verify_p256};
