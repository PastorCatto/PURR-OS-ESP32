// epd1in54.c — SSD1681-family 200x200 1.54" e-paper driver (Waveshare
// ESP32-S3-ePaper-1.54, non-touch — the first e-paper device in this
// tree). Command sequence, LUT table and pixel-buffer bit convention
// ported from Waveshare's own official BSP (waveshareteam/ESP32-S3-
// ePaper-1.54, 02_Example/ESP-IDF/V2/09_LVGL_V8_Test/components/
// epaper_driver_bsp/) — an SSD1681 has no meaningfully "default" init
// sequence to derive from a datasheet alone (register-level timing/LUT
// values are panel-specific), so this is a faithful C port of the
// vendor's own C++ BSP, not a reimplementation from scratch.
//
// ── Partial refresh ──────────────────────────────────────────────────
// Every push_pixels()/fill_rect() call after init uses the PARTIAL-
// refresh LUT (~0.3s) instead of the full one (~1-2s) — real speedup for
// epaper_ui's menu cycling, which used to feel sluggish per the exact
// hardware-refresh cost this file's own earlier comment documented.
// Ported from the same vendor BSP as the full-refresh path: EPD_Init()
// (full LUT) -> EPD_DisplayPartBaseImage() (writes the CURRENT buffer to
// BOTH SSD1681 RAM banks via one full-activation cycle, so "previous
// image" and "current image" start identical) -> EPD_Init_Partial()
// (loads the partial LUT + partial border/activation config) — after
// that, EPD_DisplayPart()'s equivalent (epd_partial_refresh() below)
// just writes the new buffer and activates with the partial control
// byte (0xCF instead of 0xC7). No coalescing of rapid successive writes
// into one flush yet — a debounce task is a real, separate follow-up if
// epaper_ui ever fires faster than one redraw per button event.
//
// Partial refresh accumulates visible ghosting over repeated updates
// (a real, documented characteristic of this LUT, not a bug) — this
// driver re-runs the full-refresh baseline every FULL_REFRESH_EVERY
// partial updates to clear it, same practice most e-paper reference
// designs use.
//
// ── Pixel format ──────────────────────────────────────────────────────
// catcall_display_t's push_pixels/fill_rect take RGB565 (uint16_t)
// colors — this panel is strictly 1bpp black/white. Threshold is
// deliberately trivial: exactly 0x0000 (BG_COLOR in every existing text-
// drawing convention in this codebase — purr_fbtty.c, login_render_fb.c,
// boot_splash.c all use it) maps to BLACK, anything else to WHITE. That's
// correct for text (the only content this panel draws this pass) and
// wrong for anything wanting real grayscale/dithered photographic
// content — out of scope until a real use case needs it.
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../../../kernel/core/purr_module.h"
#include "../../../kernel/core/purr_kernel.h"
#include "../../../kernel/catcalls/catcall_display.h"

static const char *TAG = "drv:epd1in54";

// ── Pin defaults (Waveshare ESP32-S3-ePaper-1.54, confirmed against the
// vendor's own user_config.h) — overridable per device.pcat's [pins]
// section via purrstrap's generated CONFIG_DRV_DISPLAY_*_PIN macros, same
// convention st7789.c already established. BUSY/PWR are new macros this
// driver introduces (no prior display driver in this tree needed either)
// — see purrstrap.py's _generate_glue() for where they're generated.
#ifndef CONFIG_DRV_DISPLAY_DC_PIN
#  define CONFIG_DRV_DISPLAY_DC_PIN   10
#endif
#ifndef CONFIG_DRV_DISPLAY_CS_PIN
#  define CONFIG_DRV_DISPLAY_CS_PIN   11
#endif
#ifndef CONFIG_DRV_DISPLAY_SCLK_PIN
#  define CONFIG_DRV_DISPLAY_SCLK_PIN 12
#endif
#ifndef CONFIG_DRV_DISPLAY_MOSI_PIN
#  define CONFIG_DRV_DISPLAY_MOSI_PIN 13
#endif
#ifndef CONFIG_DRV_DISPLAY_RST_PIN
#  define CONFIG_DRV_DISPLAY_RST_PIN  9
#endif
#ifndef CONFIG_DRV_DISPLAY_BUSY_PIN
#  define CONFIG_DRV_DISPLAY_BUSY_PIN 8
#endif
#ifndef CONFIG_DRV_DISPLAY_PWR_PIN
#  define CONFIG_DRV_DISPLAY_PWR_PIN  6
#endif

#define EPD_WIDTH   200
#define EPD_HEIGHT  200
#define EPD_STRIDE  (EPD_WIDTH / 8)                 // 25 bytes/row
#define EPD_BUF_LEN (EPD_STRIDE * EPD_HEIGHT)        // 5000 bytes

