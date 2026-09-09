// ssd1306.c — SSD1306 I2C OLED display driver for PURR OS
// Catcall-compatible, ESP-IDF v5.x, pure C.
//
// Target hardware: 128×64 monochrome OLED (Heltec WiFi LoRa 32 and compatible)
//   Default I2C: SDA=17, SCL=18, RST=21, addr=0x3C
//
// The SSD1306 is monochrome (1bpp). The catcall interface delivers RGB565 pixels.
// push_pixels() converts incoming RGB565 to 1-bit by luminance threshold and
// writes the result into a 1KB framebuffer (128×64 / 8), then flushes the
// affected pages to the controller over I2C.
//
// Internal framebuffer layout: column-major pages.
//   page p covers rows [p*8 .. p*8+7], columns 0..127.
//   s_fb[p * 128 + col] holds bits for column `col` in page `p`,
//   bit 0 = top row of the page, bit 7 = bottom row.
//
// Call ssd1306_configure() with your pin assignments, then register the
// purr_module. The kernel calls purr_module.init → ssd1306_drv_init().

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/catcall_display.h"
#include "purr_quirk.h"

// ── Default pin assignments (Heltec WiFi LoRa 32) ────────────────────────────
#ifndef CONFIG_SSD1306_SDA_PIN
#  define CONFIG_SSD1306_SDA_PIN  17
#endif
#ifndef CONFIG_SSD1306_SCL_PIN
#  define CONFIG_SSD1306_SCL_PIN  18
#endif
#ifndef CONFIG_SSD1306_RST_PIN
#  define CONFIG_SSD1306_RST_PIN  21
#endif
#ifndef CONFIG_SSD1306_I2C_ADDR
#  define CONFIG_SSD1306_I2C_ADDR 0x3C
#endif
#ifndef CONFIG_SSD1306_I2C_PORT
#  define CONFIG_SSD1306_I2C_PORT I2C_NUM_0
#endif
#ifndef CONFIG_SSD1306_I2C_FREQ_HZ
#  define CONFIG_SSD1306_I2C_FREQ_HZ  400000
#endif

// Panel geometry
#define SSD1306_WIDTH   128
#define SSD1306_HEIGHT  64
#define SSD1306_PAGES   8       // 64 / 8
#define SSD1306_FB_SIZE (SSD1306_WIDTH * SSD1306_PAGES)   // 1024 bytes

// SSD1306 command bytes
#define SSD_DISPLAYOFF         0xAE
#define SSD_DISPLAYON          0xAF
#define SSD_SETDISPLAYCLOCKDIV 0xD5
#define SSD_SETMULTIPLEX       0xA8
#define SSD_SETDISPLAYOFFSET   0xD3
#define SSD_SETSTARTLINE       0x40   // | line (0..63)
#define SSD_CHARGEPUMP         0x8D
#define SSD_MEMORYMODE         0x20
#define SSD_SEGREMAP_NORMAL    0xA0
#define SSD_SEGREMAP_FLIP      0xA1
#define SSD_COMSCANDEC         0xC8
#define SSD_COMSCANINC         0xC0
#define SSD_SETCOMPINS         0xDA
#define SSD_SETCONTRAST        0x81
#define SSD_SETPRECHARGE       0xD9
#define SSD_SETVCOMDETECT      0xDB
#define SSD_DISPLAYALLON_RESUME 0xA4
#define SSD_NORMALDISPLAY      0xA6
#define SSD_INVERTDISPLAY      0xA7
#define SSD_PAGEADDR           0x22
#define SSD_COLUMNADDR         0x21
#define SSD_SCROLL_STOP        0x2E

// I2C control bytes (Co=0, D/C#=0 → command stream; Co=0, D/C#=1 → data stream)
#define SSD_I2C_CMD_STREAM  0x00
#define SSD_I2C_DAT_STREAM  0x40

// Luma threshold for RGB565 → 1-bit conversion (0..255)
#define SSD_LUMA_THRESHOLD  128

static const char *TAG = "drv:ssd1306";

