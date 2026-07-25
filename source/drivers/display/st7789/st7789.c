// st7789.c — ST7789 SPI display driver for PURR OS
// Catcall-compatible, ESP-IDF v5.x, pure C, synchronous SPI.
//
// Target hardware: LilyGO T-Deck (and variants) — ST7789 320×240 landscape
//   Default pins: MOSI=41 SCLK=40 CS=12 DC=11 RST=-1 BL=42
//   Native orientation 240×320 portrait; MADCTL=0x70 gives 320×240 landscape.
//
// For 240×280 panels (e.g. Waveshare 1.69"): pass flags=1 in display_config_t.
//   The driver sets height=280 and adjusts the row offset accordingly.
//
// Call st7789_configure() with your pin assignments, then register the
// purr_module. The kernel calls purr_module.init → st7789_drv_init().

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"   // Step 0 instrumentation — see DP8_CHECKLIST.md
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/catcall_display.h"

// ── Default pin assignments (T-Deck) ─────────────────────────────────────────
#ifndef CONFIG_DRV_DISPLAY_CS_PIN
#  define CONFIG_DRV_DISPLAY_CS_PIN   12
#endif
#ifndef CONFIG_DRV_DISPLAY_DC_PIN
#  define CONFIG_DRV_DISPLAY_DC_PIN   11
#endif
#ifndef CONFIG_DRV_DISPLAY_MOSI_PIN
#  define CONFIG_DRV_DISPLAY_MOSI_PIN 41
#endif
#ifndef CONFIG_DRV_DISPLAY_SCLK_PIN
#  define CONFIG_DRV_DISPLAY_SCLK_PIN 40
#endif
#ifndef CONFIG_DRV_DISPLAY_RST_PIN
#  define CONFIG_DRV_DISPLAY_RST_PIN  (-1)
#endif
#ifndef CONFIG_DRV_DISPLAY_BL_PIN
#  define CONFIG_DRV_DISPLAY_BL_PIN   42
#endif

// ST7789 landscape resolution (MADCTL-rotated)
#define ST7789_WIDTH    320
#define ST7789_HEIGHT   240

// Waveshare 1.69" / 240×280 variant (selected by flags bit 0 in display_config_t)
#define ST7789_FLAG_WS169  (1u << 0)
#define ST7789_WS169_W     240
#define ST7789_WS169_H     280

// SPI host and clock — overridable at runtime via st7789_set_spi_host(),
// default preserved for every device that doesn't call it.
#define ST7789_SPI_HOST  SPI2_HOST
static spi_host_device_t s_spi_host = ST7789_SPI_HOST;
#define ST7789_CLK_HZ    (80 * 1000 * 1000)
static uint32_t s_spi_freq_hz = ST7789_CLK_HZ;

// LEDC backlight
#define BL_SPEED_MODE  LEDC_LOW_SPEED_MODE
#define BL_TIMER_NUM   LEDC_TIMER_2
#define BL_CHANNEL     LEDC_CHANNEL_2
#define BL_FREQ_HZ     5000
#define BL_RESOLUTION  LEDC_TIMER_8_BIT

// ST7789 command codes
#define ST_SWRESET    0x01
#define ST_SLPOUT     0x11
#define ST_COLMOD     0x3A
#define ST_MADCTL     0x36
#define ST_PORCTRL    0xB2
#define ST_GCTRL      0xB7
#define ST_VCOMS      0xBB
#define ST_LCMCTRL    0xC0
#define ST_VDVVRHEN   0xC2
#define ST_VRHS       0xC3
#define ST_VDVS       0xC4
#define ST_FRCTR2     0xC6
#define ST_PWCTRL1    0xD0
#define ST_PVGAMCTRL  0xE0
#define ST_NVGAMCTRL  0xE1
#define ST_CASET      0x2A
#define ST_RASET      0x2B
#define ST_RAMWR      0x2C
#define ST_DISPON     0x29
#define ST_NORON      0x13
#define ST_INVON      0x21

// MADCTL: 0x70 = MX|MY|MV (swap XY then mirror both) → 320×240 landscape, RGB
#define MADCTL_LANDSCAPE  0x70
// For portrait native 240×280 (WS169)
#define MADCTL_PORTRAIT   0x00

static const char *TAG = "drv:st7789";

// ── Driver state ──────────────────────────────────────────────────────────────

