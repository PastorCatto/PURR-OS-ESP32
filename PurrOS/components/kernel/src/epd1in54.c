/*
 * epd1in54.c - SSD1681-family 200x200 1.54" e-paper driver (Waveshare ESP32-S3-ePaper-1.54).
 *
 * Command sequence, LUT tables and pixel-buffer convention ported from the archive's own
 * driver (archive/DP9/code/source/drivers/display/epd1in54/epd1in54.c), itself a faithful C
 * port of Waveshare's official BSP (waveshareteam/ESP32-S3-ePaper-1.54) -- an SSD1681 has no
 * meaningfully "default" init sequence to derive from a datasheet alone, so this is carried
 * over byte-for-byte, not re-derived. What changed getting here: GPIO-bit-banged CS/DC and a
 * from-scratch SPI bus bring-up became an spics_io_num-managed CS plus a pre_cb DC toggle,
 * the same pattern st7789.c already uses -- the bus itself is brought up once, centrally, by
 * purr_kernel_init() from the board profile, not by this driver; and this now speaks
 * purr_display_v2_t/purr_driver_t (probe/remove) instead of the archive's own catcall_display_t/
 * PURR_MODULE_REGISTER. The hardware protocol itself -- timing, LUTs, BUSY/PWR polarity,
 * partial-refresh ghosting mitigation -- is unchanged.
 *
 * -- Partial refresh --
 * Every blit()/fill() after the one-time transition uses the PARTIAL-refresh LUT (~0.3s)
 * instead of the full one (~1-2s). Partial refresh accumulates visible ghosting over repeated
 * updates (a real, documented characteristic of this LUT, not a bug), so a full-refresh
 * baseline is re-established every FULL_REFRESH_EVERY partial updates.
 *
 * -- Pixel format --
 * purr_display_v2_t's blit()/fill() still take RGB565 (uint16_t) -- this panel is strictly
 * 1bpp black/white, so the threshold is the same one every other text-drawing convention in
 * this codebase already uses: exactly 0x0000 maps to BLACK, anything else to WHITE --
 * inverted (EPD_INVERT below) if the caller wants the opposite.
 * purr_display_info_t.bits_per_pixel (purr_display.h) reports 1, for a caller that wants to
 * know not to expect real color.
 *
 * -- Invert --
 * EPD_INVERT flips that threshold panel-wide: with it set, a mostly-black UI (console text on
 * a black background, PurrOS/PurrOS main.c's convention) actually drives the panel as mostly
 * white-with-black-text instead. Requested 2026-10-03: a console that's black-background-
 * most-of-the-screen means most pixels flip on every refresh, which costs more time/ghosting
 * on this panel than a white-background UI where only the text pixels flip. This is the one
 * place that decision belongs -- every blit()/fill() caller above this file keeps using the
 * same 0x0000=black/else=white colors it always did; only the panel's own idea of which way
 * to drive the particles changes.
 */
#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "purr_board.h"
#include "purr_display.h"
#include "purr_driver.h"

static const char *TAG = "epd1in54";

#define EPD_WIDTH   200
#define EPD_HEIGHT  200
#define EPD_STRIDE  (EPD_WIDTH / 8)                 /* 25 bytes/row */
#define EPD_BUF_LEN (EPD_STRIDE * EPD_HEIGHT)        /* 5000 bytes */

/* How many partial refreshes before forcing one full-refresh cycle to clear accumulated
 * ghosting -- measured on real hardware in the archive driver; carried over unchanged. */
#define FULL_REFRESH_EVERY 10

/* Full-refresh LUT -- ported byte-for-byte from the vendor BSP's WF_Full_1IN54[159]. Opaque
 * timing/voltage-sequence data; see epd_set_lut() for how the last 6 bytes split into
 * separate register writes. */
static const uint8_t WF_FULL_1IN54[159] = {
    0x80,0x48,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x40,0x48,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x80,0x48,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x40,0x48,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0A,0x00,0x00,0x00,0x00,0x00,0x00,
    0x08,0x01,0x00,0x08,0x01,0x00,0x02,
    0x0A,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x22,0x22,0x22,0x22,0x22,0x22,0x00,0x00,0x00,
    0x22,0x17,0x41,0x00,0x32,0x20,
};

