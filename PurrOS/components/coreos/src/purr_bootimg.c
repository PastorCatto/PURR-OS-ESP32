#include "purr_bootimg.h"

#include <string.h>

#include "purr_abi.h"
#include "purr_util.h"

const char *purr_bootimg_check(const uint8_t *img, size_t len, uint16_t chip_id)
{
    if (img == NULL || len < 24 + 32) {
        return "too small to be a bootloader";
    }
    if (len > PURR_BOOTIMG_MAX) {
        return "bigger than the space below the partition table";
    }
    if (img[0] != 0xE9) {
        return "not an ESP image (wrong magic)";
    }
    if (img[1] == 0 || img[1] > 16) {
        return "unbelievable segment count";
    }
    uint16_t chip = (uint16_t)(img[12] | (img[13] << 8));
    if (chip != chip_id) {
        return "built for a different chip";
    }
    if (img[23] == 1) {                                      /* hash_appended */
        uint8_t digest[PURR_SHA256_LEN];
        purr_sha256(img, len - 32, digest);
        if (memcmp(digest, img + len - 32, 32) != 0) {
            return "its own SHA-256 does not match";
        }
    }
    return NULL;
}
