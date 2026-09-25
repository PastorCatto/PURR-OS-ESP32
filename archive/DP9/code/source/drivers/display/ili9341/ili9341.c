// ili9341.c — ILI9341 SPI display driver for PURR OS
// Catcall-compatible, ESP-IDF v5.x, pure C, synchronous SPI (no DMA races).
//
// Target hardware: ESP32-2432S028R / S024C (CYD) — ILI9341 2.8" 320×240
//   Default pins: MOSI=13 MISO=12 SCLK=14 CS=15 DC=2 RST=-1 BL=21
//   S024C variant: BL=27 (set via ili9341_configure() before init)
//
// Call ili9341_configure() with your pin assignments, then register the
// purr_module. The kernel calls purr_module.init → ili9341_drv_init().

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/catcall_display.h"
#include "purr_quirk.h"

// ── Default pin assignments (CYD / ESP32-2432S028R) ──────────────────────────
#ifndef CONFIG_DRV_DISPLAY_CS_PIN
#  define CONFIG_DRV_DISPLAY_CS_PIN   15
#endif
#ifndef CONFIG_DRV_DISPLAY_DC_PIN
#  define CONFIG_DRV_DISPLAY_DC_PIN   2
#endif
#ifndef CONFIG_DRV_DISPLAY_MOSI_PIN
#  define CONFIG_DRV_DISPLAY_MOSI_PIN 13
#endif
#ifndef CONFIG_DRV_DISPLAY_SCLK_PIN
#  define CONFIG_DRV_DISPLAY_SCLK_PIN 14
#endif
#ifndef CONFIG_DRV_DISPLAY_RST_PIN
#  define CONFIG_DRV_DISPLAY_RST_PIN  (-1)
#endif
#ifndef CONFIG_DRV_DISPLAY_BL_PIN
#  define CONFIG_DRV_DISPLAY_BL_PIN   21
#endif

// ILI9341 native resolution
#define ILI9341_WIDTH   320
#define ILI9341_HEIGHT  240

// SPI host and clock
#define ILI9341_SPI_HOST  SPI2_HOST
#define ILI9341_CLK_HZ    (40 * 1000 * 1000)

// LEDC backlight
#define BL_SPEED_MODE   LEDC_LOW_SPEED_MODE
#define BL_TIMER_NUM    LEDC_TIMER_1
#define BL_CHANNEL      LEDC_CHANNEL_1
#define BL_FREQ_HZ      5000
#define BL_RESOLUTION   LEDC_TIMER_8_BIT

// ILI9341 command codes
#define ILI_SWRESET   0x01
#define ILI_SLPOUT    0x11
#define ILI_NORON     0x13
#define ILI_DISPON    0x29
#define ILI_CASET     0x2A
#define ILI_PASET     0x2B
#define ILI_RAMWR     0x2C
#define ILI_MADCTL    0x36
#define ILI_COLMOD    0x3A
#define ILI_FRMCTR1   0xB1
#define ILI_DFUNCTR   0xB6
#define ILI_PWCTR1    0xC0
#define ILI_PWCTR2    0xC1
#define ILI_VMCTR1    0xC5
#define ILI_VMCTR2    0xC7
#define ILI_GMCTRP1   0xE0
#define ILI_GMCTRN1   0xE1

// MADCTL values
// 0x48 = MX | BGR  → landscape (width=320), columns mirrored for CYD PCB
// 0x28 = MY | BGR  → portrait  (width=240)
#define MADCTL_LANDSCAPE  0x48
#define MADCTL_PORTRAIT   0x28

static const char *TAG = "drv:ili9341";

// ── Driver state ──────────────────────────────────────────────────────────────

typedef struct {
    int cs_pin;
    int dc_pin;
    int mosi_pin;
    int miso_pin;   // optional, set -1 if unused
    int sclk_pin;
    int rst_pin;
    int bl_pin;
} ili9341_pins_t;