typedef struct {
    int cs_pin;
    int dc_pin;
    int mosi_pin;
    int miso_pin;
    int sclk_pin;
    int rst_pin;
    int bl_pin;
} st7789_pins_t;

static st7789_pins_t s_pins = {
    .cs_pin   = CONFIG_DRV_DISPLAY_CS_PIN,
    .dc_pin   = CONFIG_DRV_DISPLAY_DC_PIN,
    .mosi_pin = CONFIG_DRV_DISPLAY_MOSI_PIN,
    .miso_pin = -1,
    .sclk_pin = CONFIG_DRV_DISPLAY_SCLK_PIN,
    .rst_pin  = CONFIG_DRV_DISPLAY_RST_PIN,
    .bl_pin   = CONFIG_DRV_DISPLAY_BL_PIN,
};

static spi_device_handle_t s_spi      = NULL;
static bool                s_ready    = false;
static uint16_t            s_width    = ST7789_WIDTH;
static uint16_t            s_height   = ST7789_HEIGHT;
// Column / row offset for panels that don't start at 0,0 in GRAM
static uint16_t            s_col_off  = 0;
static uint16_t            s_row_off  = 0;

// Row buffer — 320 px × 2 bytes
static uint16_t s_row_buf[ST7789_WIDTH];

// ── Perf mode: optional PSRAM bulk-transfer buffer ──────────────────────
// See st7789_set_perf_mode()'s doc comment in st7789.h. NULL unless/until
// enabled; push_pixels() falls back to the row-by-row s_row_buf path
// whenever this is NULL or the dirty rect doesn't fit, so every existing
// device (and any device without PSRAM) is completely unaffected.
static uint16_t *s_bulk_buf    = NULL;
static size_t    s_bulk_buf_px = 0;   // capacity, in pixels

// Bulk transfer chunk size, in BYTES. The bulk buffer is exactly this big.
//
// Two independent limits forced this design, both found on hardware:
//
// 1. HARD LIMIT — the SPI transfer-length register is 18 bits, so one
//    transaction carries at most 2^18 bits = 32,768 bytes. A full 320x240
//    frame is 153,600 and even one 80-line LVGL flush buffer is 51,200, so
//    both were rejected outright with "txdata transfer > hardware max
//    supported len". A rejected transfer sends NOTHING, so that region of GRAM
//    kept whatever was in it — the black rectangular blocks. Small dirty rects
//    stayed under the limit, which is why the trackball cursor still worked
//    while opening an app did not.
//
// 2. BOUNCE BUFFER — with the bulk buffer in PSRAM, spi_device_queue_trans()
//    could not DMA from it directly and tried to allocate a temporary
//    INTERNAL-RAM copy per transaction. At 32KB that fails, giving
//    ESP_ERR_NO_MEM and, again, an unsent region. It also meant every pixel was
//    being copied twice for nothing.
//
// So the buffer now lives in INTERNAL DMA-capable RAM and is one chunk in size,
// and push_pixels walks the source rect chunk by chunk. Three consequences, all
// good: no transaction can ever exceed the hardware limit regardless of rect
// size, there is no bounce buffer because the source is already DMA-capable,
// and the byte-swap writes into internal RAM rather than PSRAM.
//
// 16KB is the target. Internal DRAM on this device is tight (a 12288-byte task
// stack already overflowed the segment), so the allocator walks down rather
// than failing outright — 16K, then 8K, then 4K. Even the 4K floor is a large
// improvement: a full-screen push goes from 240 transactions to 38.
#define ST7789_CHUNK_PREF  (16 * 1024)
#define ST7789_CHUNK_FLOOR (4 * 1024)

// ── Step 0 instrumentation (DP8_CHECKLIST.md) ───────────────────────────
//
// TEMPORARY. Exists to answer "where does the frame time actually go" with
// numbers instead of guesses, and comes out once Steps 1-2 are landed and
// measured. See DP8_CHECKLIST.md's baseline table.
//
// Aggregated over a window and logged once per window rather than per flush:
// a log line per flush would itself dominate what it is trying to measure.
// The counters are plain uint32_t written only from the render task (the sole
// caller of push_pixels) and read only in the same place, so no atomics.
#define ST7789_PERF_WINDOW 120   // flushes per log line

