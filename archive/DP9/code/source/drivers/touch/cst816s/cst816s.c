// cst816s.c — CST816S capacitive touch driver for PURR OS
// Interface: I2C, address 0x15
//
// Used on:
//   cyd_s024c  — SDA=33, SCL=32, INT=21, RST=25
//   waveshare169 — similar wiring
//
// Register map:
//   0x01 = gesture code
//   0x02 = finger count
//   0x03 = X high byte  [3:0] = X[11:8]
//   0x04 = X low  byte         X[7:0]
//   0x05 = Y high byte  [3:0] = Y[11:8]
//   0x06 = Y low  byte         Y[7:0]
//
// RST pin: pulsed low 10ms then high during init
// INT pin: active-low, falling edge signals new data. We support both
//          interrupt-driven and polling modes.
//
// ESP-IDF v5.x (i2c_master_* APIs from driver/i2c_master.h)

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#include "../../kernel/catcalls/catcall_touch.h"
#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_kernel.h"

static const char *TAG = "cst816s";

// ── Forward declarations ──────────────────────────────────────────────────────

static const catcall_touch_t s_catcall;
static esp_err_t cst816s_init(const touch_config_t *cfg);
static bool      cst816s_read_point(uint16_t *x, uint16_t *y);
static bool      cst816s_is_pressed(void);
static esp_err_t cst816s_deinit(void);

// ── Constants ─────────────────────────────────────────────────────────────────

#define CST816S_I2C_ADDR     0x15
#define CST816S_I2C_FREQ_HZ  400000

#define CST816S_REG_GESTURE  0x01
#define CST816S_REG_FINGERS  0x02
#define CST816S_REG_XH       0x03   // read 4 bytes starting here: XH, XL, YH, YL

// Default pins (cyd_s024c)
#define CST_DEFAULT_SDA   33
#define CST_DEFAULT_SCL   32
#define CST_DEFAULT_INT   21
#define CST_DEFAULT_RST   25

// ── Module state ──────────────────────────────────────────────────────────────

static i2c_master_bus_handle_t    s_bus_handle = NULL;
static i2c_master_dev_handle_t    s_dev_handle = NULL;
static int                        s_int_pin    = -1;
static int                        s_rst_pin    = -1;
static volatile bool              s_data_ready = false;
static bool                       s_initialized = false;

// ── GPIO ISR for INT pin ──────────────────────────────────────────────────────

static void IRAM_ATTR cst816s_int_isr(void *arg)
{
    s_data_ready = true;
}

// ── I2C helpers ───────────────────────────────────────────────────────────────

static esp_err_t cst816s_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev_handle, &reg, 1, data, len,
                                       pdMS_TO_TICKS(50));
}

// ── catcall_touch_t implementation ────────────────────────────────────────────

static esp_err_t cst816s_init(const touch_config_t *cfg)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    int sda  = cfg ? cfg->sda_pin  : CST_DEFAULT_SDA;
    int scl  = cfg ? cfg->scl_pin  : CST_DEFAULT_SCL;
    s_int_pin = cfg ? (cfg->int_pin != 0xFF ? (int)cfg->int_pin : -1) : CST_DEFAULT_INT;
    s_rst_pin = cfg ? (cfg->rst_pin != 0xFF ? (int)cfg->rst_pin : -1) : CST_DEFAULT_RST;
    int port  = cfg ? cfg->i2c_port : 0;

    // RST pulse: hold low 10ms then release high to reset the chip
    if (s_rst_pin >= 0) {
        gpio_config_t rst_cfg = {
            .pin_bit_mask = (1ULL << s_rst_pin),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), TAG, "RST gpio config failed");
        gpio_set_level(s_rst_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(s_rst_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(50));   // CST816S boot time
    }

    // I2C master bus
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port        = (i2c_port_num_t)port,
        .sda_io_num      = sda,
        .scl_io_num      = scl,
        .clk_source      = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus_handle),
                        TAG, "I2C bus init failed");

    // Add CST816S device
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = CST816S_I2C_ADDR,
        .scl_speed_hz    = CST816S_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &s_dev_handle),
                        TAG, "I2C add device failed");

    // INT pin: falling-edge ISR
    if (s_int_pin >= 0) {
        gpio_config_t int_cfg = {
            .pin_bit_mask = (1ULL << s_int_pin),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_NEGEDGE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&int_cfg), TAG, "INT gpio config failed");
        gpio_install_isr_service(0);
        gpio_isr_handler_add((gpio_num_t)s_int_pin, cst816s_int_isr, NULL);
    }

    s_initialized = true;
    ESP_LOGI(TAG, "CST816S initialized (SDA=%d, SCL=%d, INT=%d, RST=%d)",
             sda, scl, s_int_pin, s_rst_pin);
    purr_kernel_register_touch(&s_catcall);
    return ESP_OK;
}

