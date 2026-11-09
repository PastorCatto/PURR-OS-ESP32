#include <string.h>

#include "purr_abi.h"
#include "purr_bootimg.h"
#include "purr_util.h"
#include "testkit.h"

/* A made-up but well-formed ESP32-S3 image: header, one tiny "segment", then its SHA-256. */
static size_t make(uint8_t *img, size_t body, uint16_t chip, int hash_flag, int good_hash)
{
    memset(img, 0, body + 32);
    img[0] = 0xE9;
    img[1] = 2;
    img[12] = (uint8_t)(chip & 0xFF);
    img[13] = (uint8_t)(chip >> 8);
    img[23] = (uint8_t)hash_flag;
    for (size_t i = 24; i < body; i++) {
        img[i] = (uint8_t)(i * 7);
    }
    purr_sha256(img, body, img + body);
    if (!good_hash) {
        img[body + 5] ^= 0xFF;
    }
    return body + 32;
}

static void test_good_and_bad(void)
{
    static uint8_t img[0x9000];
    size_t n = make(img, 4096, PURR_BOOTIMG_CHIP_ESP32S3, 1, 1);
    CHECK(purr_bootimg_check(img, n, PURR_BOOTIMG_CHIP_ESP32S3) == NULL);

    n = make(img, 4096, PURR_BOOTIMG_CHIP_ESP32S3, 1, 0);
    CHECK(purr_bootimg_check(img, n, PURR_BOOTIMG_CHIP_ESP32S3) != NULL);          /* a wrong digest the ROM would refuse */

    n = make(img, 4096, 0x0005, 1, 1);
    CHECK(purr_bootimg_check(img, n, PURR_BOOTIMG_CHIP_ESP32S3) != NULL);          /* another chip's bootloader */

    n = make(img, 4096, PURR_BOOTIMG_CHIP_ESP32S3, 1, 1);
    img[0] = 0x00;
    CHECK(purr_bootimg_check(img, n, PURR_BOOTIMG_CHIP_ESP32S3) != NULL);          /* not an ESP image */

    n = make(img, 4096, PURR_BOOTIMG_CHIP_ESP32S3, 1, 1);
    img[1] = 0;
    CHECK(purr_bootimg_check(img, n, PURR_BOOTIMG_CHIP_ESP32S3) != NULL);          /* no segments */
}

static void test_sizes(void)
{
    static uint8_t img[PURR_BOOTIMG_MAX + 64];
    size_t n = make(img, PURR_BOOTIMG_MAX - 32, PURR_BOOTIMG_CHIP_ESP32S3, 1, 1);
    CHECK_EQ((int)n, (int)PURR_BOOTIMG_MAX);
    CHECK(purr_bootimg_check(img, n, PURR_BOOTIMG_CHIP_ESP32S3) == NULL);          /* exactly fills the space */
    n = make(img, PURR_BOOTIMG_MAX, PURR_BOOTIMG_CHIP_ESP32S3, 1, 1);
    CHECK(purr_bootimg_check(img, n, PURR_BOOTIMG_CHIP_ESP32S3) != NULL);          /* one byte past the partition table */
    CHECK(purr_bootimg_check(img, 10, PURR_BOOTIMG_CHIP_ESP32S3) != NULL);
    CHECK(purr_bootimg_check(NULL, 100, PURR_BOOTIMG_CHIP_ESP32S3) != NULL);
}

int main(void)
{
    test_good_and_bad();
    test_sizes();
    TK_DONE("test_bootimg");
}
