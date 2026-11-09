// The app-side P-256 backend for purr_verify (the bootloader uses micro-ecc; this uses
// mbedtls, which the app already links for HTTPS).
#include <string.h>

#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"

#include "purr_verify.h"

static int verify_p256(const uint8_t pub[PURR_PUBKEY_LEN], const uint8_t hash[PURR_SHA256_LEN],
                       const uint8_t sig[PURR_SIG_LEN])
{
    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_mpi r, s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    uint8_t uncompressed[1 + PURR_PUBKEY_LEN];
    uncompressed[0] = 0x04;                        /* the uncompressed point marker */
    memcpy(uncompressed + 1, pub, PURR_PUBKEY_LEN);

    int ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
             mbedtls_ecp_point_read_binary(&grp, &q, uncompressed, sizeof(uncompressed)) == 0 &&
             mbedtls_mpi_read_binary(&r, sig, PURR_SIG_LEN / 2) == 0 &&
             mbedtls_mpi_read_binary(&s, sig + PURR_SIG_LEN / 2, PURR_SIG_LEN / 2) == 0 &&
             mbedtls_ecdsa_verify(&grp, hash, PURR_SHA256_LEN, &q, &r, &s) == 0;

    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

const purr_crypto_t purr_crypto_mbedtls = {verify_p256};
