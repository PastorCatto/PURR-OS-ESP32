/* BME280 temperature, pressure and humidity sensor (I2C): the first driver "Find hardware" can fetch for hardware it finds.
 *
 * Freestanding like every loadable driver: it reaches the chip only through the bus table. Its device comes from the discovered board
 * profile (/etc/board.conf), so dev->cfg is a purr_i2c_dev_cfg_t. read() takes one forced measurement and writes a line of text:
 *     T=23.45C P=1013.25hPa H=45.1%
 * The compensation is Bosch's own 32-bit integer routine from the BME280 datasheet (section 4.2.3), so no floating point.
 */
#include "purr_driver.h"
#include "purr_driver_bus.h"

static const purr_driver_bus_table_t *s_bus;

typedef struct {
    uint32_t h;                               /* the bus-table I2C handle */
    uint16_t T1, P1;
    int16_t T2, T3, P2, P3, P4, P5, P6, P7, P8, P9;
    uint8_t H1, H3;
    int16_t H2, H4, H5;
    int8_t H6;
    int in_use;
} bme_t;

static bme_t s_dev[2];

static int rd(uint32_t h, uint8_t reg, uint8_t *out, size_t n)
{
    return s_bus->i2c_write_read(h, &reg, 1, out, n, 50);
}

static int wr(uint32_t h, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return s_bus->i2c_write(h, b, 2, 50);
}

static uint16_t u16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static int16_t s16(const uint8_t *b) { return (int16_t)u16(b); }

static int bme_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    (void)board;
    const purr_i2c_dev_cfg_t *c = (const purr_i2c_dev_cfg_t *)dev->cfg;
    if (c == NULL) {
        return -1;
    }
    bme_t *d = NULL;
    for (int i = 0; i < 2; i++) {
        if (!s_dev[i].in_use) {
            d = &s_dev[i];
            break;
        }
    }
    if (d == NULL) {
        return -1;
    }
    uint32_t h = s_bus->i2c_add_device(c->port, c->sda, c->scl, c->addr, (int)c->hz);
    if (h == 0) {
        return -1;
    }
    uint8_t id = 0, cal1[24], h1 = 0, cal2[7];
    if (rd(h, 0xD0, &id, 1) != 0 || id != 0x60 || rd(h, 0x88, cal1, 24) != 0 || rd(h, 0xA1, &h1, 1) != 0 || rd(h, 0xE1, cal2, 7) != 0) {
        s_bus->i2c_remove_device(h);
        return -1;                            /* not a BME280 after all */
    }
    d->h = h;
    d->T1 = u16(cal1 + 0);  d->T2 = s16(cal1 + 2);  d->T3 = s16(cal1 + 4);
    d->P1 = u16(cal1 + 6);  d->P2 = s16(cal1 + 8);  d->P3 = s16(cal1 + 10); d->P4 = s16(cal1 + 12);
    d->P5 = s16(cal1 + 14); d->P6 = s16(cal1 + 16); d->P7 = s16(cal1 + 18); d->P8 = s16(cal1 + 20); d->P9 = s16(cal1 + 22);
    d->H1 = h1;
    d->H2 = s16(cal2 + 0);
    d->H3 = cal2[2];
    d->H4 = (int16_t)(((int8_t)cal2[3] << 4) | (cal2[4] & 0x0F));
    d->H5 = (int16_t)(((int8_t)cal2[5] << 4) | (cal2[4] >> 4));
    d->H6 = (int8_t)cal2[6];
    d->in_use = 1;
    *out_handle = d;
    return 0;
}

static void bme_remove(void *handle)
{
    bme_t *d = handle;
    if (d != NULL && d->in_use) {
        s_bus->i2c_remove_device(d->h);
        d->in_use = 0;
    }
}

/* Appends an unsigned number with `frac` decimal digits (value is scaled by 10^frac). Returns the new length. */
static size_t put_fixed(uint8_t *out, size_t n, size_t cap, int32_t v, int frac)
{
    if (v < 0 && n < cap) {
        out[n++] = '-';
        v = -v;
    }
    uint32_t scale = 1;
    for (int i = 0; i < frac; i++) {
        scale *= 10;
    }
    uint32_t whole = (uint32_t)v / scale, part = (uint32_t)v % scale;
    char tmp[12];
    int k = 0;
    do {
        tmp[k++] = (char)('0' + whole % 10);
        whole /= 10;
    } while (whole > 0 && k < 11);
    while (k > 0 && n < cap) {
        out[n++] = (uint8_t)tmp[--k];
    }
    if (frac > 0 && n < cap) {
        out[n++] = '.';
        for (uint32_t s = scale / 10; s > 0 && n < cap; s /= 10) {
            out[n++] = (uint8_t)('0' + (part / s) % 10);
        }
    }
    return n;
}

