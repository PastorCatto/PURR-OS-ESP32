/* Real loadable touch driver -- converted off purr_kernel.c's compiled-in gt911_probe/
 * gt911_remove plus the chip-specific logic that used to live OUTSIDE the driver entirely, in
 * purr_kernel_touch_read() itself (status/point register reads, the keepalive quirk, the
 * swap+mirror+rescale math). purr_driver_t's generic read() (purr_driver_bus.h) has no notion
 * of "x/y coordinates" -- it is a plain byte read, same as the keyboard's -- so this driver
 * packs (x, y) as four bytes (x low, x high, y low, y high, little-endian) into the caller's
 * buffer and returns 4 on a real touch, 0 on no touch, matching purr_driver_read_fn's own
 * POSIX-read() convention (purr_kernel.c's purr_kernel_touch_read() unpacks the same four
 * bytes back into uint16_t x/y -- see that file's own comment).
 *
 * Every register, address, and the rescale math below is unchanged from the archive-derived
 * driver this replaces (purr_kernel.c's own comment on where it came from) -- moved, not
 * reinvented. This module is freestanding (Modules/SPEC.md section 2/4): no libc, no
 * i2c_master.h, everything through purr_driver_bus_table_t.
 */
#include "purr_driver.h"
#include "purr_driver_bus.h"

static const purr_driver_bus_table_t *s_bus;

#define GT911_I2C_ADDR_PRIMARY   0x5D
#define GT911_I2C_ADDR_ALT       0x14

#define GT911_REG_STATUS         0x814E
#define GT911_REG_POINT1         0x8150
#define GT911_REG_COMMAND        0x8040
#define GT911_REG_CONFIG_START   0x8047
#define GT911_CONFIG_SIZE        184
#define GT911_REG_CONFIG_CHKSUM  0x80FF
#define GT911_REG_CONFIG_FRESH   0x8100
#define GT911_CFG_OFS_X_MAX_L    1
#define GT911_CFG_OFS_X_MAX_H    2
#define GT911_CFG_OFS_Y_MAX_L    3
#define GT911_CFG_OFS_Y_MAX_H    4

#define GT911_STATUS_TOUCH_MASK  0x0F
#define GT911_STATUS_BUFFER_STAT 0x80

#define GT911_KEEPALIVE_MS       2000
#define GT911_IO_TIMEOUT_MS      50

typedef struct __attribute__((packed)) {
    uint8_t x_low, x_high, y_low, y_high;
} gt911_point_t;

/* Only one GT911 is ever bound at a time on real hardware -- a plain static instead of a
 * freestanding-safe allocator (this module has no malloc). probe() fills it and returns its
 * address as the opaque handle. */
typedef struct {
    uint32_t i2c;
    uint16_t width, height;
    uint16_t native_x_max, native_y_max;
    uint32_t keepalive_at_ms;
} gt911_state_t;

static gt911_state_t s_state;

static int gt911_write_reg(uint32_t i2c, uint16_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[2 + GT911_CONFIG_SIZE];
    buf[0] = (uint8_t)(reg >> 8);
    buf[1] = (uint8_t)(reg & 0xFF);
    for (size_t i = 0; i < len; i++) {
        buf[2 + i] = data[i];
    }
    return s_bus->i2c_write(i2c, buf, 2 + len, GT911_IO_TIMEOUT_MS);
}

static int gt911_read_reg(uint32_t i2c, uint16_t reg, uint8_t *data, size_t len)
{
    uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    return s_bus->i2c_write_read(i2c, addr, sizeof(addr), data, len, GT911_IO_TIMEOUT_MS);
}

static int gt911_clear_status(uint32_t i2c)
{
    uint8_t zero = 0x00;
    return gt911_write_reg(i2c, GT911_REG_STATUS, &zero, 1);
}

/* Same resolution-fix the compiled driver did at probe time -- the chip's own X/Y_OUTPUT_MAX
 * config has to match the real panel, and a single register poke is ignored unless the whole
 * config block's checksum is recomputed and the "fresh" flag set. */
static void gt911_fix_resolution(uint32_t i2c, uint16_t width, uint16_t height)
{
    uint8_t cfg[GT911_CONFIG_SIZE];
    if (gt911_read_reg(i2c, GT911_REG_CONFIG_START, cfg, sizeof(cfg)) != 0) {
        return;
    }
    uint16_t old_x = (uint16_t)cfg[GT911_CFG_OFS_X_MAX_L] | ((uint16_t)cfg[GT911_CFG_OFS_X_MAX_H] << 8);
    uint16_t old_y = (uint16_t)cfg[GT911_CFG_OFS_Y_MAX_L] | ((uint16_t)cfg[GT911_CFG_OFS_Y_MAX_H] << 8);
    if (old_x == width && old_y == height) {
        return;
    }
    cfg[GT911_CFG_OFS_X_MAX_L] = (uint8_t)(width & 0xFF);
    cfg[GT911_CFG_OFS_X_MAX_H] = (uint8_t)((width >> 8) & 0xFF);
    cfg[GT911_CFG_OFS_Y_MAX_L] = (uint8_t)(height & 0xFF);
    cfg[GT911_CFG_OFS_Y_MAX_H] = (uint8_t)((height >> 8) & 0xFF);

    uint8_t sum = 0;
    for (size_t i = 0; i < sizeof(cfg); i++) {
        sum = (uint8_t)(sum + cfg[i]);
    }
    uint8_t checksum = (uint8_t)((~sum) + 1);

    gt911_write_reg(i2c, GT911_REG_CONFIG_START, cfg, sizeof(cfg));
    uint8_t chk = checksum;
    gt911_write_reg(i2c, GT911_REG_CONFIG_CHKSUM, &chk, 1);
    uint8_t fresh = 0x01;
    gt911_write_reg(i2c, GT911_REG_CONFIG_FRESH, &fresh, 1);
}