/* Partial-refresh LUT -- ported byte-for-byte from the vendor BSP's WF_PARTIAL_1IN54_0[159]. */
static const uint8_t WF_PARTIAL_1IN54[159] = {
    0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x80,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x40,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0F,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x01,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x22,0x22,0x22,0x22,0x22,0x22,0x00,0x00,0x00,
    0x02,0x17,0x41,0xB0,0x32,0x28,
};

static struct {
    spi_device_handle_t dev;
    const purr_epd_cfg_t *cfg;
    uint8_t fb[EPD_BUF_LEN];      /* 1bpp shadow buffer -- bit=1 WHITE, bit=0 BLACK */
    bool partial_mode;
    int partial_count;
} s;

/* ---------------------------------------------------------------- SPI/GPIO helpers */

static void IRAM_ATTR pre_cb(spi_transaction_t *t)
{
    gpio_set_level((gpio_num_t)s.cfg->dc, (int)(intptr_t)t->user);
}

/* Found live on real hardware: the full framebuffer (EPD_BUF_LEN, 5000 bytes) sent in one
 * spi_device_transmit() exceeded the bus's transfer limit ("txdata transfer > host maximum")
 * -- purr_kernel_init()'s bus config sizes max_transfer_sz from board->display, which this
 * board doesn't use (it has no ST7789), so it defaults to 0 and ESP-IDF falls back to 4092.
 * Chunking here is the same discipline st7789.c's own blit() already uses, and it's correct
 * regardless of whatever max_transfer_sz actually ends up being, so it's the fix here rather
 * than trying to get that number exactly right for this board too. */
#define SPI_CHUNK_MAX 1024

static esp_err_t spi_send(const void *buf, size_t len, int dc)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        size_t n = len > SPI_CHUNK_MAX ? SPI_CHUNK_MAX : len;
        spi_transaction_t t = {.length = n * 8, .tx_buffer = p, .user = (void *)(intptr_t)dc};
        esp_err_t e = spi_device_transmit(s.dev, &t);
        if (e != ESP_OK) {
            return e;
        }
        p += n;
        len -= n;
    }
    return ESP_OK;
}

static esp_err_t epd_cmd(uint8_t c)
{
    return spi_send(&c, 1, 0);
}

static esp_err_t epd_data(uint8_t d)
{
    return spi_send(&d, 1, 1);
}

static esp_err_t epd_data_buf(const uint8_t *buf, size_t len)
{
    return spi_send(buf, len, 1);
}

/* BUSY is active HIGH on this panel (confirmed against the vendor BSP's own read_busy()).
 * A 5s ceiling, not an unbounded spin: a stuck-HIGH BUSY line should fail a refresh loudly
 * instead of hanging the caller forever. */
static bool epd_wait_busy(void)
{
    int waited_ms = 0;
    while (gpio_get_level((gpio_num_t)s.cfg->busy) == 1) {
        vTaskDelay(pdMS_TO_TICKS(5));
        waited_ms += 5;
        if (waited_ms > 5000) {
            ESP_LOGE(TAG, "BUSY stuck high for 5s -- aborting wait");
            return false;
        }
    }
    return true;
}

static void epd_set_lut(const uint8_t *lut)
{
    epd_cmd(0x32);
    epd_data_buf(lut, 153);
    epd_wait_busy();

    epd_cmd(0x3F); epd_data(lut[153]);
    epd_cmd(0x03); epd_data(lut[154]);
    epd_cmd(0x04);
    epd_data(lut[155]); epd_data(lut[156]); epd_data(lut[157]);
    epd_cmd(0x2C); epd_data(lut[158]);
}

static void epd_hw_reset(void)
{
    gpio_set_level((gpio_num_t)s.cfg->rst, 1); vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level((gpio_num_t)s.cfg->rst, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)s.cfg->rst, 1); vTaskDelay(pdMS_TO_TICKS(50));
}

/* Full init sequence, byte-for-byte from the vendor BSP's EPD_Init(): driver output control
 * (MUX=199), data entry mode, RAM window/cursor set to the full 200x200 panel, border
 * waveform, temperature-sensor-driven waveform load, then the full-refresh LUT. */