static uint32_t s_perf_flushes   = 0;
static uint32_t s_perf_trans     = 0;    // SPI transactions this window
static uint64_t s_perf_total_us  = 0;
static uint32_t s_perf_max_us    = 0;
static uint64_t s_perf_total_px  = 0;

// Bumped by spi_transmit_bounded() — counts every transaction including the
// CASET/RASET/RAMWR command writes, because those are real bus round-trips
// with real per-transaction overhead, not free.
static uint32_t s_perf_trans_ctr = 0;

// ── Public configure API ──────────────────────────────────────────────────────

void st7789_configure(int cs, int dc, int mosi, int miso, int sclk, int rst, int bl)
{
    s_pins.cs_pin   = cs;
    s_pins.dc_pin   = dc;
    s_pins.mosi_pin = mosi;
    s_pins.miso_pin = miso;
    s_pins.sclk_pin = sclk;
    s_pins.rst_pin  = rst;
    s_pins.bl_pin   = bl;
}

void st7789_set_spi_host(spi_host_device_t host)
{
    s_spi_host = host;
}

void st7789_set_spi_freq(uint32_t freq_hz)
{
    s_spi_freq_hz = freq_hz;
}

void st7789_set_perf_mode(bool enable)
{
    if (!enable) {
        if (s_bulk_buf) {
            heap_caps_free(s_bulk_buf);
            s_bulk_buf    = NULL;
            s_bulk_buf_px = 0;
            ESP_LOGI(TAG, "perf mode disabled — back to row-by-row push");
        }
        return;
    }
    if (s_bulk_buf) return;   // already on

    // INTERNAL DMA-capable RAM, deliberately — not PSRAM. See the chunk-size
    // comment above: a PSRAM source forces the SPI driver to bounce every
    // transaction through an internal-RAM copy it allocates per transfer,
    // which both wastes a full copy and fails with ESP_ERR_NO_MEM once the
    // chunk is large. Internal RAM is also where we want the byte-swap writing.
    //
    // Walk down rather than fail: internal DRAM is scarce on this device.
    for (size_t want = ST7789_CHUNK_PREF; want >= ST7789_CHUNK_FLOOR; want /= 2) {
        uint16_t *buf = (uint16_t *)heap_caps_malloc(want, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (buf) {
            s_bulk_buf    = buf;
            s_bulk_buf_px = want / sizeof(uint16_t);
            ESP_LOGW(TAG, "[perf] bulk mode ON — %u-byte internal DMA chunk buffer "
                          "(%u px; full frame = %u chunks, was 240 row transfers)",
                     (unsigned)want, (unsigned)s_bulk_buf_px,
                     (unsigned)(((size_t)s_width * s_height * 2 + want - 1) / want));
            return;
        }
    }
    ESP_LOGW(TAG, "perf mode: no internal DMA buffer down to %u bytes — staying on row-by-row push",
             (unsigned)ST7789_CHUNK_FLOOR);
}

// ── Low-level SPI helpers ─────────────────────────────────────────────────────

// Bounded per-transaction timeout, same mechanism and same reasoning as
// RadioLib's EspHal::spiTransfer() this session: spi_device_transmit()
// (used here previously) is internally just spi_device_queue_trans() +
// spi_device_get_trans_result(), both called with portMAX_DELAY -- if the
// SPI peripheral's hardware transfer-complete condition never fires (the
// confirmed, still-reproducible failure mode on this project's shared
// display/SD/radio bus), that blocks the calling task (cupcake_task, on
// this device -- the same task the crash-guard heartbeat watchdog
// monitors) forever, with no way to recover short of a full reboot.
//
// dev_cfg.queue_size is 1 (see st7789_init(), below) -- confirmed live
// this session on the radio's own bounded-transfer attempt that a queue
// this small means a single timed-out, un-reclaimed transaction
// permanently exhausts the only slot, failing every transfer after it for
// the rest of the boot. The opportunistic non-blocking reclaim below
// (checking whether an earlier abandoned transaction has since actually
// finished) is the fix for that -- it only helps once the earlier
// transaction is no longer stuck, but that's the same honest limitation
// the radio-side version has.
//
// trans must be heap-allocated by the caller, never a stack local --
// deliberately leaked on timeout (freed here only on success), since the
// driver may still complete and write through it after this function has
// already returned false. tx_data (inline, <=4 bytes) and tx_buffer
// pointers into this driver's own static scratch buffers (s_row_buf,
// s_bulk_buf) are both safe to reference even after a timeout for the
// same reason -- neither is stack or short-lived heap memory.
#define ST7789_SPI_TIMEOUT_MS 200

static bool spi_transmit_bounded(spi_transaction_t *trans)
{
    s_perf_trans_ctr++;   // Step 0 instrumentation — see DP8_CHECKLIST.md
    spi_transaction_t *stale = NULL;
    if (spi_device_get_trans_result(s_spi, &stale, 0) == ESP_OK && stale) {
        free(stale);   // reclaim a previous timeout's slot, if it's since finished
    }

    esp_err_t ret = spi_device_queue_trans(s_spi, trans, pdMS_TO_TICKS(ST7789_SPI_TIMEOUT_MS));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "display SPI queue_trans failed: %s", esp_err_to_name(ret));
        free(trans);
        return false;
    }

    spi_transaction_t *rtrans = NULL;
    ret = spi_device_get_trans_result(s_spi, &rtrans, pdMS_TO_TICKS(ST7789_SPI_TIMEOUT_MS));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "display SPI transfer timed out (%s) — bus may still be wedged", esp_err_to_name(ret));
        // Deliberately NOT freed — see doc comment above.
        return false;
    }
    free(trans);
    return true;
}