static spi_device_handle_t s_spi = NULL;
static uint8_t s_fb[EPD_BUF_LEN];   // 1bpp shadow buffer — bit=1 WHITE, bit=0 BLACK (Waveshare's own convention)
static bool s_partial_mode = false;   // false until epd_drv_init() finishes the one-time transition
static int  s_partial_count = 0;

// How many partial refreshes to do before forcing one full-refresh cycle
// to clear accumulated ghosting — see this file's own top comment.
// Measured on real hardware: ghosting becomes visible around 6-7 partial
// refreshes. Tried lowering this to 5 and separately tried forcing a
// full refresh on every real screen change (both reverted — see this
// file's own git history/commit message for that attempt and why it
// made things worse on real hardware, not better); 10 remains the
// version that measured best overall.
#define FULL_REFRESH_EVERY 10

// Full-refresh LUT — ported byte-for-byte from the vendor BSP's
// WF_Full_1IN54[159]. Opaque timing/voltage-sequence data to this driver;
// see EPD_SetLut() below for how the last 6 bytes get split into
// separate VGH/VSH1/VSH2/VDL/frame-rate register writes.
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

// Partial-refresh LUT — ported byte-for-byte from the vendor BSP's
// WF_PARTIAL_1IN54_0[159]. Same opaque-data, same epd_set_lut() split.
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

// ── Low-level SPI/GPIO helpers ───────────────────────────────────────────

static inline void dc_data(void)    { gpio_set_level(CONFIG_DRV_DISPLAY_DC_PIN, 1); }
static inline void dc_cmd(void)     { gpio_set_level(CONFIG_DRV_DISPLAY_DC_PIN, 0); }
static inline void cs_select(void)  { gpio_set_level(CONFIG_DRV_DISPLAY_CS_PIN, 0); }
static inline void cs_deselect(void){ gpio_set_level(CONFIG_DRV_DISPLAY_CS_PIN, 1); }

static void spi_send(const uint8_t *buf, size_t len)
{
    if (!len) return;
    spi_transaction_t t = { .length = 8 * len, .tx_buffer = buf };
    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    if (err != ESP_OK) ESP_LOGE(TAG, "spi_device_polling_transmit failed: %s", esp_err_to_name(err));
}

static void epd_cmd(uint8_t c)
{
    dc_cmd(); cs_select();
    spi_send(&c, 1);
    cs_deselect();
}

static void epd_data(uint8_t d)
{
    dc_data(); cs_select();
    spi_send(&d, 1);
    cs_deselect();
}

static void epd_data_buf(const uint8_t *buf, size_t len)
{
    dc_data(); cs_select();
    spi_send(buf, len);
    cs_deselect();
}

