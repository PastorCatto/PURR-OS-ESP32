/* purr_util.h - CRC-32, SHA-256 and version helpers. Plain C, no dependencies. */
#ifndef PURR_UTIL_H
#define PURR_UTIL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320), same as zlib crc32. */
uint32_t purr_crc32(uint32_t crc, const void *data, size_t len);

/* SHA-256, streaming. */
typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    uint32_t buflen;
} purr_sha256_t;

void purr_sha256_init(purr_sha256_t *ctx);
void purr_sha256_update(purr_sha256_t *ctx, const void *data, size_t len);
void purr_sha256_final(purr_sha256_t *ctx, uint8_t out[32]);
void purr_sha256(const void *data, size_t len, uint8_t out[32]);

/*
 * Versions are text such as "1.2.3" or "1.0.0-dp10". Only the leading
 * major.minor.patch counts for ordering. A missing part is 0.
 * Packed form: major<<24 | minor<<16 | patch (each 0..255).
 * Returns 0 on success, -1 if the text is not a version.
 */
int purr_version_parse(const char *text, size_t max_len, uint32_t *out);

#define PURR_VERSION(maj, min, pat) \
    (((uint32_t)(maj) << 24) | ((uint32_t)(min) << 16) | (uint32_t)(pat))

#ifdef __cplusplus
}
#endif

#endif /* PURR_UTIL_H */