static void epd_init_sequence(void)
{
    epd_hw_reset();
    epd_wait_busy();

    epd_cmd(0x12);   /* SWRESET */
    epd_wait_busy();

    epd_cmd(0x01);   /* driver output control */
    epd_data(0xC7); epd_data(0x00); epd_data(0x01);

    epd_cmd(0x11);   /* data entry mode */
    epd_data(0x01);

    epd_cmd(0x44); epd_data(0x00); epd_data((EPD_WIDTH - 1) >> 3);
    epd_cmd(0x45);
    epd_data((EPD_HEIGHT - 1) & 0xFF); epd_data(((EPD_HEIGHT - 1) >> 8) & 0xFF);
    epd_data(0x00); epd_data(0x00);

    epd_cmd(0x3C);   /* border waveform */
    epd_data(0x01);

    epd_cmd(0x18);   /* internal temperature sensor */
    epd_data(0x80);

    epd_cmd(0x22);   /* load temperature + waveform setting */
    epd_data(0xB1);
    epd_cmd(0x20);

    epd_cmd(0x4E); epd_data(0x00);
    epd_cmd(0x4F); epd_data((EPD_HEIGHT - 1) & 0xFF); epd_data(((EPD_HEIGHT - 1) >> 8) & 0xFF);
    epd_wait_busy();

    epd_set_lut(WF_FULL_1IN54);
}

static void epd_full_refresh(void)
{
    epd_cmd(0x24); epd_data_buf(s.fb, EPD_BUF_LEN);
    epd_cmd(0x22); epd_data(0xC7);
    epd_cmd(0x20);
    epd_wait_busy();
}

/* Writes the CURRENT buffer to BOTH the "current" (0x24) and "previous" (0x26) SSD1681 RAM
 * banks and does one FULL activation -- the clean, ghost-free baseline partial refresh
 * compares every future update against. */
static void epd_write_baseline_full(void)
{
    epd_cmd(0x24); epd_data_buf(s.fb, EPD_BUF_LEN);
    epd_cmd(0x26); epd_data_buf(s.fb, EPD_BUF_LEN);
    epd_cmd(0x22); epd_data(0xC7);
    epd_cmd(0x20);
    epd_wait_busy();
}

/* Loads the partial LUT plus this panel's own partial-mode border/activation config -- the
 * 0x37 payload and 0x3C/0x80 border byte are opaque, panel-specific bytes. */
static void epd_enter_partial_mode(void)
{
    epd_hw_reset();
    epd_wait_busy();

    epd_set_lut(WF_PARTIAL_1IN54);

    epd_cmd(0x37);
    static const uint8_t cfg37[10] = {0x00,0x00,0x00,0x00,0x00,0x40,0x00,0x00,0x00,0x00};
    epd_data_buf(cfg37, sizeof(cfg37));

    epd_cmd(0x3C); epd_data(0x80);
    epd_cmd(0x22); epd_data(0xC0);
    epd_cmd(0x20);
    epd_wait_busy();
}

static void epd_partial_refresh(void)
{
    epd_cmd(0x24); epd_data_buf(s.fb, EPD_BUF_LEN);
    epd_cmd(0x22); epd_data(0xCF);
    epd_cmd(0x20);
    epd_wait_busy();
}

static void epd_refresh(void)
{
    if (!s.partial_mode) {
        epd_full_refresh();
        return;
    }
    s.partial_count++;
    if (s.partial_count >= FULL_REFRESH_EVERY) {
        epd_write_baseline_full();
        epd_enter_partial_mode();
        s.partial_count = 0;
    } else {
        epd_partial_refresh();
    }
}

/* ---------------------------------------------------------------- pixel buffer */

#define EPD_INVERT 1

static inline void epd_set_pixel(int x, int y, bool white)
{
    if ((unsigned)x >= EPD_WIDTH || (unsigned)y >= EPD_HEIGHT) {
        return;
    }
    uint8_t *byte = &s.fb[y * EPD_STRIDE + (x >> 3)];
    uint8_t bit = (uint8_t)(1 << (7 - (x & 7)));
    if (white) {
        *byte |= bit;
    } else {
        *byte &= (uint8_t)~bit;
    }
}