static ili9341_pins_t s_pins = {
    .cs_pin   = CONFIG_DRV_DISPLAY_CS_PIN,
    .dc_pin   = CONFIG_DRV_DISPLAY_DC_PIN,
    .mosi_pin = CONFIG_DRV_DISPLAY_MOSI_PIN,
    .miso_pin = 12,
    .sclk_pin = CONFIG_DRV_DISPLAY_SCLK_PIN,
    .rst_pin  = CONFIG_DRV_DISPLAY_RST_PIN,
    .bl_pin   = CONFIG_DRV_DISPLAY_BL_PIN,
};

static spi_device_handle_t s_spi   = NULL;
static bool                s_ready = false;

// Row buffer (320 px × 2 bytes) — reused for fill_rect to avoid heap pressure.
static uint16_t s_row_buf[ILI9341_WIDTH];

// Layout for a "ili9341.pins" .purr v2 quirk block — see purr_quirk_pkg.h.
// Plain int32_t fields matching ili9341_configure()'s own parameter list.
typedef struct {
    int32_t cs;
    int32_t dc;
    int32_t mosi;
    int32_t miso;
    int32_t sclk;
    int32_t rst;
    int32_t bl;
} ili9341_pin_quirk_t;

// ── Public configure API ──────────────────────────────────────────────────────

void ili9341_configure(int cs, int dc, int mosi, int miso,
                       int sclk, int rst, int bl)
{
    // A loaded .purr v2 quirk package's "ili9341.pins" block, if present
    // and correctly sized, overrides the CALLER's own values — same
    // precedence gt911_configure()/adc_battery's own quirk checks already use.
    size_t qsz = 0;
    const void *qblk = purr_quirk_get_block("ili9341.pins", &qsz);
    if (qblk && qsz == sizeof(ili9341_pin_quirk_t)) {
        const ili9341_pin_quirk_t *q = (const ili9341_pin_quirk_t *)qblk;
        cs = q->cs; dc = q->dc; mosi = q->mosi; miso = q->miso;
        sclk = q->sclk; rst = q->rst; bl = q->bl;
        ESP_LOGI(TAG, "using loaded quirk package's pin assignment instead of caller's "
                      "(cs=%d dc=%d mosi=%d miso=%d sclk=%d rst=%d bl=%d)",
                 cs, dc, mosi, miso, sclk, rst, bl);
    } else if (qblk) {
        ESP_LOGW(TAG, "quirk block 'ili9341.pins' has wrong size (%u, expected %u) — ignoring",
                 (unsigned)qsz, (unsigned)sizeof(ili9341_pin_quirk_t));
    }

    s_pins.cs_pin   = cs;
    s_pins.dc_pin   = dc;
    s_pins.mosi_pin = mosi;
    s_pins.miso_pin = miso;
    s_pins.sclk_pin = sclk;
    s_pins.rst_pin  = rst;
    s_pins.bl_pin   = bl;
}

// ── Low-level SPI helpers ─────────────────────────────────────────────────────

static void spi_write_cmd(uint8_t cmd)
{
    gpio_set_level((gpio_num_t)s_pins.dc_pin, 0);
    spi_transaction_t t = {
        .length     = 8,
        .flags      = SPI_TRANS_USE_TXDATA,
        .tx_data    = { cmd, 0, 0, 0 },
    };
    spi_device_transmit(s_spi, &t);
}

static void spi_write_data(const void *data, size_t len)
{
    if (!len) return;
    gpio_set_level((gpio_num_t)s_pins.dc_pin, 1);
    spi_transaction_t t = { .length = len * 8 };
    if (len <= 4) {
        t.flags = SPI_TRANS_USE_TXDATA;
        memcpy(t.tx_data, data, len);
    } else {
        t.flags     = 0;
        t.tx_buffer = data;
    }
    spi_device_transmit(s_spi, &t);
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
    uint8_t col[4] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF };
    uint8_t row[4] = { y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF };
    lcd_cmd_data(ILI_CASET, col, 4);
    lcd_cmd_data(ILI_PASET, row, 4);
    spi_write_cmd(ILI_RAMWR);
}

// ── ILI9341 full init sequence ────────────────────────────────────────────────
// Matches TFT_eSPI ILI9341_Init.h — required by CYD panels.

