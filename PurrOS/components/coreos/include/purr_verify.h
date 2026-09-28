/*
 * purr_verify.h - container reading and verification
 * (PurrOS/components/coreos/SPEC.md section 3.5, bootloader/SPEC.md section 3).
 *
 * Plain C with no ESP-IDF dependency, so the bootloader and CoreOS share it
 * and it is tested on the PC. The image is read through a callback, and the
 * P-256 check goes through a small crypto interface (mbedtls in CoreOS,
 * micro-ecc in the bootloader).
 */
#ifndef PURR_VERIFY_H
#define PURR_VERIFY_H

#include <stdint.h>

#include "purr_abi.h"
#include "purr_keybag.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PURR_V_OK = 0,
    PURR_V_BAD_MAGIC,
    PURR_V_BAD_VERSION,        /* header version, or a version string that is not one */
    PURR_V_BAD_LAYOUT,         /* sizes or offsets outside the image */
    PURR_V_BAD_CHIP,
    PURR_V_BAD_TYPE,
    PURR_V_TOO_OLD_BOOTLOADER,
    PURR_V_BELOW_FLOOR,        /* older than the version floor (enforce mode) */
    PURR_V_NO_KEY,
    PURR_V_REVOKED,
    PURR_V_ROLE_MISMATCH,
    PURR_V_BAD_HASH,
    PURR_V_BAD_SIGNATURE,
    PURR_V_IO_ERROR
} purr_verify_result_t;

typedef struct {
    /* Returns nonzero if sig is a valid ECDSA P-256 signature of hash under pub. */
    int (*verify_p256)(const uint8_t pub[PURR_PUBKEY_LEN],
                       const uint8_t hash[PURR_SHA256_LEN],
                       const uint8_t sig[PURR_SIG_LEN]);
} purr_crypto_t;

/* Read len bytes at offset. Returns 0 on success. */
typedef int (*purr_read_fn)(void *ctx, uint32_t offset, void *buf, uint32_t len);

typedef struct {
    uint16_t             chip_id;             /* this device, PURR_CHIP_* */
    const purr_keybag_t *bag;
    const purr_crypto_t *crypto;
    uint32_t             bootloader_version;  /* packed */
    uint32_t             version_floor;       /* packed, 0 = none */
    int                  enforce_floor;       /* nonzero in enforce mode */
} purr_verify_env_t;

/*
 * Verify an image of image_size bytes. Cheap checks run first, then the
 * payload hash, then the signature. On PURR_V_OK (and for most failures after
 * the header was read) hdr_out, if not NULL, holds the header.
 */
purr_verify_result_t purr_image_verify(const purr_verify_env_t *env,
                                       purr_read_fn rd, void *ctx,
                                       uint32_t image_size,
                                       purr_image_header_t *hdr_out);

const char *purr_verify_name(purr_verify_result_t r);

/* A purr_read_fn over a plain in-memory buffer, for the several callers that already have
 * the whole image in RAM (a download, a file read off disk) rather than streaming it. */
typedef struct {
    const uint8_t *base;
    uint32_t size;
} purr_mem_read_ctx_t;

int purr_mem_read(void *ctx, uint32_t offset, void *buf, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* PURR_VERIFY_H */