// ── Driver state ──────────────────────────────────────────────────────────────

typedef struct {
    int     sda_pin;
    int     scl_pin;
    int     rst_pin;
    uint8_t i2c_addr;
    int     i2c_port;
    int     i2c_freq_hz;
} ssd1306_config_t;

static ssd1306_config_t s_cfg = {
    .sda_pin    = CONFIG_SSD1306_SDA_PIN,
    .scl_pin    = CONFIG_SSD1306_SCL_PIN,
    .rst_pin    = CONFIG_SSD1306_RST_PIN,
    .i2c_addr   = CONFIG_SSD1306_I2C_ADDR,
    .i2c_port   = CONFIG_SSD1306_I2C_PORT,
    .i2c_freq_hz = CONFIG_SSD1306_I2C_FREQ_HZ,
};

static bool    s_ready = false;
static uint8_t s_fb[SSD1306_FB_SIZE];   // 128×8 pages = 1024 bytes

// Layout for a "ssd1306.pins" .purr v2 quirk block — see purr_quirk_pkg.h.
// All fields widened to int32_t (including i2c_addr, a uint8_t in the
// caller's own signature) so every field lands on a natural 4-byte
// boundary with zero implicit padding — same convention purr_quirk_pkg_
// header_t's own doc comment explains.
typedef struct {
    int32_t sda;
    int32_t scl;
    int32_t rst;
    int32_t addr;
    int32_t port;
} ssd1306_pin_quirk_t;

// ── Public configure API ──────────────────────────────────────────────────────

void ssd1306_configure(int sda, int scl, int rst, uint8_t addr, int port)
{
    // A loaded .purr v2 quirk package's "ssd1306.pins" block, if present
    // and correctly sized, overrides the CALLER's own values — same
    // precedence gt911_configure()/adc_battery's own quirk checks already use.
    size_t qsz = 0;
    const void *qblk = purr_quirk_get_block("ssd1306.pins", &qsz);
    if (qblk && qsz == sizeof(ssd1306_pin_quirk_t)) {
        const ssd1306_pin_quirk_t *q = (const ssd1306_pin_quirk_t *)qblk;
        sda = q->sda; scl = q->scl; rst = q->rst;
        addr = (uint8_t)q->addr; port = q->port;
        ESP_LOGI(TAG, "using loaded quirk package's pin assignment instead of caller's "
                      "(sda=%d scl=%d rst=%d addr=0x%02X port=%d)", sda, scl, rst, addr, port);
    } else if (qblk) {
        ESP_LOGW(TAG, "quirk block 'ssd1306.pins' has wrong size (%u, expected %u) — ignoring",
                 (unsigned)qsz, (unsigned)sizeof(ssd1306_pin_quirk_t));
    }

    s_cfg.sda_pin  = sda;
    s_cfg.scl_pin  = scl;
    s_cfg.rst_pin  = rst;
    s_cfg.i2c_addr = addr;
    s_cfg.i2c_port = port;
}

// ── I2C helpers ───────────────────────────────────────────────────────────────

// Send one or more command bytes in a single I2C transaction.
static esp_err_t ssd_send_cmds(const uint8_t *cmds, size_t n)
{
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (uint8_t)((s_cfg.i2c_addr << 1) | I2C_MASTER_WRITE), true);
    i2c_master_write_byte(h, SSD_I2C_CMD_STREAM, true);
    i2c_master_write(h, (uint8_t *)cmds, n, true);
    i2c_master_stop(h);
    esp_err_t ret = i2c_master_cmd_begin((i2c_port_t)s_cfg.i2c_port, h,
                                         pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(h);
    return ret;
}

static inline esp_err_t ssd_cmd1(uint8_t c)
{
    return ssd_send_cmds(&c, 1);
}

static esp_err_t ssd_cmd2(uint8_t c, uint8_t d)
{
    uint8_t buf[2] = { c, d };
    return ssd_send_cmds(buf, 2);
}