static void spi_write_cmd(uint8_t cmd)
{
    gpio_set_level((gpio_num_t)s_pins.dc_pin, 0);
    spi_transaction_t *t = (spi_transaction_t *)calloc(1, sizeof(*t));
    if (!t) return;   // OOM — drop this one write rather than block forever
    t->length  = 8;
    t->flags   = SPI_TRANS_USE_TXDATA;
    t->tx_data[0] = cmd;
    spi_transmit_bounded(t);
}

static void spi_write_data(const void *data, size_t len)
{
    if (!len) return;
    gpio_set_level((gpio_num_t)s_pins.dc_pin, 1);
    spi_transaction_t *t = (spi_transaction_t *)calloc(1, sizeof(*t));
    if (!t) return;   // OOM — drop this one write rather than block forever
    t->length = len * 8;
    if (len <= 4) {
        t->flags = SPI_TRANS_USE_TXDATA;
        memcpy(t->tx_data, data, len);
    } else {
        t->flags     = 0;
        t->tx_buffer = data;   // caller's own static buffer — see doc comment above
    }
    spi_transmit_bounded(t);
}

static inline void spi_write_byte(uint8_t d) { spi_write_data(&d, 1); }

static void lcd_cmd_data(uint8_t cmd, const uint8_t *d, size_t n)
{
    spi_write_cmd(cmd);
    if (n) spi_write_data(d, n);
}

// ── Address window ────────────────────────────────────────────────────────────

static void set_addr_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    x0 = (uint16_t)(x0 + s_col_off);
    x1 = (uint16_t)(x1 + s_col_off);
    y0 = (uint16_t)(y0 + s_row_off);
    y1 = (uint16_t)(y1 + s_row_off);

    uint8_t col[4] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF };
    uint8_t row[4] = { y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF };
    lcd_cmd_data(ST_CASET, col, 4);
    lcd_cmd_data(ST_RASET, row, 4);
    spi_write_cmd(ST_RAMWR);
}

// ── ST7789 full init sequence ─────────────────────────────────────────────────