static int gt911_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    (void)board;
    const purr_touch_cfg_t *t = (const purr_touch_cfg_t *)dev->cfg;

    if (t->rst_pin != PURR_PIN_NONE) {
        s_bus->gpio_set_output(t->rst_pin, 0);
        s_bus->delay_ms(20);
        s_bus->gpio_set_output(t->rst_pin, 1);
    }
    /* Same real-hardware settle time the compiled driver needed (purr_kernel.c's own
     * comment): ~300-400ms from power-on/reset before the chip answers on I2C. */
    s_bus->delay_ms(350);

    uint32_t i2c = s_bus->i2c_add_device(t->port, t->sda, t->scl, GT911_I2C_ADDR_PRIMARY, (int)t->hz);
    if (i2c == 0) {
        return -1;
    }

    uint8_t pid[4] = {0};
    if (gt911_read_reg(i2c, 0x8140, pid, sizeof(pid)) != 0) {
        s_bus->i2c_remove_device(i2c);
        i2c = s_bus->i2c_add_device(t->port, t->sda, t->scl, GT911_I2C_ADDR_ALT, (int)t->hz);
        if (i2c == 0 || gt911_read_reg(i2c, 0x8140, pid, sizeof(pid)) != 0) {
            return -1;
        }
    }

    gt911_clear_status(i2c);
    gt911_fix_resolution(i2c, t->width, t->height);

    s_state.i2c = i2c;
    s_state.width = t->width;
    s_state.height = t->height;
    s_state.native_x_max = t->native_x_max;
    s_state.native_y_max = t->native_y_max;
    s_state.keepalive_at_ms = 0;

    *out_handle = &s_state;
    return 0;
}

static void gt911_remove(void *handle)
{
    (void)handle;
}

/* GT911's own "send 0x00 to the command register periodically" quirk -- without it the chip
 * stops updating its status/point registers after the first touch. */
static void gt911_keepalive(gt911_state_t *st)
{
    uint32_t now = (uint32_t)(s_bus->now_us() / 1000);
    if (st->keepalive_at_ms != 0 && now < st->keepalive_at_ms) {
        return;
    }
    st->keepalive_at_ms = now + GT911_KEEPALIVE_MS;
    uint8_t zero = 0x00;
    gt911_write_reg(st->i2c, GT911_REG_COMMAND, &zero, 1);
}

/* data[0..3] = x_low, x_high, y_low, y_high (little-endian uint16 pair) on a real touch; 4 is
 * returned. 0 means "nothing to report right now" (no touch, or a release event just
 * consumed) -- not an error. Same signed-interpretation and swap+mirror+rescale math the
 * compiled purr_kernel_touch_read() used to do itself (purr_kernel.c's own comment on why:
 * this unit's native range is not the commonly assumed 0-4095, and the raw bytes must be read
 * signed or an overflow looks like a reset-and-climb instead of a small negative number). */
static int gt911_read(void *handle, uint8_t *data, size_t len)
{
    gt911_state_t *st = (gt911_state_t *)handle;
    if (st == NULL || len < 4) {
        return -1;
    }
    gt911_keepalive(st);

    uint8_t status = 0;
    if (gt911_read_reg(st->i2c, GT911_REG_STATUS, &status, 1) != 0) {
        return 0;
    }
    if (!(status & GT911_STATUS_BUFFER_STAT)) {
        return 0;
    }
    if ((status & GT911_STATUS_TOUCH_MASK) == 0) {
        gt911_clear_status(st->i2c);     /* release event -- must clear or the buffer-ready
                                           * flag latches and the next touch never reports. */
        return 0;
    }

    gt911_point_t pt;
    if (gt911_read_reg(st->i2c, GT911_REG_POINT1, (uint8_t *)&pt, sizeof(pt)) != 0) {
        gt911_clear_status(st->i2c);
        return 0;
    }

    uint16_t raw_x = (uint16_t)pt.x_low | ((uint16_t)pt.x_high << 8);
    uint16_t raw_y = (uint16_t)pt.y_low | ((uint16_t)pt.y_high << 8);
    int16_t signed_x = (int16_t)raw_x;
    int16_t signed_y = (int16_t)raw_y;

    int32_t sx = ((int32_t)signed_y * st->width) / st->native_y_max;
    int32_t sy_raw = ((int32_t)signed_x * st->height) / st->native_x_max;
    int32_t sy = (st->height - 1) - sy_raw;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx >= st->width) sx = st->width - 1;
    if (sy >= st->height) sy = st->height - 1;

    data[0] = (uint8_t)(sx & 0xFF);
    data[1] = (uint8_t)((sx >> 8) & 0xFF);
    data[2] = (uint8_t)(sy & 0xFF);
    data[3] = (uint8_t)((sy >> 8) & 0xFF);
    gt911_clear_status(st->i2c);
    return 4;
}

static const purr_driver_t s_gt911_driver = {
    .name = "gt911", .compatible = "gt911",
    .probe = gt911_probe, .remove = gt911_remove, .read = gt911_read,
};

__attribute__((used))
const purr_driver_t *gt911_entry(const purr_driver_bus_table_t *bus)
{
    s_bus = bus;
    return &s_gt911_driver;
}