// Send raw data bytes (GRAM write) in a single I2C transaction.
// n must be <= I2C buffer limit (typically 4096 bytes on ESP-IDF).
static esp_err_t ssd_send_data(const uint8_t *data, size_t n)
{
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (uint8_t)((s_cfg.i2c_addr << 1) | I2C_MASTER_WRITE), true);
    i2c_master_write_byte(h, SSD_I2C_DAT_STREAM, true);
    i2c_master_write(h, (uint8_t *)data, n, true);
    i2c_master_stop(h);
    esp_err_t ret = i2c_master_cmd_begin((i2c_port_t)s_cfg.i2c_port, h,
                                         pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(h);
    return ret;
}

// ── Page flush helpers ────────────────────────────────────────────────────────

// Flush a range of pages (inclusive) to the controller.
// Uses horizontal addressing mode (MEMORYMODE=0x00) with COLUMNADDR / PAGEADDR.
static esp_err_t flush_pages(uint8_t page_start, uint8_t page_end)
{
    // Set column address 0..127
    uint8_t col_cmd[3] = { SSD_COLUMNADDR, 0, SSD1306_WIDTH - 1 };
    ssd_send_cmds(col_cmd, 3);

    // Set page address range
    uint8_t page_cmd[3] = { SSD_PAGEADDR, page_start, page_end };
    ssd_send_cmds(page_cmd, 3);

    // Transmit framebuffer slice
    size_t  offset = (size_t)(page_start * SSD1306_WIDTH);
    size_t  len    = (size_t)((page_end - page_start + 1) * SSD1306_WIDTH);
    return ssd_send_data(s_fb + offset, len);
}

static inline esp_err_t flush_all(void)
{
    return flush_pages(0, SSD1306_PAGES - 1);
}

// ── RGB565 → 1-bit conversion ─────────────────────────────────────────────────
// Compute luminance from RGB565 (BT.601 luma, scaled to 0..255).
// R5G6B5: rrrrrggggggbbbbb
//
// Was previously computing `r*77 + g*38 + b*13` directly on the raw 5/6-bit
// channel values and calling that "0..255" — it isn't: for pure white
// (r=31,g=63,b=31) that expression is 5184, and 5184>>4 = 324, which
// silently wraps to 68 once truncated to this function's uint8_t return
// type. 68 is below SSD_LUMA_THRESHOLD (128), so white was being classified
// as an *off* pixel — confirmed live as the reason nothing ever rendered on
// real SSD1306 hardware, including a full-white test fill (every "on"
// pixel this driver was ever asked to draw hit this same silent wrap).
// Fixed by properly expanding each channel to 8 bits first (bit-replication,
// not a raw scale-and-hope), then applying the standard integer BT.601
// weights (77+150+29 = 256, so the final >>8 is an exact normalize, not an
// approximation) — 255 in, 255 out, verified by construction.
static inline uint8_t rgb565_luma(uint16_t px)
{
    uint8_t r5 = (uint8_t)((px >> 11) & 0x1F);
    uint8_t g6 = (uint8_t)((px >>  5) & 0x3F);
    uint8_t b5 = (uint8_t)( px        & 0x1F);
    uint8_t r8 = (uint8_t)((r5 << 3) | (r5 >> 2));
    uint8_t g8 = (uint8_t)((g6 << 2) | (g6 >> 4));
    uint8_t b8 = (uint8_t)((b5 << 3) | (b5 >> 2));
    uint32_t luma = (uint32_t)r8 * 77u + (uint32_t)g8 * 150u + (uint32_t)b8 * 29u;
    return (uint8_t)(luma >> 8);
}

// Write a single pixel into the framebuffer.
static inline void fb_set_pixel(int x, int y, bool on)
{
    if ((unsigned)x >= SSD1306_WIDTH || (unsigned)y >= SSD1306_HEIGHT) return;
    int     page = y >> 3;
    int     bit  = y & 7;
    size_t  idx  = (size_t)(page * SSD1306_WIDTH + x);
    if (on) {
        s_fb[idx] |=  (uint8_t)(1u << bit);
    } else {
        s_fb[idx] &= (uint8_t)~(1u << bit);
    }
}