static size_t put_str(uint8_t *out, size_t n, size_t cap, const char *s)
{
    while (*s && n < cap) {
        out[n++] = (uint8_t)*s++;
    }
    return n;
}

static int bme_read(void *handle, uint8_t *data, size_t len)
{
    bme_t *d = handle;
    if (d == NULL || !d->in_use || len < 32) {
        return -1;
    }
    if (wr(d->h, 0xF2, 0x01) != 0 || wr(d->h, 0xF4, 0x25) != 0) {       /* humidity x1; temperature x1, pressure x1, forced mode */
        return -1;
    }
    s_bus->delay_ms(15);
    uint8_t m[8];
    if (rd(d->h, 0xF7, m, 8) != 0) {
        return -1;
    }
    int32_t adc_P = ((int32_t)m[0] << 12) | ((int32_t)m[1] << 4) | (m[2] >> 4);
    int32_t adc_T = ((int32_t)m[3] << 12) | ((int32_t)m[4] << 4) | (m[5] >> 4);
    int32_t adc_H = ((int32_t)m[6] << 8) | m[7];

    int32_t v1 = ((((adc_T >> 3) - ((int32_t)d->T1 << 1))) * (int32_t)d->T2) >> 11;
    int32_t v2 = (((((adc_T >> 4) - (int32_t)d->T1) * ((adc_T >> 4) - (int32_t)d->T1)) >> 12) * (int32_t)d->T3) >> 14;
    int32_t t_fine = v1 + v2;
    int32_t temp = (t_fine * 5 + 128) >> 8;                              /* 0.01 degC */

    int32_t p1 = (t_fine >> 1) - 64000;
    int32_t p2 = (((p1 >> 2) * (p1 >> 2)) >> 11) * (int32_t)d->P6;
    p2 = p2 + ((p1 * (int32_t)d->P5) << 1);
    p2 = (p2 >> 2) + ((int32_t)d->P4 << 16);
    p1 = (((d->P3 * (((p1 >> 2) * (p1 >> 2)) >> 13)) >> 3) + (((int32_t)d->P2 * p1) >> 1)) >> 18;
    p1 = ((32768 + p1) * (int32_t)d->P1) >> 15;
    uint32_t press = 0;
    if (p1 != 0) {
        uint32_t p = (((uint32_t)(1048576 - adc_P) - (uint32_t)(p2 >> 12))) * 3125u;
        p = p < 0x80000000u ? (p << 1) / (uint32_t)p1 : (p / (uint32_t)p1) * 2u;
        p1 = ((int32_t)d->P9 * (int32_t)(((p >> 3) * (p >> 3)) >> 13)) >> 12;
        p2 = ((int32_t)(p >> 2) * (int32_t)d->P8) >> 13;
        press = (uint32_t)((int32_t)p + ((p1 + p2 + d->P7) >> 4));       /* Pa */
    }

    int32_t x = t_fine - 76800;
    x = (((((adc_H << 14) - ((int32_t)d->H4 << 20) - ((int32_t)d->H5 * x)) + 16384) >> 15) *
         (((((((x * (int32_t)d->H6) >> 10) * (((x * (int32_t)d->H3) >> 11) + 32768)) >> 10) + 2097152) * (int32_t)d->H2 + 8192) >> 14));
    x = x - (((((x >> 15) * (x >> 15)) >> 7) * (int32_t)d->H1) >> 4);
    x = x < 0 ? 0 : x;
    x = x > 419430400 ? 419430400 : x;
    uint32_t hum = (uint32_t)(x >> 12);                                  /* 1/1024 %RH */

    size_t n = 0, cap = len - 1;
    n = put_str(data, n, cap, "T=");
    n = put_fixed(data, n, cap, temp, 2);
    n = put_str(data, n, cap, "C P=");
    n = put_fixed(data, n, cap, (int32_t)press, 2);                      /* Pa printed with two decimals is hPa: 101325 Pa -> 1013.25 */
    n = put_str(data, n, cap, "hPa H=");
    n = put_fixed(data, n, cap, (int32_t)((hum * 10u) / 1024u), 1);
    n = put_str(data, n, cap, "%");
    data[n] = 0;
    return (int)n;
}

static const purr_driver_t s_driver = {
    .name = "bme280", .compatible = "bme280",
    .probe = bme_probe, .remove = bme_remove, .read = bme_read,
    .bus_abi = PURR_DRIVER_BUS_ABI_VERSION,
};

__attribute__((used))
const purr_driver_t *bme280_entry(const purr_driver_bus_table_t *bus)
{
    s_bus = bus;
    return &s_driver;
}