static void ili9341_init_regs(uint8_t rotation)
{
    spi_write_cmd(ILI_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(120));

    // Vendor-specific power-on (undocumented, required by CYD panel EEPROM)
    { static const uint8_t d[] = {0x03, 0x80, 0x02};
      lcd_cmd_data(0xEF, d, 3); }
    { static const uint8_t d[] = {0x00, 0xC1, 0x30};
      lcd_cmd_data(0xCF, d, 3); }
    { static const uint8_t d[] = {0x64, 0x03, 0x12, 0x81};
      lcd_cmd_data(0xED, d, 4); }
    { static const uint8_t d[] = {0x85, 0x00, 0x78};
      lcd_cmd_data(0xE8, d, 3); }
    { static const uint8_t d[] = {0x39, 0x2C, 0x00, 0x34, 0x02};
      lcd_cmd_data(0xCB, d, 5); }
    { static const uint8_t d[] = {0x20};
      lcd_cmd_data(0xF7, d, 1); }
    { static const uint8_t d[] = {0x00, 0x00};
      lcd_cmd_data(0xEA, d, 2); }

    // Power control
    spi_write_cmd(ILI_PWCTR1); spi_write_byte(0x23);
    spi_write_cmd(ILI_PWCTR2); spi_write_byte(0x10);
    { static const uint8_t d[] = {0x3E, 0x28};
      lcd_cmd_data(ILI_VMCTR1, d, 2); }
    spi_write_cmd(ILI_VMCTR2); spi_write_byte(0x86);

    // Memory access control (rotation / orientation)
    uint8_t madctl = (rotation == 0) ? MADCTL_LANDSCAPE : MADCTL_PORTRAIT;
    spi_write_cmd(ILI_MADCTL); spi_write_byte(madctl);

    // Pixel format: 16bpp RGB565
    spi_write_cmd(ILI_COLMOD); spi_write_byte(0x55);

    // Frame rate: 100 Hz
    { static const uint8_t d[] = {0x00, 0x13};
      lcd_cmd_data(ILI_FRMCTR1, d, 2); }

    // Display function control
    { static const uint8_t d[] = {0x08, 0x82, 0x27};
      lcd_cmd_data(ILI_DFUNCTR, d, 3); }

    // 3-Gamma disable, Gamma curve 1
    spi_write_cmd(0xF2); spi_write_byte(0x00);
    spi_write_cmd(0x26); spi_write_byte(0x01);

    // Positive gamma correction
    { static const uint8_t d[] = {
          0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1,
          0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00 };
      lcd_cmd_data(ILI_GMCTRP1, d, 15); }

    // Negative gamma correction
    { static const uint8_t d[] = {
          0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1,
          0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F };
      lcd_cmd_data(ILI_GMCTRN1, d, 15); }

    spi_write_cmd(ILI_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(120));
    // DISPON is sent after GRAM clear in init()
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

static esp_err_t ili9341_init(const display_config_t *cfg)
{
    if (s_ready) return ESP_OK;

    // Apply backlight pin override from config if caller provided it
    if (cfg && cfg->backlight_pin != 0) {
        s_pins.bl_pin = (int)cfg->backlight_pin;
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
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // SPI bus
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = s_pins.mosi_pin,
        .miso_io_num     = s_pins.miso_pin,
        .sclk_io_num     = s_pins.sclk_pin,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = ILI9341_WIDTH * ILI9341_HEIGHT * 2 + 8,
    };
    esp_err_t ret = spi_bus_initialize(ILI9341_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        // ESP_ERR_INVALID_STATE means bus already initialized — tolerate it
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // SPI device
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = ILI9341_CLK_HZ,
        .mode           = 0,
        .spics_io_num   = s_pins.cs_pin,
        .queue_size     = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(ILI9341_SPI_HOST, &dev_cfg, &s_spi));

    // Panel init sequence
    uint8_t rotation = cfg ? cfg->rotation : 0;
    ili9341_init_regs(rotation);

    // Clear GRAM to black before turning display on
    s_ready = true;

    // Fill black row buffer
    memset(s_row_buf, 0x00, sizeof(s_row_buf));
    set_addr_window(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);
    for (int r = 0; r < ILI9341_HEIGHT; r++) {
        spi_write_data(s_row_buf, ILI9341_WIDTH * 2);
    }

    spi_write_cmd(ILI_DISPON);

    // Backlight
    if (s_pins.bl_pin >= 0) {
        bl_init(s_pins.bl_pin);
        bl_set(255);
    }

    ESP_LOGI(TAG, "ILI9341 ready %dx%d", ILI9341_WIDTH, ILI9341_HEIGHT);
    return ESP_OK;
}

static esp_err_t ili9341_push_pixels(int x, int y, int w, int h, const uint16_t *data)
{
    if (!s_ready || !data || w <= 0 || h <= 0) return ESP_ERR_INVALID_STATE;

    set_addr_window((uint16_t)x, (uint16_t)y,
                    (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));

    // Byte-swap each row (SPI is big-endian, host is little-endian)
    for (int r = 0; r < h; r++) {
        const uint16_t *src = data + r * w;
        int cols = (w <= ILI9341_WIDTH) ? w : ILI9341_WIDTH;
        for (int i = 0; i < cols; i++) {
            s_row_buf[i] = (uint16_t)((src[i] >> 8) | (src[i] << 8));
        }
        spi_write_data(s_row_buf, (size_t)(cols * 2));
    }
    return ESP_OK;
}

static esp_err_t ili9341_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_ready || w <= 0 || h <= 0) return ESP_ERR_INVALID_STATE;

    int cols = (w <= ILI9341_WIDTH) ? w : ILI9341_WIDTH;
    // Byte-swap color once for big-endian SPI
    uint16_t swapped = (uint16_t)((color >> 8) | (color << 8));
    for (int i = 0; i < cols; i++) s_row_buf[i] = swapped;

    set_addr_window((uint16_t)x, (uint16_t)y,
                    (uint16_t)(x + cols - 1), (uint16_t)(y + h - 1));
    for (int r = 0; r < h; r++) {
        spi_write_data(s_row_buf, (size_t)(cols * 2));
    }
    return ESP_OK;
}