// ── SSD1306 full init sequence ────────────────────────────────────────────────

static esp_err_t ssd1306_init_regs(void)
{
    esp_err_t ret;

    ret = ssd_cmd1(SSD_DISPLAYOFF);
    if (ret != ESP_OK) return ret;

    // Set clock divide ratio / oscillator freq: divide=1, freq=8
    ssd_cmd2(SSD_SETDISPLAYCLOCKDIV, 0x80);

    // Set multiplex ratio: 63 (64MUX)
    ssd_cmd2(SSD_SETMULTIPLEX, 0x3F);

    // Set display offset: 0
    ssd_cmd2(SSD_SETDISPLAYOFFSET, 0x00);

    // Set start line: 0
    ssd_cmd1(SSD_SETSTARTLINE | 0x00);

    // Enable charge pump (required without external Vcc)
    ssd_cmd2(SSD_CHARGEPUMP, 0x14);

    // Memory addressing mode: horizontal (auto column+page increment)
    ssd_cmd2(SSD_MEMORYMODE, 0x00);

    // Segment remap: column 127 mapped to SEG0 (mirror horizontally)
    ssd_cmd1(SSD_SEGREMAP_FLIP);

    // COM output scan direction: remapped (top to bottom)
    ssd_cmd1(SSD_COMSCANDEC);

    // COM pins hardware config: alternative config, no left-right remap
    ssd_cmd2(SSD_SETCOMPINS, 0x12);

    // Contrast: full brightness
    ssd_cmd2(SSD_SETCONTRAST, 0xCF);

    // Pre-charge period: phase1=1, phase2=15 (0xF1)
    ssd_cmd2(SSD_SETPRECHARGE, 0xF1);

    // VCOMH deselect level: ~0.77×Vcc
    ssd_cmd2(SSD_SETVCOMDETECT, 0x40);

    // Entire display ON (resume from GRAM)
    ssd_cmd1(SSD_DISPLAYALLON_RESUME);

    // Normal display (not inverted)
    ssd_cmd1(SSD_NORMALDISPLAY);

    // Deactivate scroll
    ssd_cmd1(SSD_SCROLL_STOP);

    return ESP_OK;
}

// ── Catcall interface ─────────────────────────────────────────────────────────

