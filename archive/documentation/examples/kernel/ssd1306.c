/*
 * SSD1306 monochrome OLED (128x64 or 128x32) on an I2C bus the kernel already owns, exposed as a
 * native `display` (v2.0). A second, different kind of display for documentation/15. NOT run on
 * hardware. The panel is 1 bit per pixel, so the driver keeps a small framebuffer (1 KB for
 * 128x64) and converts each RGB565 pixel to on/off by brightness. The API has no frame buffer,
 * but a 1 KB private one is fine on any board.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "purr_board.h"
#include "purr_display.h"
#include "purr_kernel.h"

static const char *TAG = "ssd1306";

#define SSD_MAX_W 128
#define SSD_MAX_PAGES 8                      /* 64 rows / 8 */

static struct {
    i2c_master_dev_handle_t dev;
    const purr_oled_cfg_t *cfg;
    uint8_t fb[SSD_MAX_W * SSD_MAX_PAGES];   /* page-major: fb[page * width + x], bit = y & 7 */
    int pages;
} s;

static esp_err_t ssd_cmds(const uint8_t *c, size_t n)
{
    uint8_t buf[8];
    if (n > sizeof(buf) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = 0x00;                           /* control byte: the rest are commands */
    memcpy(buf + 1, c, n);
    return i2c_master_transmit(s.dev, buf, n + 1, 50);
}

/* Sends pages [p0, p1] of the framebuffer. */
static esp_err_t flush_pages(int p0, int p1)
{
    int w = s.cfg->width;
    uint8_t setup[6] = {0x21, 0, (uint8_t)(w - 1), 0x22, (uint8_t)p0, (uint8_t)p1};
    esp_err_t e = ssd_cmds(setup, sizeof(setup));
    uint8_t line[SSD_MAX_W + 1];
    for (int p = p0; p <= p1 && e == ESP_OK; p++) {
        line[0] = 0x40;                      /* control byte: the rest is display data */
        memcpy(line + 1, &s.fb[p * w], (size_t)w);
        e = i2c_master_transmit(s.dev, line, (size_t)w + 1, 50);
    }
    return e;
}

static int is_on(uint16_t c)
{
    /* brightness from RGB565: r (0..31) + g (0..63)/2 + b (0..31), on above about a quarter */
    return (((c >> 11) & 0x1F) + (((c >> 5) & 0x3F) >> 1) + (c & 0x1F)) > 24;
}

static void put_px(int x, int y, int on)
{
    uint8_t *b = &s.fb[(y >> 3) * s.cfg->width + x];
    if (on) {
        *b |= (uint8_t)(1u << (y & 7));
    } else {
        *b &= (uint8_t)~(1u << (y & 7));
    }
}

static esp_err_t d_get_info(purr_display_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->width = s.cfg->width;
    out->height = s.cfg->height;
    out->max_chunk_pixels = (uint16_t)(s.cfg->width * 8);
    strncpy(out->name, s.cfg->name, sizeof(out->name) - 1);
    return ESP_OK;
}

static esp_err_t d_blit(int x, int y, int w, int h, const uint16_t *px)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > s.cfg->width ? s.cfg->width : x + w;
    int y1 = y + h > s.cfg->height ? s.cfg->height : y + h;
    if (x1 <= x0 || y1 <= y0) {
        return ESP_OK;                       /* nothing visible */
    }
    for (int yy = y0; yy < y1; yy++) {
        for (int xx = x0; xx < x1; xx++) {
            put_px(xx, yy, is_on(px[(size_t)(yy - y) * w + (xx - x)]));
        }
    }
    return flush_pages(y0 >> 3, (y1 - 1) >> 3);
}

static esp_err_t d_fill(int x, int y, int w, int h, uint16_t color)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > s.cfg->width ? s.cfg->width : x + w;
    int y1 = y + h > s.cfg->height ? s.cfg->height : y + h;
    if (x1 <= x0 || y1 <= y0) {
        return ESP_OK;
    }
    int on = is_on(color);
    for (int yy = y0; yy < y1; yy++) {
        for (int xx = x0; xx < x1; xx++) {
            put_px(xx, yy, on);
        }
    }
    return flush_pages(y0 >> 3, (y1 - 1) >> 3);
}

static purr_display_v2_t s_ops = {
    .struct_size = sizeof(purr_display_v2_t),
    .major = PURR_DISPLAY_MAJOR,
    .minor = PURR_DISPLAY_MINOR,
    .features = 0,
    .get_info = d_get_info,
    .blit = d_blit,
    .fill = d_fill,
};

esp_err_t purr_ssd1306_init(i2c_master_bus_handle_t bus, const purr_oled_cfg_t *cfg,
                            const purr_display_v2_t **out)
{
    if (bus == NULL || cfg->addr == 0 || cfg->width > SSD_MAX_W || cfg->height > SSD_MAX_PAGES * 8 ||
        cfg->height % 8 != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s, 0, sizeof(s));
    s.cfg = cfg;
    s.pages = cfg->height / 8;

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = cfg->addr,
        .scl_speed_hz = cfg->hz ? cfg->hz : 400000,
    };
    esp_err_t e = i2c_master_bus_add_device(bus, &dc, &s.dev);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "add device: %s", esp_err_to_name(e));
        return e;
    }

    const uint8_t seq[] = {
        0xAE,                                /* display off */
        0xD5, 0x80,                          /* clock divide */
        0xA8, (uint8_t)(cfg->height - 1),    /* multiplex ratio */
        0xD3, 0x00,                          /* display offset */
        0x40,                                /* start line 0 */
        0x8D, 0x14,                          /* charge pump on */
        0x20, 0x00,                          /* horizontal addressing mode */
        0xA1,                                /* segment remap */
        0xC8,                                /* COM scan direction */
        0xDA, (uint8_t)(cfg->height == 64 ? 0x12 : 0x02),
        0x81, 0xCF,                          /* contrast */
        0xD9, 0xF1,                          /* pre-charge */
        0xDB, 0x40,                          /* VCOM detect */
        0xA4,                                /* follow RAM */
        0xA6,                                /* not inverted */
        0xAF,                                /* display on */
    };
    /* Commands that take an argument are sent as a pair, the rest one at a time. */
    for (size_t i = 0; i < sizeof(seq) && e == ESP_OK;) {
        size_t n = 1;
        switch (seq[i]) {
        case 0xD5: case 0xA8: case 0xD3: case 0x8D: case 0x20: case 0xDA: case 0x81:
        case 0xD9: case 0xDB:
            n = 2;
            break;
        default:
            break;
        }
        e = ssd_cmds(&seq[i], n);
        i += n;
    }
    if (e == ESP_OK) {
        e = flush_pages(0, s.pages - 1);     /* start from a blank screen */
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "init: %s", esp_err_to_name(e));
        return e;
    }
    *out = &s_ops;
    ESP_LOGI(TAG, "%s up, %dx%d", cfg->name, cfg->width, cfg->height);
    return ESP_OK;
}