/* ------------------------------------------------------------ purr_display_v2_t */

static esp_err_t d_get_info(purr_display_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->width = EPD_WIDTH;
    out->height = EPD_HEIGHT;
    out->max_chunk_pixels = EPD_WIDTH * EPD_HEIGHT;   /* the whole panel: one shadow buffer */
    out->bits_per_pixel = 1;
    strncpy(out->name, "epd1in54", sizeof(out->name) - 1);
    return ESP_OK;
}

static esp_err_t d_blit(int x, int y, int w, int h, const uint16_t *px)
{
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            /* see this file's own top comment ("Pixel format" / "Invert") */
            bool white = (px[row * w + col] != 0x0000) ^ EPD_INVERT;
            epd_set_pixel(x + col, y + row, white);
        }
    }
    epd_refresh();
    return ESP_OK;
}

static esp_err_t d_fill(int x, int y, int w, int h, uint16_t color)
{
    bool white = (color != 0x0000) ^ EPD_INVERT;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            epd_set_pixel(x + col, y + row, white);
        }
    }
    epd_refresh();
    return ESP_OK;
}

static purr_display_v2_t s_ops = {
    .struct_size = sizeof(purr_display_v2_t),
    .major = PURR_DISPLAY_MAJOR,
    .minor = PURR_DISPLAY_MINOR,
    .features = 0,                /* no backlight, no sleep-power path this pass */
    .get_info = d_get_info,
    .blit = d_blit,
    .fill = d_fill,
    .set_brightness = NULL,
    .set_power = NULL,
};

/* ---------------------------------------------------------------- driver registry (F-02) */

int purr_epd1in54_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    const purr_epd_cfg_t *cfg = (const purr_epd_cfg_t *)dev->cfg;
    memset(&s, 0, sizeof(s));
    s.cfg = cfg;

    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << cfg->dc) | (1ULL << cfg->rst),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out_cfg);
    gpio_config_t busy_cfg = {
        .pin_bit_mask = (1ULL << cfg->busy),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&busy_cfg);

    /* EPD_PWR_PIN is active LOW (confirmed against the vendor's own board_power_bsp.cpp) --
     * must go low before anything below talks to the panel, or every SPI transaction goes
     * out against an unpowered rail with no error surfaced. */
    if (cfg->pwr != PURR_PIN_NONE) {
        gpio_config_t pwr_cfg = {.pin_bit_mask = (1ULL << cfg->pwr), .mode = GPIO_MODE_OUTPUT};
        gpio_config(&pwr_cfg);
        gpio_set_level((gpio_num_t)cfg->pwr, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = (int)cfg->spi_hz,
        .mode = 0,
        .spics_io_num = cfg->cs,
        .queue_size = 2,
        .pre_cb = pre_cb,
    };
    esp_err_t e = spi_bus_add_device((spi_host_device_t)board->spi.host, &dev_cfg, &s.dev);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "add device: %s", esp_err_to_name(e));
        return (int)e;
    }

    epd_init_sequence();
    memset(s.fb, 0xFF, EPD_BUF_LEN);     /* all-white known state */
    epd_write_baseline_full();           /* also seeds both RAM banks for partial mode */
    epd_enter_partial_mode();
    s.partial_mode = true;
    s.partial_count = 0;

    *out_handle = (void *)&s_ops;
    ESP_LOGI(TAG, "epd1in54 up %dx%d (DC=%d CS=%d RST=%d BUSY=%d PWR=%d)",
             EPD_WIDTH, EPD_HEIGHT, cfg->dc, cfg->cs, cfg->rst, cfg->busy, cfg->pwr);
    return 0;
}

void purr_epd1in54_remove(void *handle)
{
    (void)handle;
    if (s.dev) {
        spi_bus_remove_device(s.dev);
        s.dev = NULL;
    }
    if (s.cfg && s.cfg->pwr != PURR_PIN_NONE) {
        gpio_set_level((gpio_num_t)s.cfg->pwr, 1);   /* power rail off */
    }
}