static esp_err_t ssd1306_init(const display_config_t *cfg)
{
    if (s_ready) return ESP_OK;
    (void)cfg;  // no SPI/backlight config needed for OLED

    // Optional hardware reset
    if (s_cfg.rst_pin >= 0) {
        gpio_config_t rst_gcfg = {
            .pin_bit_mask = 1ULL << s_cfg.rst_pin,
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&rst_gcfg));
        gpio_set_level((gpio_num_t)s_cfg.rst_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level((gpio_num_t)s_cfg.rst_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // I2C master init
    i2c_config_t i2c_cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = s_cfg.sda_pin,
        .scl_io_num       = s_cfg.scl_pin,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = (uint32_t)s_cfg.i2c_freq_hz,
    };
    esp_err_t ret = i2c_param_config((i2c_port_t)s_cfg.i2c_port, &i2c_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_driver_install((i2c_port_t)s_cfg.i2c_port, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        // ESP_ERR_INVALID_STATE: driver already installed — tolerate
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Short delay after I2C driver install
    vTaskDelay(pdMS_TO_TICKS(10));

    // Send SSD1306 init sequence
    ret = ssd1306_init_regs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ssd1306_init_regs failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Clear framebuffer and push to display
    memset(s_fb, 0x00, SSD1306_FB_SIZE);
    flush_all();

    // Turn display on
    ssd_cmd1(SSD_DISPLAYON);

    s_ready = true;
    ESP_LOGI(TAG, "SSD1306 ready 128x64 @ I2C 0x%02X", s_cfg.i2c_addr);
    return ESP_OK;
}

static esp_err_t ssd1306_push_pixels(int x, int y, int w, int h, const uint16_t *data)
{
    if (!s_ready || !data || w <= 0 || h <= 0) return ESP_ERR_INVALID_STATE;

    int page_min = SSD1306_PAGES;
    int page_max = -1;

    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < 0 || py >= SSD1306_HEIGHT) continue;
        int page = py >> 3;
        if (page < page_min) page_min = page;
        if (page > page_max) page_max = page;

        for (int col = 0; col < w; col++) {
            int px = x + col;
            if (px < 0 || px >= SSD1306_WIDTH) continue;
            uint16_t pixel = data[row * w + col];
            uint8_t  luma  = rgb565_luma(pixel);
            fb_set_pixel(px, py, luma >= SSD_LUMA_THRESHOLD);
        }
    }

    if (page_min <= page_max) {
        flush_pages((uint8_t)page_min, (uint8_t)page_max);
    }
    return ESP_OK;
}

static esp_err_t ssd1306_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_ready || w <= 0 || h <= 0) return ESP_ERR_INVALID_STATE;

    uint8_t luma  = rgb565_luma(color);
    bool    on    = (luma >= SSD_LUMA_THRESHOLD);

    // Clamp to display bounds
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w - 1) >= SSD1306_WIDTH  ? SSD1306_WIDTH  - 1 : x + w - 1;
    int y1 = (y + h - 1) >= SSD1306_HEIGHT ? SSD1306_HEIGHT - 1 : y + h - 1;

    if (x0 > x1 || y0 > y1) return ESP_OK;

    int page_min = y0 >> 3;
    int page_max = y1 >> 3;

    for (int py = y0; py <= y1; py++) {
        int     page  = py >> 3;
        int     bit   = py & 7;
        uint8_t bmask = (uint8_t)(1u << bit);
        for (int px = x0; px <= x1; px++) {
            size_t idx = (size_t)(page * SSD1306_WIDTH + px);
            if (on) {
                s_fb[idx] |=  bmask;
            } else {
                s_fb[idx] &= (uint8_t)~bmask;
            }
        }
    }

    flush_pages((uint8_t)page_min, (uint8_t)page_max);
    return ESP_OK;
}

// set_brightness maps 0..255 → SSD1306 contrast 0x00..0xFF
static esp_err_t ssd1306_set_brightness(uint8_t level)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    return ssd_cmd2(SSD_SETCONTRAST, level);
}

static void ssd1306_get_info(display_info_t *out)
{
    if (!out) return;
    out->width          = SSD1306_WIDTH;
    out->height         = SSD1306_HEIGHT;
    out->bits_per_pixel = 1;
    strncpy(out->name, "SSD1306", sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = '\0';
}

static esp_err_t ssd1306_deinit(void)
{
    if (!s_ready) return ESP_OK;
    ssd_cmd1(SSD_DISPLAYOFF);
    i2c_driver_delete((i2c_port_t)s_cfg.i2c_port);
    s_ready = false;
    return ESP_OK;
}

// ── Catcall descriptor ────────────────────────────────────────────────────────

static const catcall_display_t s_catcall = {
    .name            = "ssd1306",
    .catcall_version = CATCALL_DISPLAY_VERSION,
    .init            = ssd1306_init,
    .push_pixels     = ssd1306_push_pixels,
    .fill_rect       = ssd1306_fill_rect,
    .set_brightness  = ssd1306_set_brightness,
    .get_info        = ssd1306_get_info,
    .deinit          = ssd1306_deinit,
};

// ── Purr module lifecycle hooks ───────────────────────────────────────────────

static int ssd1306_drv_init(void)
{
    esp_err_t ret = ssd1306_init(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "hardware init failed: %s", esp_err_to_name(ret));
        return -1;
    }
    purr_kernel_register_display(&s_catcall);
    return 0;
}

static void ssd1306_drv_deinit(void)
{
    ssd1306_deinit();
}

// ── Module header ─────────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(ssd1306) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .load_priority     = PURR_PRIORITY_REQUIRED,
    .name              = "ssd1306",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = CATCALL_FLAG_DISPLAY,
    .required_catcalls = 0,
    .init              = ssd1306_drv_init,
    .deinit            = ssd1306_drv_deinit,
};
