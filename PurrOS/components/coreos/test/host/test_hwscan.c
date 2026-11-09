#include <string.h>

#include "purr_hwscan.h"
#include "testkit.h"

/* A fake bus: one device at one address with a few registers. */
typedef struct { uint8_t addr; uint16_t reg; uint8_t val[3]; int reads; } fake_t;

static int fake_rd(void *ctx, uint8_t addr, uint16_t reg, int reg_bytes, uint8_t *out, int len)
{
    fake_t *f = ctx;
    (void)reg_bytes;
    f->reads++;
    if (addr != f->addr || reg != f->reg) {
        return -1;
    }
    memcpy(out, f->val, (size_t)len);
    return 0;
}

static void test_pins(void)
{
    purr_pinmask_t r = purr_hw_reserved_s3();
    CHECK(r & PURR_PINBIT(0) && r & PURR_PINBIT(3) && r & PURR_PINBIT(45) && r & PURR_PINBIT(46));     /* straps */
    CHECK(r & PURR_PINBIT(19) && r & PURR_PINBIT(20));                                                  /* USB */
    CHECK(r & PURR_PINBIT(26) && r & PURR_PINBIT(37));                                                  /* flash, PSRAM */
    CHECK(!(r & PURR_PINBIT(18)) && !(r & PURR_PINBIT(8)) && !(r & PURR_PINBIT(10)) && !(r & PURR_PINBIT(48)));
    int pins[64];
    int n = purr_hw_candidate_pins(r, PURR_PINBIT(10) | PURR_PINBIT(18), pins, 64);
    for (int i = 0; i < n; i++) {
        CHECK(pins[i] != 10 && pins[i] != 18 && !(r & PURR_PINBIT(pins[i])));
        CHECK(i == 0 || pins[i] > pins[i - 1]);
    }
    CHECK(n > 20);
    CHECK_EQ(purr_hw_candidate_pins(r, 0, pins, 3), 3);              /* respects max */
}

static void test_identify(void)
{
    purr_hw_i2c_match_t m;
    fake_t gt = {0x5D, 0x8140, {'9', '1', '1'}, 0};
    CHECK(purr_hw_i2c_identify(0x5D, fake_rd, &gt, &m));
    CHECK(strcmp(m.compatible, "gt911") == 0 && m.by_id == 1);

    fake_t bme = {0x76, 0xD0, {0x60}, 0}, bmp = {0x76, 0xD0, {0x58}, 0};
    CHECK(purr_hw_i2c_identify(0x76, fake_rd, &bme, &m) && strcmp(m.compatible, "bme280") == 0 && m.by_id);
    CHECK(purr_hw_i2c_identify(0x76, fake_rd, &bmp, &m) && strcmp(m.compatible, "bmp280") == 0 && m.by_id);

    /* the ID register does not answer: fall back to the address, marked as a guess */
    fake_t dead = {0x00, 0, {0}, 0};
    CHECK(purr_hw_i2c_identify(0x76, fake_rd, &dead, &m) && m.by_id == 0);
    CHECK(purr_hw_i2c_identify(0x55, fake_rd, &dead, &m) && strcmp(m.compatible, "kbd_i2c") == 0 && m.by_id == 0);
    CHECK_EQ(dead.reads, 2);                                  /* two ID tries at 0x76 (BME280, BMP280); 0x55 has no ID register to try */

    /* MPU-6050 and MPU-9250 share an address; the ID register tells them apart */
    fake_t mpu = {0x68, 0x75, {0x71}, 0};
    CHECK(purr_hw_i2c_identify(0x68, fake_rd, &mpu, &m) && strcmp(m.compatible, "mpu9250") == 0 && m.by_id);

    CHECK(!purr_hw_i2c_identify(0x7E, fake_rd, &dead, &m));   /* unknown address */
    CHECK(purr_hw_i2c_identify(0x3C, NULL, NULL, &m) && strcmp(m.compatible, "ssd1306") == 0);   /* no reader: address only */
    CHECK(purr_hw_i2c_table_count() > 15);
}

int main(void)
{
    test_pins();
    test_identify();
    TK_DONE("test_hwscan");
}