// BUSY is active HIGH on this panel (per vendor BSP's own read_busy():
// "LOW: idle, HIGH: busy") — confirmed, not assumed. 5s ceiling rather
// than an unbounded spin: a stuck-HIGH BUSY line (bad wiring, dead panel)
// should fail a refresh loudly instead of hanging this driver's caller
// forever.
static bool epd_wait_busy(void)
{
    int waited_ms = 0;
    while (gpio_get_level(CONFIG_DRV_DISPLAY_BUSY_PIN) == 1) {
        vTaskDelay(pdMS_TO_TICKS(5));
        waited_ms += 5;
        if (waited_ms > 5000) {
            ESP_LOGE(TAG, "BUSY stuck high for 5s — aborting wait");
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
    gpio_set_level(CONFIG_DRV_DISPLAY_RST_PIN, 1); vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(CONFIG_DRV_DISPLAY_RST_PIN, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(CONFIG_DRV_DISPLAY_RST_PIN, 1); vTaskDelay(pdMS_TO_TICKS(50));
}

// Full init sequence, byte-for-byte from the vendor BSP's EPD_Init() —
// driver output control (MUX=199), data entry mode, RAM window/cursor set
// to the full 200x200 panel, border waveform, temperature-sensor-driven
// waveform load, then the full-refresh LUT above.
static void epd_init_sequence(void)
{
    epd_hw_reset();
    epd_wait_busy();

    epd_cmd(0x12);   // SWRESET
    epd_wait_busy();

    epd_cmd(0x01);   // driver output control
    epd_data(0xC7); epd_data(0x00); epd_data(0x01);

    epd_cmd(0x11);   // data entry mode
    epd_data(0x01);

    // SET_RAM_X/Y window — full panel, 0..199 both axes.
    epd_cmd(0x44); epd_data(0x00); epd_data((EPD_WIDTH - 1) >> 3);
    epd_cmd(0x45);
    epd_data((EPD_HEIGHT - 1) & 0xFF); epd_data(((EPD_HEIGHT - 1) >> 8) & 0xFF);
    epd_data(0x00); epd_data(0x00);

    epd_cmd(0x3C);   // border waveform
    epd_data(0x01);

    epd_cmd(0x18);   // internal temperature sensor
    epd_data(0x80);

    epd_cmd(0x22);   // load temperature + waveform setting
    epd_data(0xB1);
    epd_cmd(0x20);

    // SET_RAM_X/Y counter — cursor to (0, 199), matching the window above.
    epd_cmd(0x4E); epd_data(0x00);
    epd_cmd(0x4F); epd_data((EPD_HEIGHT - 1) & 0xFF); epd_data(((EPD_HEIGHT - 1) >> 8) & 0xFF);
    epd_wait_busy();

    epd_set_lut(WF_FULL_1IN54);
}

static void epd_full_refresh(void)
{
    epd_cmd(0x24);   // write RAM (B/W)
    epd_data_buf(s_fb, EPD_BUF_LEN);

    epd_cmd(0x22); epd_data(0xC7);   // display update control
    epd_cmd(0x20);                   // master activation
    epd_wait_busy();
}

// EPD_DisplayPartBaseImage() equivalent — writes the CURRENT buffer to
// BOTH the "current" (0x24) and "previous" (0x26) SSD1681 RAM banks and
// does one FULL activation. Establishes the clean, ghost-free baseline
// partial refresh compares every future update against; called once at
// the partial-mode transition and again every FULL_REFRESH_EVERY partial
// updates to clear accumulated ghosting.
static void epd_write_baseline_full(void)
{
    epd_cmd(0x24); epd_data_buf(s_fb, EPD_BUF_LEN);
    epd_cmd(0x26); epd_data_buf(s_fb, EPD_BUF_LEN);

    epd_cmd(0x22); epd_data(0xC7);
    epd_cmd(0x20);
    epd_wait_busy();
}

// EPD_Init_Partial() equivalent — loads the partial LUT plus this
// panel's own partial-mode border/activation config (the 0x37 payload
// and 0x3C/0x80 border byte are opaque, panel-specific bytes ported
// verbatim from the vendor BSP, same as every other command sequence in
// this file).
static void epd_enter_partial_mode(void)
{
    epd_hw_reset();
    epd_wait_busy();

    epd_set_lut(WF_PARTIAL_1IN54);

    epd_cmd(0x37);
    static const uint8_t cfg37[10] = {0x00,0x00,0x00,0x00,0x00,0x40,0x00,0x00,0x00,0x00};
    epd_data_buf(cfg37, sizeof(cfg37));

    epd_cmd(0x3C); epd_data(0x80);   // border waveform (partial)

    epd_cmd(0x22); epd_data(0xC0);
    epd_cmd(0x20);                   // master activation
    epd_wait_busy();
}

// EPD_DisplayPart() equivalent — the fast path: write the new buffer,
// activate with the PARTIAL control byte (0xCF, not 0xC7). No LUT
// reload, no hardware reset — that's what makes this the fast one.
static void epd_partial_refresh(void)
{
    epd_cmd(0x24); epd_data_buf(s_fb, EPD_BUF_LEN);

    epd_cmd(0x22); epd_data(0xCF);
    epd_cmd(0x20);
    epd_wait_busy();
}

// The one function push_pixels()/fill_rect() actually call. Dispatches
// to a full refresh before partial mode is armed (early boot, and this
// driver's own module_init() call before purr_kernel_register_display()
// runs), otherwise does a fast partial refresh — except every
// FULL_REFRESH_EVERY-th call, which re-establishes the ghost-free
// baseline first (see epd_write_baseline_full()'s own comment).
static void epd_refresh(void)
{
    if (!s_partial_mode) {
        epd_full_refresh();
        return;
    }
    s_partial_count++;
    if (s_partial_count >= FULL_REFRESH_EVERY) {
        epd_write_baseline_full();
        epd_enter_partial_mode();
        s_partial_count = 0;
    } else {
        epd_partial_refresh();
    }
}

// ── Pixel buffer ──────────────────────────────────────────────────────

static inline void epd_set_pixel(int x, int y, bool white)
{
    if ((unsigned)x >= EPD_WIDTH || (unsigned)y >= EPD_HEIGHT) return;
    uint8_t *byte = &s_fb[y * EPD_STRIDE + (x >> 3)];
    uint8_t bit = 1 << (7 - (x & 7));
    if (white) *byte |= bit; else *byte &= (uint8_t)~bit;
}

// ── catcall_display_t ────────────────────────────────────────────────────

static esp_err_t epd_drv_init(const display_config_t *cfg)
{
    (void)cfg;

    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_DRV_DISPLAY_DC_PIN) | (1ULL << CONFIG_DRV_DISPLAY_CS_PIN) |
                        (1ULL << CONFIG_DRV_DISPLAY_RST_PIN) | (1ULL << CONFIG_DRV_DISPLAY_PWR_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);

    gpio_config_t busy_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_DRV_DISPLAY_BUSY_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&busy_cfg);

    // EPD_PWR_PIN is active LOW (confirmed against the vendor's own
    // board_power_bsp.cpp: POWEER_EPD_ON() drives it low) — must go low
    // before anything below talks to the panel, or every SPI transaction
    // goes out against an unpowered rail with no error surfaced, same
    // class of silent failure heltec's own Vext comment documents.
    gpio_set_level(CONFIG_DRV_DISPLAY_PWR_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = CONFIG_DRV_DISPLAY_MOSI_PIN,
        .miso_io_num     = -1,
        .sclk_io_num     = CONFIG_DRV_DISPLAY_SCLK_PIN,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = EPD_BUF_LEN,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 40 * 1000 * 1000,   // confirmed against vendor BSP (40MHz, mode 0)
        .mode           = 0,
        .spics_io_num   = -1,                  // CS driven by hand — see cs_select()/cs_deselect()
        .queue_size     = 7,
    };
    err = spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    epd_init_sequence();

    memset(s_fb, 0xFF, EPD_BUF_LEN);   // all-white known state
    epd_write_baseline_full();         // one full refresh — also seeds BOTH RAM banks for partial mode below
    epd_enter_partial_mode();
    s_partial_mode = true;
    s_partial_count = 0;

    ESP_LOGI(TAG, "EPD1in54 ready %dx%d (DC=%d CS=%d SCK=%d MOSI=%d RST=%d BUSY=%d PWR=%d)",
             EPD_WIDTH, EPD_HEIGHT,
             CONFIG_DRV_DISPLAY_DC_PIN, CONFIG_DRV_DISPLAY_CS_PIN, CONFIG_DRV_DISPLAY_SCLK_PIN,
             CONFIG_DRV_DISPLAY_MOSI_PIN, CONFIG_DRV_DISPLAY_RST_PIN, CONFIG_DRV_DISPLAY_BUSY_PIN,
             CONFIG_DRV_DISPLAY_PWR_PIN);
    return ESP_OK;
}

static esp_err_t epd_push_pixels(int x, int y, int w, int h, const uint16_t *data)
{
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            bool white = data[row * w + col] != 0x0000;   // see this file's own top comment on the threshold
            epd_set_pixel(x + col, y + row, white);
        }
    }
    epd_refresh();
    return ESP_OK;
}