static void st7789_init_regs(uint8_t madctl)
{
    spi_write_cmd(ST_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(150));

    spi_write_cmd(ST_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Pixel format: 16bpp RGB565
    spi_write_cmd(ST_COLMOD); spi_write_byte(0x55);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Memory access / rotation
    spi_write_cmd(ST_MADCTL); spi_write_byte(madctl);

    // Porch control: back/front/separate porch
    { static const uint8_t d[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
      lcd_cmd_data(ST_PORCTRL, d, 5); }

    // Gate control: VGH=14.7V VGL=-7.35V
    spi_write_cmd(ST_GCTRL); spi_write_byte(0x35);

    // VCOM: 1.35V
    spi_write_cmd(ST_VCOMS); spi_write_byte(0x28);

    // LCM control
    spi_write_cmd(ST_LCMCTRL); spi_write_byte(0x0C);

    // VDV/VRH enable
    spi_write_cmd(ST_VDVVRHEN); spi_write_byte(0x01);

    // VRH: +4.45V
    spi_write_cmd(ST_VRHS); spi_write_byte(0x13);

    // VDV: 0V
    spi_write_cmd(ST_VDVS); spi_write_byte(0x20);

    // Frame rate: 60Hz
    spi_write_cmd(ST_FRCTR2); spi_write_byte(0x0F);

    // Power control: AVDD=6.8V AVCL=-4.8V VDDS=2.3V
    { static const uint8_t d[] = {0xA4, 0xA1};
      lcd_cmd_data(ST_PWCTRL1, d, 2); }

    // Positive gamma
    { static const uint8_t d[] = {
          0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32, 0x44,
          0x42, 0x06, 0x0E, 0x12, 0x14, 0x17 };
      lcd_cmd_data(ST_PVGAMCTRL, d, 14); }

    // Negative gamma
    { static const uint8_t d[] = {
          0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31, 0x54,
          0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E };
      lcd_cmd_data(ST_NVGAMCTRL, d, 14); }

    spi_write_cmd(ST_INVON);   // panel requires inversion for correct colors
    spi_write_cmd(ST_NORON);
    vTaskDelay(pdMS_TO_TICKS(10));
    // ST_DISPON sent after GRAM clear
}

// ── Backlight (LEDC PWM) ──────────────────────────────────────────────────────

static void bl_init(int pin)
{
    ledc_timer_config_t tcfg = {
        .speed_mode      = BL_SPEED_MODE,
        .duty_resolution = BL_RESOLUTION,
        .timer_num       = BL_TIMER_NUM,
        .freq_hz         = BL_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tcfg);

    ledc_channel_config_t ccfg = {
        .gpio_num   = pin,
        .speed_mode = BL_SPEED_MODE,
        .channel    = BL_CHANNEL,
        .timer_sel  = BL_TIMER_NUM,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ccfg);
}

static void bl_set(uint8_t level)
{
    ledc_set_duty(BL_SPEED_MODE, BL_CHANNEL, level);
    ledc_update_duty(BL_SPEED_MODE, BL_CHANNEL);
}

// ── Catcall interface ─────────────────────────────────────────────────────────

static esp_err_t st7789_init(const display_config_t *cfg)
{
    if (s_ready) return ESP_OK;

    bool ws169 = false;
    uint8_t rotation = 0;

    if (cfg) {
        if (cfg->backlight_pin != 0) s_pins.bl_pin = (int)cfg->backlight_pin;
        rotation = cfg->rotation;
        ws169    = (cfg->flags & ST7789_FLAG_WS169) != 0;
    }

    // Set panel geometry
    if (ws169) {
        s_width   = ST7789_WS169_W;
        s_height  = ST7789_WS169_H;
        s_col_off = 0;
        s_row_off = 20;  // 240×280 GRAM sits at row offset 20 in a 240×320 controller
    } else {
        s_width   = ST7789_WIDTH;
        s_height  = ST7789_HEIGHT;
        s_col_off = 0;
        s_row_off = 0;
    }

    // DC pin
    gpio_config_t dc_cfg = {
        .pin_bit_mask = 1ULL << s_pins.dc_pin,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&dc_cfg));
    gpio_set_level((gpio_num_t)s_pins.dc_pin, 1);

    // Optional hardware reset
    if (s_pins.rst_pin >= 0) {
        gpio_config_t rst_cfg = {
            .pin_bit_mask = 1ULL << s_pins.rst_pin,
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&rst_cfg));
        gpio_set_level((gpio_num_t)s_pins.rst_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level((gpio_num_t)s_pins.rst_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    // SPI bus
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = s_pins.mosi_pin,
        .miso_io_num     = s_pins.miso_pin,
        .sclk_io_num     = s_pins.sclk_pin,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = ST7789_WIDTH * ST7789_HEIGHT * 2 + 8,
    };
    esp_err_t ret = spi_bus_initialize(s_spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = s_spi_freq_hz,
        .mode           = 0,
        .spics_io_num   = s_pins.cs_pin,
        .queue_size     = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(s_spi_host, &dev_cfg, &s_spi));

    // What we ASKED for is not necessarily what we GOT. The requested clock is
    // rounded down to a divider the peripheral can actually produce, and on
    // ESP32-S3 a device whose MOSI/SCLK are not the SPI2 IOMUX pins is routed
    // through the GPIO matrix, which does not reliably carry 80MHz. T-Deck Plus
    // is exactly that case (MOSI=41, SCLK=40 — neither is FSPI IOMUX), so this
    // asks the driver rather than assuming. Every throughput estimate in
    // DP8_CHECKLIST.md scales directly off this number.
    // Diagnostic only — the transaction size is now bounded by the chunk buffer
    // (see ST7789_CHUNK_PREF), not by this. Logged because it is the limit that
    // caused the black-block corruption, and it is worth seeing on any new board.
    {
        size_t max_bytes = 0;
        if (spi_bus_get_max_transaction_len(s_spi_host, &max_bytes) == ESP_OK) {
            ESP_LOGW(TAG, "[perf] bus max transaction: %u bytes", (unsigned)max_bytes);
        }
    }

    {
        int actual_khz = 0;
        if (spi_device_get_actual_freq(s_spi, &actual_khz) == ESP_OK) {
            ESP_LOGW(TAG, "[perf] SPI clock: requested %lu kHz, actual %d kHz%s",
                     (unsigned long)(s_spi_freq_hz / 1000), actual_khz,
                     (actual_khz * 1000u < s_spi_freq_hz) ? "  <-- CLAMPED" : "");
        } else {
            ESP_LOGW(TAG, "[perf] SPI clock: could not read actual freq");
        }
    }

    // Panel init sequence
    uint8_t madctl = ws169 ? MADCTL_PORTRAIT
                            : ((rotation == 0) ? MADCTL_LANDSCAPE : MADCTL_PORTRAIT);
#ifdef CONFIG_PURR_DISPLAY_ST7789_BGR_ORDER
    madctl |= 0x08;   // panel is physically wired BGR — see Kconfig help text
#endif
    st7789_init_regs(madctl);

    // Clear GRAM before display on.
    // Reuses s_row_buf (static, zeroed here) rather than a heap-allocated-
    // then-freed buffer — deliberate, not a downgrade: with bounded SPI
    // transfers below, a timed-out transaction's descriptor may still be
    // touched by the driver later, and a buffer freed immediately after
    // this loop (the previous heap_caps_calloc()+free() pattern) would be
    // a genuine use-after-free in that case. s_row_buf lives for the whole
    // program, so it's always safe to reference even if a write to it
    // times out. The ESP-IDF SPI driver already handles non-MALLOC_CAP_DMA
    // source pointers transparently (an internal bounce-buffer copy — see
    // spi_master.c's own tx_unaligned handling) so this isn't a
    // correctness downgrade, and this path only runs once at boot.
    // Send DISPOFF first so the panel never shows un-cleared GRAM.
    spi_write_cmd(0x28); // DISPOFF
    vTaskDelay(pdMS_TO_TICKS(10));
    {
        const int ROW_BYTES = s_width * 2;
        memset(s_row_buf, 0, sizeof(s_row_buf));
        set_addr_window(0, 0, (uint16_t)(s_width - 1), (uint16_t)(s_height - 1));
        for (int r = 0; r < s_height; r++) {
            spi_write_data(s_row_buf, (size_t)ROW_BYTES);
        }
    }
    s_ready = true;

    spi_write_cmd(ST_DISPON);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Backlight
    if (s_pins.bl_pin >= 0) {
        bl_init(s_pins.bl_pin);
        bl_set(255);
    }

    ESP_LOGI(TAG, "ST7789 ready %dx%d%s", s_width, s_height,
             ws169 ? " (ws169)" : "");
    return ESP_OK;
}

// set_addr_window() + the pixel-write loop below are together ONE logical
// "draw this rect" operation on the chip — CASET, RASET, RAMWR, then every
// row's data must land as one uninterrupted sequence, or the ST7789's
// on-chip address-window state can be left inconsistent (a stray radio
// transaction landing between CASET and RAMWR, say). Each individual
// spi_write_cmd()/spi_write_data() call is already bounded on its own
// (spi_transmit_bounded(), 200ms), but nothing previously stopped the
// RADIO from interleaving a transaction of its OWN between any two of
// these — the radio's EspHal.cpp does bracket ITS transfers with
// spi_device_acquire_bus()/release_bus(), which incidentally serializes
// against this driver too, but that's a one-sided guarantee from the
// radio's side, not something this driver ever provided for its own
// multi-transaction sequences. spi_device_acquire_bus(s_spi, ...) here
// closes that gap directly, mirroring EspHal.cpp's own bracket pattern.
// Every spi_write_cmd()/spi_write_data() call inside stays non-fatal on
// its own timeout (neither returns a status this driver acts on), so the
// only paths that need an explicit release are the existing return
// statements below — nothing here bails out early on a failed transfer.
static esp_err_t st7789_push_pixels_inner(int x, int y, int w, int h, const uint16_t *data)
{
    if (!s_ready || !data || w <= 0 || h <= 0) return ESP_ERR_INVALID_STATE;

    spi_device_acquire_bus(s_spi, portMAX_DELAY);

    set_addr_window((uint16_t)x, (uint16_t)y,
                    (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));

    int cols = (w <= ST7789_WIDTH) ? w : ST7789_WIDTH;
    size_t px = (size_t)cols * (size_t)h;

    // Bulk path: byte-swap the whole dirty rect into one PSRAM buffer and
    // send it as a single SPI transaction, instead of one per row. Same
    // swap math as the fallback below, just writing into a bigger
    // contiguous buffer. Falls through to the row-by-row path whenever
    // perf mode is off or the rect is larger than the buffer (shouldn't
    // happen — the buffer is sized for a full frame — but never assume).
    // Bulk path: byte-swap into the internal DMA chunk buffer and send, one
    // chunk at a time, until the whole rect is out.
    //
    // No size limit applies — the rect can be a single row or the entire
    // screen, because the transaction size is bounded by the buffer rather
    // than by the rect. That is the whole point of the rewrite: the previous
    // "swap it all, send it in one go" shape is what hit both the 32,768-byte
    // hardware transaction limit and the PSRAM bounce-buffer allocation.
    //
    // The panel does not see the seams. After RAMWR the ST7789 consumes a
    // continuous pixel stream and advances its own GRAM pointer, so where the
    // stream is split is invisible to it — the only requirement is that a split
    // never falls mid-pixel, which holds because the buffer is a whole number
    // of uint16_t.
    if (s_bulk_buf) {
        const uint16_t *src  = data;          // walks the source rect row-major
        size_t          left = px;            // pixels still to send
        int             col  = 0;             // column within the current row
        while (left) {
            size_t n = (left < s_bulk_buf_px) ? left : s_bulk_buf_px;
            for (size_t i = 0; i < n; i++) {
                uint16_t v = src[col];
                s_bulk_buf[i] = (uint16_t)((v >> 8) | (v << 8));
                // Source rows are w apart but only `cols` wide are sent, so
                // step over the remainder at each row end. Identical geometry
                // to the row-by-row path below, just not resynchronised to a
                // row boundary on every transfer.
                if (++col == cols) { col = 0; src += w; }
            }
            spi_write_data(s_bulk_buf, n * 2);
            left -= n;
        }
        spi_device_release_bus(s_spi);
        return ESP_OK;
    }

    for (int r = 0; r < h; r++) {
        const uint16_t *src = data + r * w;
        for (int i = 0; i < cols; i++) {
            s_row_buf[i] = (uint16_t)((src[i] >> 8) | (src[i] << 8));
        }
        spi_write_data(s_row_buf, (size_t)(cols * 2));
    }
    spi_device_release_bus(s_spi);
    return ESP_OK;
}

// Step 0 instrumentation wrapper (DP8_CHECKLIST.md). TEMPORARY — remove with
// the counters above once Steps 1-2 are measured.
//
// A wrapper rather than probes inside the function so that every exit path is
// timed without editing any of them, and so deleting this later is one clean
// excision that cannot leave a half-instrumented body behind.
//
// What the log line answers:
//   trans/flush   how many SPI round-trips one LVGL flush costs. Row-by-row
//                 this tracks the flush height (~80) + 3 command writes; the
//                 bulk path should collapse it to ~4.
//   us/flush      wall time inside push_pixels. Compare against the pure wire
//                 time implied by the actual clock logged at init: the gap is
//                 per-transaction overhead, which is what Step 2 attacks.
//   px/flush      guards against reading the above without knowing how much
//                 was actually drawn — a small dirty rect is cheap for real
//                 reasons and should not be mistaken for a win.
static esp_err_t st7789_push_pixels(int x, int y, int w, int h, const uint16_t *data)
{
    uint32_t  t0     = s_perf_trans_ctr;
    int64_t   start  = esp_timer_get_time();

    esp_err_t ret    = st7789_push_pixels_inner(x, y, w, h, data);

    uint32_t  us     = (uint32_t)(esp_timer_get_time() - start);
    s_perf_flushes++;
    s_perf_trans    += (s_perf_trans_ctr - t0);
    s_perf_total_us += us;
    s_perf_total_px += (uint64_t)(w > 0 ? w : 0) * (uint64_t)(h > 0 ? h : 0);
    if (us > s_perf_max_us) s_perf_max_us = us;

    if (s_perf_flushes >= ST7789_PERF_WINDOW) {
        ESP_LOGW(TAG, "[perf] %lu flushes: %lu trans/flush, %lu us/flush avg, "
                      "%lu us max, %lu px/flush, %s",
                 (unsigned long)s_perf_flushes,
                 (unsigned long)(s_perf_trans   / s_perf_flushes),
                 (unsigned long)(s_perf_total_us / s_perf_flushes),
                 (unsigned long)s_perf_max_us,
                 (unsigned long)(s_perf_total_px / s_perf_flushes),
                 s_bulk_buf ? "BULK" : "row-by-row");
        s_perf_flushes  = 0;
        s_perf_trans    = 0;
        s_perf_total_us = 0;
        s_perf_max_us   = 0;
        s_perf_total_px = 0;
    }
    return ret;
}

// See st7789_push_pixels()'s doc comment — same reasoning applies here.
static esp_err_t st7789_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_ready || w <= 0 || h <= 0) return ESP_ERR_INVALID_STATE;

    int cols = (w <= ST7789_WIDTH) ? w : ST7789_WIDTH;
    uint16_t swapped = (uint16_t)((color >> 8) | (color << 8));
    for (int i = 0; i < cols; i++) s_row_buf[i] = swapped;

    spi_device_acquire_bus(s_spi, portMAX_DELAY);

    set_addr_window((uint16_t)x, (uint16_t)y,
                    (uint16_t)(x + cols - 1), (uint16_t)(y + h - 1));
    for (int r = 0; r < h; r++) {
        spi_write_data(s_row_buf, (size_t)(cols * 2));
    }
    spi_device_release_bus(s_spi);
    return ESP_OK;
}