static esp_err_t ili9341_set_brightness(uint8_t level)
{
    if (s_pins.bl_pin < 0) return ESP_ERR_NOT_SUPPORTED;
    bl_set(level);
    return ESP_OK;
}

static void ili9341_get_info(display_info_t *out)
{
    if (!out) return;
    out->width         = ILI9341_WIDTH;
    out->height        = ILI9341_HEIGHT;
    out->bits_per_pixel = 16;
    strncpy(out->name, "ILI9341", sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = '\0';
}

static esp_err_t ili9341_deinit(void)
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
    .name            = "ili9341",
    .catcall_version = CATCALL_DISPLAY_VERSION,
    .init            = ili9341_init,
    .push_pixels     = ili9341_push_pixels,
    .fill_rect       = ili9341_fill_rect,
    .set_brightness  = ili9341_set_brightness,
    .get_info        = ili9341_get_info,
    .deinit          = ili9341_deinit,
};

// ── Purr module lifecycle hooks ───────────────────────────────────────────────

static int ili9341_drv_init(void)
{
    esp_err_t ret = ili9341_init(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "hardware init failed: %s", esp_err_to_name(ret));
        return -1;
    }
    purr_kernel_register_display(&s_catcall);
    return 0;
}

static void ili9341_drv_deinit(void)
{
    ili9341_deinit();
}

// ── Module header ─────────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(ili9341) = {
    .magic            = PURR_MODULE_MAGIC,
    .abi_version      = PURR_MODULE_ABI_VERSION,
    .module_type      = PURR_MOD_DRIVER,
    .load_priority    = PURR_PRIORITY_REQUIRED,
    .name             = "ili9341",
    .version          = "1.0.0",
    .kernel_min       = "0.11.1",
    .kernel_max       = "",
    .provided_catcalls = CATCALL_FLAG_DISPLAY,
    .required_catcalls = 0,
    .init             = ili9341_drv_init,
    .deinit           = ili9341_drv_deinit,
};