static esp_err_t epd_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    bool white = color != 0x0000;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            epd_set_pixel(x + col, y + row, white);
        }
    }
    epd_refresh();
    return ESP_OK;
}

static esp_err_t epd_set_brightness(uint8_t level)
{
    (void)level;
    return ESP_ERR_NOT_SUPPORTED;   // no backlight on an e-paper panel
}

static void epd_get_info(display_info_t *out)
{
    out->width          = EPD_WIDTH;
    out->height         = EPD_HEIGHT;
    out->bits_per_pixel = 1;
    snprintf(out->name, sizeof(out->name), "epd1in54");
}

static esp_err_t epd_deinit(void)
{
    if (s_spi) { spi_bus_remove_device(s_spi); s_spi = NULL; }
    spi_bus_free(SPI2_HOST);
    gpio_set_level(CONFIG_DRV_DISPLAY_PWR_PIN, 1);   // power rail off
    return ESP_OK;
}

static const catcall_display_t s_catcall = {
    .name            = "epd1in54",
    .catcall_version = CATCALL_DISPLAY_VERSION,
    .init            = epd_drv_init,
    .push_pixels     = epd_push_pixels,
    .fill_rect       = epd_fill_rect,
    .set_brightness  = epd_set_brightness,
    .get_info        = epd_get_info,
    .deinit          = epd_deinit,
    // No async push — this panel never redraws often enough to need
    // double-buffered DMA overlap (see catcall_display.h's own comment on
    // why that exists at all); every refresh already blocks for ~1-2s on
    // epd_wait_busy() regardless of how the SPI write itself is issued.
    .push_pixels_async = NULL,
    .flush_done_cb     = NULL,
};

// ── Module lifecycle ─────────────────────────────────────────────────────

static int module_init(void)
{
    esp_err_t ret = epd_drv_init(NULL);
    if (ret != ESP_OK) return -1;
    purr_kernel_register_display(&s_catcall);
    return 0;
}

static void module_deinit(void)
{
    epd_deinit();
}

PURR_MODULE_REGISTER(epd1in54) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .load_priority     = PURR_PRIORITY_REQUIRED,
    .name              = "epd1in54",
    .version           = "0.1.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = CATCALL_FLAG_DISPLAY,
    .required_catcalls = 0,
    .init              = module_init,
    .deinit            = module_deinit,
};
