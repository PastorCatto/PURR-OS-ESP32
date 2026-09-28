#include "purr_verify.h"

#include <string.h>

#include "purr_util.h"

#define HASH_CHUNK 512u

static int parse_field(const char *text, size_t len, uint32_t *out)
{
    if (text[0] == '\0') {
        *out = 0;                       /* an empty version means "none" */
        return 0;
    }
    return purr_version_parse(text, len, out);
}

purr_verify_result_t purr_image_verify(const purr_verify_env_t *env,
                                       purr_read_fn rd, void *ctx,
                                       uint32_t image_size,
                                       purr_image_header_t *hdr_out)
{
    purr_image_header_t hdr;
    uint32_t image_version, min_boot;

    if (image_size < sizeof(hdr)) {
        return PURR_V_BAD_LAYOUT;
    }
    if (rd(ctx, 0, &hdr, sizeof(hdr)) != 0) {
        return PURR_V_IO_ERROR;
    }
    if (hdr_out) {
        *hdr_out = hdr;
    }

    if (hdr.magic != PURR_IMAGE_MAGIC) {
        return PURR_V_BAD_MAGIC;
    }
    if (hdr.header_version != PURR_IMAGE_HEADER_VERSION) {
        return PURR_V_BAD_VERSION;
    }
    if (hdr.header_size < sizeof(hdr) || hdr.header_size > image_size) {
        return PURR_V_BAD_LAYOUT;
    }
    if (hdr.image_type < PURR_IMG_OS || hdr.image_type > PURR_IMG_APP) {
        return PURR_V_BAD_TYPE;
    }
    if (hdr.chip_id != env->chip_id &&
        !(hdr.chip_id == PURR_CHIP_ANY && hdr.image_type == PURR_IMG_APP)) {
        return PURR_V_BAD_CHIP;
    }

    if (parse_field(hdr.version, sizeof(hdr.version), &image_version) != 0 ||
        parse_field(hdr.min_boot_version, sizeof(hdr.min_boot_version), &min_boot) != 0) {
        return PURR_V_BAD_VERSION;
    }
    if (env->bootloader_version < min_boot) {
        return PURR_V_TOO_OLD_BOOTLOADER;
    }
    if (env->enforce_floor && image_version < env->version_floor) {
        return PURR_V_BELOW_FLOOR;
    }

    /* The payload must sit after the header and inside the image. */
    if (hdr.payload_offset < hdr.header_size ||
        (uint64_t)hdr.payload_offset + hdr.payload_size > image_size) {
        return PURR_V_BAD_LAYOUT;
    }

    const purr_key_t *key = purr_keybag_find(env->bag, hdr.key_id);
    if (key == NULL) {
        return PURR_V_NO_KEY;
    }
    if (purr_keybag_is_revoked(env->bag, hdr.key_id)) {
        return PURR_V_REVOKED;
    }
    if (!purr_role_may_sign(key->role, hdr.image_type,
                            (uint8_t)PURR_FLAGS_SUBTYPE(hdr.flags))) {
        return PURR_V_ROLE_MISMATCH;
    }

    /* Payload hash, streamed so no full-image buffer is needed. */
    purr_sha256_t sha;
    uint8_t chunk[HASH_CHUNK], digest[PURR_SHA256_LEN];
    uint32_t done = 0;

    purr_sha256_init(&sha);
    while (done < hdr.payload_size) {
        uint32_t n = hdr.payload_size - done;
        if (n > HASH_CHUNK) {
            n = HASH_CHUNK;
        }
        if (rd(ctx, hdr.payload_offset + done, chunk, n) != 0) {
            return PURR_V_IO_ERROR;
        }
        purr_sha256_update(&sha, chunk, n);
        done += n;
    }
    purr_sha256_final(&sha, digest);
    if (memcmp(digest, hdr.payload_sha256, PURR_SHA256_LEN) != 0) {
        return PURR_V_BAD_HASH;
    }

    /* Signature over every header byte before the signature field. */
    purr_sha256(&hdr, PURR_IMAGE_SIGNED_LEN, digest);
    if (!env->crypto->verify_p256(key->pub, digest, hdr.signature)) {
        return PURR_V_BAD_SIGNATURE;
    }
    return PURR_V_OK;
}

const char *purr_verify_name(purr_verify_result_t r)
{
    switch (r) {
    case PURR_V_OK:                 return "ok";
    case PURR_V_BAD_MAGIC:          return "bad-magic";
    case PURR_V_BAD_VERSION:        return "bad-version";
    case PURR_V_BAD_LAYOUT:         return "bad-layout";
    case PURR_V_BAD_CHIP:           return "bad-chip";
    case PURR_V_BAD_TYPE:           return "bad-type";
    case PURR_V_TOO_OLD_BOOTLOADER: return "too-old-bootloader";
    case PURR_V_BELOW_FLOOR:        return "below-version-floor";
    case PURR_V_NO_KEY:             return "no-key";
    case PURR_V_REVOKED:            return "revoked";
    case PURR_V_ROLE_MISMATCH:      return "role-mismatch";
    case PURR_V_BAD_HASH:           return "bad-hash";
    case PURR_V_BAD_SIGNATURE:      return "bad-signature";
    case PURR_V_IO_ERROR:           return "io-error";
    }
    return "unknown";
}

int purr_mem_read(void *ctx, uint32_t offset, void *buf, uint32_t len)
{
    purr_mem_read_ctx_t *m = ctx;
    if ((uint64_t)offset + len > m->size) {
        return -1;
    }
    memcpy(buf, m->base + offset, len);
    return 0;
}
