/* Real loadable display driver -- converted off purr_kernel.c's compiled-in st7789.c
 * (purr_st7789_init/purr_st7789_probe/purr_st7789_remove, deleted there). Unlike keyboard/
 * touch, display's real logic already lived entirely inside the purr_display_v2_t ops table
 * returned by probe() -- nothing had to move OUT of core kernel code, this is a more direct
 * port than those two were.
 *
 * Two things display needed that no other driver so far has: a DMA-capable pixel buffer
 * (freestanding modules have no malloc) and backlight PWM (purr_driver_bus_table_t's
 * dma_alloc()/backlight_init()/backlight_set()). The D/C line is toggled via gpio_write()
 * (level-only, no per-call gpio_config()) instead of gpio_set_output() -- it flips on every
 * SPI chunk during a blit, and gpio_set_output()'s reconfigure-every-call cost would be a
 * real, measurable regression at that frequency. gpio_set_output() still configures it as an
 * output once, in probe(). KNOWN ISSUE (2026-10-15, being investigated): LVGL's first real
 * full-screen draw (as opposed to the boot console's small text blits) renders a stuck blank
 * screen with this approach -- a pre_cb-based fix was tried and reverted after it introduced
 * a separate, worse crash (backlight_init receiving a garbage pin value); rolled back here to
 * this known checkpoint to investigate the pin behavior specifically before trying again.
 *
 * Not wired into real boot's hard requirement by this file alone: main.c's own halt-if-no-
 * display check moved to after purr_modules_setup() (which loads/registers this), since
 * display cannot bind before the filesystem mounts the way the compiled-in version always
 * could -- see main.c's own comment on why.
 */
#include "purr_driver.h"
#include "purr_driver_bus.h"

/* purr_display.h's own real shape (PurrOS/components/kernel/include/purr_display.h),
 * mirrored by hand instead of included -- it pulls in esp_err.h, which isn't on a module
 * build's include path at all (purr_driver.h's own comment on exactly this: "probe()/
 * remove() return plain int, not esp_err_t... using it here would pull esp_err.h into this
 * otherwise ESP-IDF-free header"). esp_err_t is itself just `int`, and 0/nonzero means the
 * same thing either way, so this is binary-identical to the real struct from the caller's
 * side (purr_console.c/purr_lvgl.c), which only ever reads these fields through a
 * `const purr_display_v2_t *`, never this header. */
#define D_MAJOR 2
#define D_MINOR 0
#define D_F_BRIGHTNESS (1u << 0)

typedef struct {
    uint16_t width, height, max_chunk_pixels;
    uint8_t bits_per_pixel;
    char name[24];
} display_info_t;

typedef struct {
    uint16_t struct_size;
    uint8_t major, minor;
    uint32_t features;
    int (*get_info)(display_info_t *out);
    int (*blit)(int x, int y, int w, int h, const uint16_t *pixels);
    int (*fill)(int x, int y, int w, int h, uint16_t color);
    int (*set_brightness)(uint8_t level);
    int (*set_power)(int on);
} display_v2_t;

static const purr_driver_bus_table_t *s_bus;

#define CHUNK_LINES 16

#define CMD_SLPOUT 0x11
#define CMD_NORON  0x13
#define CMD_INVON  0x21
#define CMD_DISPON 0x29
#define CMD_CASET  0x2A
#define CMD_RASET  0x2B
#define CMD_RAMWR  0x2C
#define CMD_MADCTL 0x36

typedef struct {
    uint8_t cmd;
    uint8_t delay_ms;
    uint8_t len;
    uint8_t data[14];
} step_t;

#define MADCTL_STEP_INDEX 3