static esp_err_t st7789_set_brightness(uint8_t level)
{
    if (s_pins.bl_pin < 0) return ESP_ERR_NOT_SUPPORTED;
    bl_set(level);
    return ESP_OK;
}

static void st7789_get_info(display_info_t *out)
{
    if (!out) return;
    out->width          = s_width;
    out->height         = s_height;
    out->bits_per_pixel = 16;
    strncpy(out->name, "ST7789", sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = '\0';
}

static esp_err_t st7789_deinit(void)
{
    if (!s_ready) return ESP_OK;
    if (s_pins.bl_pin >= 0) bl_set(0);
    spi_bus_remove_device(s_spi);
    s_spi   = NULL;
    s_ready = false;
    return ESP_OK;
}

// ── Catcall descriptor ────────────────────────────────────────────────────────

static const catcall_display_t s_catcall = {
    .name            = "st7789",
    .catcall_version = CATCALL_DISPLAY_VERSION,
    .init            = st7789_init,
    .push_pixels     = st7789_push_pixels,
    .fill_rect       = st7789_fill_rect,
    .set_brightness  = st7789_set_brightness,
    .get_info        = st7789_get_info,
    .deinit          = st7789_deinit,
};

// ── Purr module lifecycle hooks ───────────────────────────────────────────────

int st7789_drv_init(void)
{
    esp_err_t ret = st7789_init(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "hardware init failed: %s", esp_err_to_name(ret));
        return -1;
    }
    purr_kernel_register_display(&s_catcall);
    return 0;
}

static void st7789_drv_deinit(void)
{
    st7789_deinit();
}

// ── Module header ─────────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(st7789) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .load_priority     = PURR_PRIORITY_REQUIRED,
    .name              = "st7789",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = CATCALL_FLAG_DISPLAY,
    .required_catcalls = 0,
    .init              = st7789_drv_init,
    .deinit            = st7789_drv_deinit,
};
