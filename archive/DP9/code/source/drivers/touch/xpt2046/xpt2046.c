// xpt2046.c — XPT2046 resistive touch driver for PURR OS
// Interface: SPI (shared bus with ILI9341 on CYD boards)
//
// CYD board defaults:
//   CS=33, MISO=12, MOSI=13, SCLK=14, IRQ=36
//
// touch_config_t pin mapping (repurposed fields — I2C pins used for SPI):
//   sda_pin  → MISO
//   scl_pin  → SCLK
//   int_pin  → CS
//   rst_pin  → IRQ (active-low, optional)
// i2c_port is not used; we always create a dedicated SPI device.
//
// Calibration:
//   Raw X: 200–3900 → screen 0–319
//   Raw Y: 300–3800 → screen 0–239
//
// ESP-IDF v5.x

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

#include "../../kernel/catcalls/catcall_touch.h"
#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_kernel.h"

static const char *TAG = "xpt2046";

// ── Forward declarations ──────────────────────────────────────────────────────

static const catcall_touch_t s_catcall;
static esp_err_t xpt2046_init(const touch_config_t *cfg);
static bool      xpt2046_read_point(uint16_t *x, uint16_t *y);
static bool      xpt2046_is_pressed(void);
static esp_err_t xpt2046_deinit(void);

// ── CYD board pin defaults ────────────────────────────────────────────────────

#define XPT_DEFAULT_MOSI   13
#define XPT_DEFAULT_MISO   12
#define XPT_DEFAULT_SCLK   14
#define XPT_DEFAULT_CS     33
#define XPT_DEFAULT_IRQ    36

// SPI commands
#define XPT_CMD_X   0xD0   // differential X measurement
#define XPT_CMD_Y   0x90   // differential Y measurement

// Calibration constants (raw ADC → pixel coords)
#define XPT_RAW_X_MIN    200
#define XPT_RAW_X_MAX   3900
#define XPT_RAW_Y_MIN    300
#define XPT_RAW_Y_MAX   3800
#define XPT_SCREEN_W     320
#define XPT_SCREEN_H     240

// Noise rejection: number of samples to average
#define XPT_SAMPLE_COUNT   4

// Minimum raw value above which we consider the screen touched
#define XPT_TOUCH_THRESHOLD  200

// ── Module state ──────────────────────────────────────────────────────────────

static spi_device_handle_t s_spi    = NULL;
static int                 s_irq    = XPT_DEFAULT_IRQ;
static bool                s_initialized = false;

// ── SPI helpers ───────────────────────────────────────────────────────────────

// Read a 12-bit ADC value for the given command byte.
// XPT2046 protocol: send cmd byte, read 2 bytes (MSB first, 12 bits left-aligned).
static uint16_t xpt_read_raw(uint8_t cmd)
{
    uint8_t tx[3] = { cmd, 0x00, 0x00 };
    uint8_t rx[3] = { 0 };

    spi_transaction_t t = {
        .length    = 24,        // 3 bytes
        .tx_buffer = tx,
        .rx_buffer = rx,
        .flags     = 0,
    };
    if (spi_device_polling_transmit(s_spi, &t) != ESP_OK) return 0;

    // Bytes rx[1] and rx[2] contain the 12-bit result, MSB first, left-aligned
    uint16_t raw = ((uint16_t)(rx[1] & 0x7F) << 5) | (rx[2] >> 3);
    return raw;
}

// Average multiple samples for noise rejection
static uint16_t xpt_sample_avg(uint8_t cmd)
{
    uint32_t sum = 0;
    for (int i = 0; i < XPT_SAMPLE_COUNT; i++) {
        sum += xpt_read_raw(cmd);
    }
    return (uint16_t)(sum / XPT_SAMPLE_COUNT);
}

// Map raw ADC value to screen coordinate, clamped to [0, max_px]
static uint16_t xpt_map(uint16_t raw, uint16_t raw_min, uint16_t raw_max, uint16_t max_px)
{
    if (raw <= raw_min) return 0;
    if (raw >= raw_max) return max_px;
    return (uint16_t)((uint32_t)(raw - raw_min) * max_px / (raw_max - raw_min));
}

// ── catcall_touch_t implementation ────────────────────────────────────────────

