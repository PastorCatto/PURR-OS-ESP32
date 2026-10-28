/* purr_hwscan.c - see purr_hwscan.h. */
#include "purr_hwscan.h"

#include <string.h>

purr_pinmask_t purr_hw_reserved_s3(void)
{
    purr_pinmask_t m = 0;
    const int fixed[] = {0, 3, 45, 46, 19, 20};
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        m |= PURR_PINBIT(fixed[i]);
    }
    for (int p = 26; p <= 37; p++) {
        m |= PURR_PINBIT(p);                    /* flash (26-32) and octal PSRAM (33-37) */
    }
    for (int p = 22; p <= 25; p++) {
        m |= PURR_PINBIT(p);                    /* not on the chip */
    }
    for (int p = 49; p < 64; p++) {
        m |= PURR_PINBIT(p);
    }
    return m;
}

int purr_hw_candidate_pins(purr_pinmask_t reserved, purr_pinmask_t in_use, int *out, int max)
{
    int n = 0;
    for (int p = 0; p < 64 && n < max; p++) {
        if (((reserved | in_use) & PURR_PINBIT(p)) == 0) {
            out[n++] = p;
        }
    }
    return n;
}

typedef struct {
    uint8_t addr;
    const char *name, *compatible, *catcall;
    uint16_t reg;              /* ID register; reg_bytes 0 = none, address match only */
    uint8_t reg_bytes;
    uint8_t expect[3];
    uint8_t expect_len;
} i2c_entry_t;

/* More specific entries (with an ID check) come before the address-only fallback for the same address. */
static const i2c_entry_t k_table[] = {
    {0x5D, "GT911 touch controller",  "gt911",   "touch",   0x8140, 2, {'9', '1', '1'}, 3},
    {0x14, "GT911 touch controller",  "gt911",   "touch",   0x8140, 2, {'9', '1', '1'}, 3},
    {0x38, "FT6x36 touch controller", "ft6x36",  "touch",   0xA8,   1, {0x11}, 1},
    {0x55, "T-Deck keyboard",         "kbd_i2c", "keyboard", 0,     0, {0}, 0},
    {0x3C, "SSD1306 / SH1106 OLED",   "ssd1306", "display", 0,     0, {0}, 0},
    {0x3D, "SSD1306 / SH1106 OLED",   "ssd1306", "display", 0,     0, {0}, 0},
    {0x76, "BME280 sensor",           "bme280",  "sensor",  0xD0,  1, {0x60}, 1},
    {0x77, "BME280 sensor",           "bme280",  "sensor",  0xD0,  1, {0x60}, 1},
    {0x76, "BMP280 sensor",           "bmp280",  "sensor",  0xD0,  1, {0x58}, 1},
    {0x77, "BMP280 sensor",           "bmp280",  "sensor",  0xD0,  1, {0x58}, 1},
    {0x76, "BMP/BME sensor",          "bme280",  "sensor",  0,     0, {0}, 0},
    {0x77, "BMP/BME sensor",          "bme280",  "sensor",  0,     0, {0}, 0},
    {0x68, "MPU-6050 motion sensor",  "mpu6050", "sensor",  0x75,  1, {0x68}, 1},
    {0x69, "MPU-6050 motion sensor",  "mpu6050", "sensor",  0x75,  1, {0x68}, 1},
    {0x68, "MPU-9250 motion sensor",  "mpu9250", "sensor",  0x75,  1, {0x71}, 1},
    {0x68, "DS3231 / DS1307 clock",   "ds3231",  "rtc",     0,     0, {0}, 0},
    {0x51, "PCF8563 clock",           "pcf8563", "rtc",     0,     0, {0}, 0},
    {0x6A, "LSM6DS3 motion sensor",   "lsm6ds3", "sensor",  0x0F,  1, {0x6A}, 1},
    {0x6B, "LSM6DS3 motion sensor",   "lsm6ds3", "sensor",  0x0F,  1, {0x6A}, 1},
    {0x29, "VL53L0X distance sensor", "vl53l0x", "sensor",  0xC0,  1, {0xEE}, 1},
    {0x23, "BH1750 light sensor",     "bh1750",  "sensor",  0,     0, {0}, 0},
    {0x44, "SHT3x humidity sensor",   "sht3x",   "sensor",  0,     0, {0}, 0},
    {0x45, "SHT3x humidity sensor",   "sht3x",   "sensor",  0,     0, {0}, 0},
    {0x34, "AXP power controller",    "axp",     "power",   0,     0, {0}, 0},
};

int purr_hw_i2c_table_count(void) { return (int)(sizeof(k_table) / sizeof(k_table[0])); }

int purr_hw_i2c_identify(uint8_t addr, purr_hw_reg_read_fn rd, void *ctx, purr_hw_i2c_match_t *out)
{
    const i2c_entry_t *fallback = NULL;
    for (size_t i = 0; i < sizeof(k_table) / sizeof(k_table[0]); i++) {
        const i2c_entry_t *e = &k_table[i];
        if (e->addr != addr) {
            continue;
        }
        if (e->reg_bytes == 0) {
            if (fallback == NULL) {
                fallback = e;                           /* the first address-only entry for this address */
            }
            continue;
        }
        if (rd == NULL) {
            continue;
        }
        uint8_t got[3] = {0};
        if (rd(ctx, addr, e->reg, e->reg_bytes, got, e->expect_len) == 0 && memcmp(got, e->expect, e->expect_len) == 0) {
            *out = (purr_hw_i2c_match_t){e->name, e->compatible, e->catcall, 1};
            return 1;
        }
    }
    if (fallback != NULL) {
        *out = (purr_hw_i2c_match_t){fallback->name, fallback->compatible, fallback->catcall, 0};
        return 1;
    }
    return 0;
}