static bool cst816s_is_pressed(void)
{
    if (!s_initialized) return false;

    // Fast path: use INT pin level (active low)
    if (s_int_pin >= 0) {
        return gpio_get_level(s_int_pin) == 0;
    }

    // Polling: read finger count register
    uint8_t fingers = 0;
    if (cst816s_read_regs(CST816S_REG_FINGERS, &fingers, 1) != ESP_OK) return false;
    return fingers > 0;
}

static bool cst816s_read_point(uint16_t *x, uint16_t *y)
{
    if (!s_initialized || !x || !y) return false;

    // Read 6 bytes starting at 0x01: gesture, fingers, XH, XL, YH, YL
    uint8_t buf[6];
    if (cst816s_read_regs(CST816S_REG_GESTURE, buf, sizeof(buf)) != ESP_OK) return false;

    // buf[1] = finger count
    if (buf[1] == 0) return false;

    // buf[2]=XH, buf[3]=XL — lower 4 bits of XH are X[11:8]
    // buf[4]=YH, buf[5]=YL — lower 4 bits of YH are Y[11:8]
    *x = (uint16_t)((buf[2] & 0x0F) << 8) | buf[3];
    *y = (uint16_t)((buf[4] & 0x0F) << 8) | buf[5];

    // Clear the data-ready flag set by ISR
    s_data_ready = false;
    return true;
}

static esp_err_t cst816s_deinit(void)
{
    if (!s_initialized) return ESP_OK;

    if (s_int_pin >= 0) {
        gpio_isr_handler_remove((gpio_num_t)s_int_pin);
    }
    if (s_dev_handle) {
        i2c_master_bus_rm_device(s_dev_handle);
        s_dev_handle = NULL;
    }
    if (s_bus_handle) {
        i2c_del_master_bus(s_bus_handle);
        s_bus_handle = NULL;
    }
    s_initialized = false;
    return ESP_OK;
}

// ── Static catcall descriptor ─────────────────────────────────────────────────

static const catcall_touch_t s_catcall = {
    .name            = "cst816s",
    .catcall_version = CATCALL_TOUCH_VERSION,
    .init            = cst816s_init,
    .read_point      = cst816s_read_point,
    .is_pressed      = cst816s_is_pressed,
    .deinit          = cst816s_deinit,
};

// ── Module lifecycle ──────────────────────────────────────────────────────────

static int cst816s_drv_init(void)
{
    touch_config_t default_cfg = {
        .i2c_port = 0,
        .sda_pin  = CST_DEFAULT_SDA,
        .scl_pin  = CST_DEFAULT_SCL,
        .int_pin  = CST_DEFAULT_INT,
        .rst_pin  = CST_DEFAULT_RST,
    };
    esp_err_t ret = cst816s_init(&default_cfg);
    return (ret == ESP_OK) ? 0 : -1;
}

static void cst816s_drv_deinit(void)
{
    cst816s_deinit();
}

// ── PURR module header ────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(cst816s) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .load_priority     = PURR_PRIORITY_REQUIRED,
    .name              = "cst816s",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = CATCALL_FLAG_TOUCH,
    .required_catcalls = 0,
    .init              = cst816s_drv_init,
    .deinit            = cst816s_drv_deinit,
};
