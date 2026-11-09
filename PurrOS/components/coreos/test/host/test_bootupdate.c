#include <string.h>

#include "testkit.h"
#include "../../../../main/purr_bootupdate.c"       /* the real writer, over the fake flash below */

int g_have_battery, g_percent;

int purr_battery_get(int *percent, int *millivolts)       /* the test's own battery */
{
    if (!g_have_battery) {
        return 0;
    }
    *percent = g_percent;
    *millivolts = 3900;
    return 1;
}

#define AREA 0xA000u
static uint8_t flash[AREA + 0x1000];
static uint32_t erased[64];
static int n_erased;
static int fail_write_at = -1, fail_erase_at = -1;

esp_err_t esp_flash_read(esp_flash_t *chip, void *buffer, uint32_t address, uint32_t length)
{
    (void)chip;
    memcpy(buffer, flash + address, length);
    return ESP_OK;
}

esp_err_t esp_flash_erase_region(esp_flash_t *chip, uint32_t start, uint32_t len)
{
    (void)chip;
    if ((int)start == fail_erase_at) {
        return ESP_FAIL;
    }
    memset(flash + start, 0xFF, len);
    erased[n_erased++] = start;
    return ESP_OK;
}

esp_err_t esp_flash_write(esp_flash_t *chip, const void *buffer, uint32_t address, uint32_t length)
{
    (void)chip;
    if ((int)address == fail_write_at) {
        return ESP_FAIL;
    }
    const uint8_t *b = buffer;
    for (uint32_t i = 0; i < length; i++) {
        flash[address + i] &= b[i];               /* a real write can only turn 1s into 0s */
    }
    return ESP_OK;
}

static void fill(uint8_t *p, size_t n, uint8_t seed)
{
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(seed + i * 31);
    }
}

static void reset(const uint8_t *old)
{
    memset(flash, 0xFF, sizeof(flash));
    memcpy(flash, old, AREA);
    n_erased = 0;
    fail_write_at = fail_erase_at = -1;
}

static void test_write_and_restore(void)
{
    static uint8_t old[AREA], img[0x8A60];
    fill(old, AREA, 3);
    fill(img, sizeof(img), 9);
    memcpy(img, old, 4096);                                  /* the first sector happens to be the same: it must not be touched */

    reset(old);
    CHECK_EQ(purr_bootupdate_write(img, sizeof(img)), 0);
    CHECK(memcmp(flash, img, sizeof(img)) == 0);              /* the new image is there */
    CHECK_EQ(n_erased, 8);                                    /* 9 sectors cover 0x8A60 bytes; the unchanged first one was skipped */
    for (int i = 1; i < n_erased; i++) {
        CHECK(erased[i] < erased[i - 1]);                     /* last sector first, so the header sector is touched last (or not at all) */
    }
    CHECK(memcmp(flash + 0x9000, old + 0x9000, AREA - 0x9000) == 0);   /* beyond the new image: left alone */
}

static void test_failure_then_backup(void)
{
    static uint8_t old[AREA], img[0x8A60];
    fill(old, AREA, 5);
    fill(img, sizeof(img), 17);

    reset(old);
    fail_write_at = 0x3000;                                  /* a sector in the middle refuses to write */
    CHECK_EQ(purr_bootupdate_write(img, sizeof(img)), -1);   /* reported, not hidden */
    CHECK(memcmp(flash, old, AREA) != 0);                    /* the flash is now a mixture: this is why the backup exists */
    fail_write_at = -1;
    CHECK_EQ(purr_bootupdate_write(old, AREA), 0);           /* the backup goes straight back */
    CHECK(memcmp(flash, old, AREA) == 0);                    /* byte for byte what was there */

    reset(old);
    fail_erase_at = 0x1000;
    CHECK_EQ(purr_bootupdate_write(img, sizeof(img)), -1);
}

static void test_power(void)
{
    char why[96] = "";
    g_have_battery = 0;
    CHECK_EQ(purr_bootupdate_power_ok(why, sizeof(why)), 1);          /* no cell: on USB */
    g_have_battery = 1;
    g_percent = 49;
    CHECK_EQ(purr_bootupdate_power_ok(why, sizeof(why)), 0);
    CHECK(strstr(why, "49%") != NULL);
    g_percent = 50;
    CHECK_EQ(purr_bootupdate_power_ok(why, sizeof(why)), 1);
}

int main(void)
{
    test_write_and_restore();
    test_failure_then_backup();
    test_power();
    TK_DONE("test_bootupdate");
}