static const step_t s_init[] = {
    {0x01, 150, 0, {0}},
    {CMD_SLPOUT, 10, 0, {0}},
    {0x3A, 10, 1, {0x55}},
    {CMD_MADCTL, 0, 1, {0}},
    {0xB2, 0, 5, {0x0C, 0x0C, 0x00, 0x33, 0x33}},
    {0xB7, 0, 1, {0x35}},
    {0xBB, 0, 1, {0x28}},
    {0xC0, 0, 1, {0x0C}},
    {0xC2, 0, 1, {0x01}},
    {0xC3, 0, 1, {0x13}},
    {0xC4, 0, 1, {0x20}},
    {0xC6, 0, 1, {0x0F}},
    {0xD0, 0, 2, {0xA4, 0xA1}},
    {0xE0, 0, 14, {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32, 0x44,
                   0x42, 0x06, 0x0E, 0x12, 0x14, 0x17}},
    {0xE1, 0, 14, {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31, 0x54,
                   0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E}},
};

typedef struct {
    uint32_t spi;
    const purr_display_cfg_t *cfg;
    uint8_t *dma;
    uint32_t dma_bytes;
} st7789_state_t;

static st7789_state_t s_state;

static int send_raw(int dc, const uint8_t *buf, size_t len)
{
    if (len == 0) {
        return 0;
    }
    s_bus->gpio_write(s_state.cfg->dc, dc);
    return s_bus->spi_transfer(s_state.spi, buf, NULL, len);
}

static int cmd(uint8_t c)
{
    return send_raw(0, &c, 1);
}

static int cmd_data(uint8_t c, const uint8_t *d, size_t n)
{
    int e = cmd(c);
    return (e == 0 && n) ? send_raw(1, d, n) : e;
}