static esp_err_t xpt2046_init(const touch_config_t *cfg)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    // Resolve pins from config fields (see file-top comment for mapping)
    int pin_mosi = XPT_DEFAULT_MOSI;   // MOSI is fixed on shared CYD bus
    int pin_miso = cfg ? cfg->sda_pin  : XPT_DEFAULT_MISO;
    int pin_sclk = cfg ? cfg->scl_pin  : XPT_DEFAULT_SCLK;
    int pin_cs   = cfg ? cfg->int_pin  : XPT_DEFAULT_CS;
    s_irq        = cfg ? cfg->rst_pin  : XPT_DEFAULT_IRQ;

    // Install SPI bus (may already be initialized if sharing with display;
    // spi_bus_initialize returns ESP_ERR_INVALID_STATE if already up — ignore that)
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = pin_mosi,
        .miso_io_num     = pin_miso,
        .sclk_io_num     = pin_sclk,
        .quadhd_io_num   = -1,
        .quadwp_io_num   = -1,
        .max_transfer_sz = 16,
    };
    esp_err_t bus_ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_DISABLED);
    if (bus_ret != ESP_OK && bus_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(bus_ret));
        return bus_ret;
    }

    // Add XPT2046 as a device on the bus
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 2 * 1000 * 1000,   // XPT2046 max ~2.5 MHz
        .mode           = 0,
        .spics_io_num   = pin_cs,
        .queue_size     = 1,
        .flags          = 0,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi),
                        TAG, "SPI add device failed");

    // Configure IRQ pin as input (active-low, no pull needed — XPT has its own)
    if (s_irq != 0xFF) {
        gpio_config_t irq_cfg = {
            .pin_bit_mask = (1ULL << s_irq),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&irq_cfg), TAG, "IRQ gpio config failed");
    }

    s_initialized = true;
    ESP_LOGI(TAG, "XPT2046 initialized (CS=%d, IRQ=%d)", pin_cs, s_irq);
    purr_kernel_register_touch(&s_catcall);
    return ESP_OK;
}

static bool xpt2046_is_pressed(void)
{
    if (!s_initialized) return false;

    // If IRQ pin is configured, use it (active low = touched)
    if (s_irq != 0xFF) {
        return gpio_get_level(s_irq) == 0;
    }

    // Fallback: check raw Y reading above threshold
    uint16_t raw_y = xpt_read_raw(XPT_CMD_Y);
    return raw_y > XPT_TOUCH_THRESHOLD;
}

static bool xpt2046_read_point(uint16_t *x, uint16_t *y)
{
    if (!s_initialized || !x || !y) return false;
    if (!xpt2046_is_pressed()) return false;

    uint16_t raw_x = xpt_sample_avg(XPT_CMD_X);
    uint16_t raw_y = xpt_sample_avg(XPT_CMD_Y);

    // Sanity check — discard if below threshold (pen lift during sampling)
    if (raw_x < XPT_TOUCH_THRESHOLD || raw_y < XPT_TOUCH_THRESHOLD) return false;

    *x = xpt_map(raw_x, XPT_RAW_X_MIN, XPT_RAW_X_MAX, XPT_SCREEN_W - 1);
    *y = xpt_map(raw_y, XPT_RAW_Y_MIN, XPT_RAW_Y_MAX, XPT_SCREEN_H - 1);
    return true;
}

static esp_err_t xpt2046_deinit(void)
{
    if (!s_initialized) return ESP_OK;
    spi_bus_remove_device(s_spi);
    s_spi         = NULL;
    s_initialized = false;
    return ESP_OK;
}

// ── Static catcall descriptor ─────────────────────────────────────────────────

static const catcall_touch_t s_catcall = {
    .name            = "xpt2046",
    .catcall_version = CATCALL_TOUCH_VERSION,
    .init            = xpt2046_init,
    .read_point      = xpt2046_read_point,
    .is_pressed      = xpt2046_is_pressed,
    .deinit          = xpt2046_deinit,
};

// ── Module lifecycle ──────────────────────────────────────────────────────────

static int xpt2046_drv_init(void)
{
    // Default CYD pin config
    touch_config_t default_cfg = {
        .i2c_port = 0,                  // unused for SPI
        .sda_pin  = XPT_DEFAULT_MISO,   // MISO
        .scl_pin  = XPT_DEFAULT_SCLK,   // SCLK
        .int_pin  = XPT_DEFAULT_CS,     // CS
        .rst_pin  = XPT_DEFAULT_IRQ,    // IRQ
    };
    esp_err_t ret = xpt2046_init(&default_cfg);
    return (ret == ESP_OK) ? 0 : -1;
}

static void xpt2046_drv_deinit(void)
{
    xpt2046_deinit();
}

// ── PURR module header ────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(xpt2046) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .load_priority     = PURR_PRIORITY_REQUIRED,
    .name              = "xpt2046",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = CATCALL_FLAG_TOUCH,
    .required_catcalls = 0,
    .init              = xpt2046_drv_init,
    .deinit            = xpt2046_drv_deinit,
};