static int set_window(int x0, int y0, int x1, int y1)
{
    x0 += s_state.cfg->col_off; x1 += s_state.cfg->col_off;
    y0 += s_state.cfg->row_off; y1 += s_state.cfg->row_off;
    uint8_t col[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
    uint8_t row[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
    int e = cmd_data(CMD_CASET, col, 4);
    if (e == 0) e = cmd_data(CMD_RASET, row, 4);
    if (e == 0) e = cmd(CMD_RAMWR);
    return e;
}

/* 1 if there's something visible left to draw (x/y/w/h updated, skip_x/skip_y set), 0 if the
 * rectangle clipped away entirely (caller should treat that as a harmless no-op success). */
static int clip(int *x, int *y, int *w, int *h, int *skip_x, int *skip_y)
{
    int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s_state.cfg->width) x1 = s_state.cfg->width;
    if (y1 > s_state.cfg->height) y1 = s_state.cfg->height;
    if (x1 <= x0 || y1 <= y0) {
        return 0;
    }
    *skip_x = x0 - *x;
    *skip_y = y0 - *y;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return 1;
}

static void copy_name(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    for (; i + 1 < cap && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static int d_get_info(display_info_t *out)
{
    out->width = s_state.cfg->width;
    out->height = s_state.cfg->height;
    out->max_chunk_pixels = (uint16_t)(s_state.dma_bytes / 2);
    out->bits_per_pixel = 16;
    copy_name(out->name, s_state.cfg->name, sizeof(out->name));
    return 0;
}

static int d_blit(int x, int y, int w, int h, const uint16_t *px)
{
    int full_w = w, sx = 0, sy = 0;
    if (!clip(&x, &y, &w, &h, &sx, &sy)) {
        return 0;
    }
    int e = set_window(x, y, x + w - 1, y + h - 1);
    int lines_per_chunk = (int)(s_state.dma_bytes / 2) / w;
    if (lines_per_chunk < 1) lines_per_chunk = 1;
    for (int row = 0; row < h && e == 0; row += lines_per_chunk) {
        int n = h - row < lines_per_chunk ? h - row : lines_per_chunk;
        for (int l = 0; l < n; l++) {
            const uint16_t *src = px + (size_t)(sy + row + l) * full_w + sx;
            uint8_t *dst = s_state.dma + (size_t)l * w * 2;
            for (int i = 0; i < w; i++) {
                dst[2 * i] = (uint8_t)(src[i] >> 8);
                dst[2 * i + 1] = (uint8_t)src[i];
            }
        }
        e = send_raw(1, s_state.dma, (size_t)n * w * 2);
    }
    return e;
}

static int d_fill(int x, int y, int w, int h, uint16_t color)
{
    int sx, sy;
    if (!clip(&x, &y, &w, &h, &sx, &sy)) {
        return 0;
    }
    int e = set_window(x, y, x + w - 1, y + h - 1);
    uint32_t total = (uint32_t)w * h;
    uint32_t per = s_state.dma_bytes / 2;
    for (uint32_t i = 0; i < per; i++) {
        s_state.dma[2 * i] = (uint8_t)(color >> 8);
        s_state.dma[2 * i + 1] = (uint8_t)color;
    }
    while (total && e == 0) {
        uint32_t n = total < per ? total : per;
        e = send_raw(1, s_state.dma, (size_t)n * 2);
        total -= n;
    }
    return e;
}

static int d_set_brightness(uint8_t level)
{
    s_bus->backlight_set(level);
    return 0;
}

static display_v2_t s_ops = {
    .struct_size = sizeof(display_v2_t),
    .major = D_MAJOR,
    .minor = D_MINOR,
    .features = 0,
    .get_info = d_get_info,
    .blit = d_blit,
    .fill = d_fill,
    .set_brightness = NULL,
    .set_power = NULL,
};

static int st7789_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    const purr_display_cfg_t *cfg = (const purr_display_cfg_t *)dev->cfg;

    s_state.cfg = cfg;
    s_state.dma_bytes = (uint32_t)cfg->width * CHUNK_LINES * 2;
    s_state.dma = (uint8_t *)s_bus->dma_alloc(s_state.dma_bytes);
    if (s_state.dma == NULL) {
        return -1;
    }

    s_bus->gpio_set_output(cfg->dc, 0);   /* configured as output once; gpio_write() toggles it */

    uint32_t spi = s_bus->spi_add_device(board->spi.host, cfg->cs, 0, (int)cfg->spi_hz);
    if (spi == 0) {
        return -1;
    }
    s_state.spi = spi;

    if (cfg->rst != PURR_PIN_NONE) {
        s_bus->gpio_set_output(cfg->rst, 0);
        s_bus->delay_ms(20);
        s_bus->gpio_write(cfg->rst, 1);
        s_bus->delay_ms(120);
    }

    int e = 0;
    for (unsigned i = 0; i < sizeof(s_init) / sizeof(s_init[0]) && e == 0; i++) {
        uint8_t data[14];
        for (int k = 0; k < 14; k++) {
            data[k] = s_init[i].data[k];
        }
        if (i == MADCTL_STEP_INDEX) {
            data[0] = cfg->madctl | (cfg->bgr ? 0x08 : 0x00);
        }
        e = cmd_data(s_init[i].cmd, data, s_init[i].len);
        if (s_init[i].delay_ms) {
            s_bus->delay_ms(s_init[i].delay_ms);
        }
    }
    if (e == 0 && cfg->invert) e = cmd(CMD_INVON);
    if (e == 0) e = cmd(CMD_NORON);
    s_bus->delay_ms(10);
    if (e != 0) {
        return -1;
    }

    d_fill(0, 0, cfg->width, cfg->height, 0x0000);
    cmd(CMD_DISPON);
    s_bus->delay_ms(10);

    if (cfg->backlight != PURR_PIN_NONE && s_bus->backlight_init(cfg->backlight) == 0) {
        s_ops.set_brightness = d_set_brightness;
        s_ops.features |= D_F_BRIGHTNESS;
    }

    *out_handle = (void *)&s_ops;
    return 0;
}

static void st7789_remove(void *handle)
{
    (void)handle;
}

static const purr_driver_t s_st7789_driver = {
    .name = "st7789", .compatible = "st7789",
    .probe = st7789_probe, .remove = st7789_remove,
    .bus_abi = PURR_DRIVER_BUS_ABI_VERSION,
};

__attribute__((used))
const purr_driver_t *st7789_entry(const purr_driver_bus_table_t *bus)
{
    s_bus = bus;
    return &s_st7789_driver;
}
