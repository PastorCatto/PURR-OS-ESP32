// gt911.c — GT911 capacitive touch driver for PURR OS
// Interface: I2C, address 0x5D (primary) or 0x14 (alternate)
//
// Used on:
//   tdeck_plus — SDA=18, SCL=8, INT=16, RST=38
//
// Register map:
//   0x814E = touch status (bits[3:0] = touch count; write 0 to clear)
//   0x8150 = first touch point data (5 bytes):
//            byte0: track_id
//            byte1: x_low
//            byte2: x_high
//            byte3: y_low
//            byte4: y_high
//
// INT pin:
//   0xFF or -1  → polling mode (reads status register each call)
//   valid GPIO  → falling-edge ISR sets data-ready flag
//
// RST pin:
//   0xFF or -1  → not connected, skip reset pulse
//   valid GPIO  → pulse low 20ms then high during init
//
// I2C: 400 kHz
// ESP-IDF v5.x (i2c_master_* APIs)

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#include "../../kernel/catcalls/catcall_touch.h"
#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_kernel.h"
#include "purr_quirk.h"

static const char *TAG = "gt911";

// Layout for a "gt911.pins" .purr v2 quirk block — see purr_quirk_pkg.h.
// Plain int32_t fields matching gt911_configure()'s own parameter list —
// a generator producing this block must match this exact layout.
typedef struct {
    int32_t sda;
    int32_t scl;
    int32_t int_pin;
    int32_t rst_pin;
    int32_t i2c_port;
} gt911_pin_quirk_t;

// ── Forward declarations ──────────────────────────────────────────────────────

static const catcall_touch_t s_catcall;
static esp_err_t gt911_init(const touch_config_t *cfg);
static bool      gt911_read_point(uint16_t *x, uint16_t *y);
static bool      gt911_is_pressed(void);
static esp_err_t gt911_deinit(void);

// ── Constants ─────────────────────────────────────────────────────────────────

#define GT911_I2C_ADDR_PRIMARY   0x5D
#define GT911_I2C_ADDR_ALT       0x14
#define GT911_I2C_FREQ_HZ        400000

// T-Deck Plus panel — the only device currently using this driver.
#define GT911_LCD_WIDTH          320
#define GT911_LCD_HEIGHT         240

#define GT911_REG_STATUS         0x814E
#define GT911_REG_POINT1         0x8150

#define GT911_STATUS_TOUCH_MASK  0x0F
#define GT911_STATUS_BUFFER_STAT 0x80

// Main checksummed config block. X/Y_OUTPUT_MAX live inside it at fixed
// offsets — this is the chip's own internal idea of panel resolution,
// independent of whatever the host later does with the raw coordinates.
// On this unit it's configured for portrait (240x320), not the landscape
// 320x240 panel actually in use — confirmed by the wraparound happening at
// exactly 3/4 of the screen width (320 * 3/4 = 240). Writing the correct
// resolution here requires the whole block: a single register poke is
// silently ignored unless the checksum (0x80FF) is recomputed and the
// "config fresh" flag (0x8100) is set, telling the chip to reload.
#define GT911_CONFIG_START      0x8047
#define GT911_CONFIG_SIZE       184   // 0x8047..0x80FE inclusive
#define GT911_REG_CONFIG_CHKSUM 0x80FF
#define GT911_REG_CONFIG_FRESH  0x8100
#define GT911_CFG_OFS_X_MAX_L   1     // 0x8048 - 0x8047
#define GT911_CFG_OFS_X_MAX_H   2
#define GT911_CFG_OFS_Y_MAX_L   3     // 0x804A - 0x8047
#define GT911_CFG_OFS_Y_MAX_H   4

// Command register — periodically re-writing 0x00 (normal scan mode) keeps
// the chip reporting touches. Without this it stops updating its
// status/point registers after the first touch (observed: touch works
// once, then goes dead) — same fix already proven necessary on the
// Arduino-kernel GT911 driver (kernel_atdp_boot.cpp), just never ported to
// this native i2c_master driver until now.
#define GT911_REG_COMMAND       0x8040
#define GT911_KEEPALIVE_MS      2000

// Default pins (tdeck_plus)
#define GT911_DEFAULT_SDA    18
#define GT911_DEFAULT_SCL     8
#define GT911_DEFAULT_INT    16
#define GT911_DEFAULT_RST    38

// Override set by gt911_configure() before drv_init
static int s_cfg_sda      = GT911_DEFAULT_SDA;
static int s_cfg_scl      = GT911_DEFAULT_SCL;
static int s_cfg_int      = GT911_DEFAULT_INT;
static int s_cfg_rst      = GT911_DEFAULT_RST;
static int s_cfg_i2c_port = 0;

// ── Touch point struct ────────────────────────────────────────────────────────

// No leading track_id byte here — confirmed against both 0.11's
// hal_touch.cpp (which reads X directly at offset 0 from this same
// register) and the official GT911 register map: track_id actually lives
// at 0x814F, one byte *before* GT911_REG_POINT1 (0x8150) — so a read
// starting at 0x8150 is already sitting on x_low, with no byte to skip.
// This driver previously assumed a track_id byte here, shifting every
// field over by one and corrupting both axes (Y the worst, since it
// landed on x_high/y_low instead of y_low/y_high).
typedef struct __attribute__((packed)) {
    uint8_t  x_low;
    uint8_t  x_high;
    uint8_t  y_low;
    uint8_t  y_high;
} gt911_point_t;

// ── Module state ──────────────────────────────────────────────────────────────

static i2c_master_bus_handle_t  s_bus_handle  = NULL;
static i2c_master_dev_handle_t  s_dev_handle  = NULL;
static int                      s_int_pin     = -1;
static int                      s_rst_pin     = -1;
static volatile bool            s_data_ready  = false;
static bool                     s_poll_mode   = true;
static bool                     s_initialized = false;
static uint32_t                 s_keepalive_at = 0;
static bool                     s_have_last_point = false;
static uint8_t                  s_settle_count = 0;
static uint16_t                 s_last_true_raw_x = 0, s_last_true_raw_y = 0;
static uint16_t                 s_last_x = 0, s_last_y = 0;
static bool                      s_resolution_fixed = false;

// ── GPIO ISR ──────────────────────────────────────────────────────────────────

static void IRAM_ATTR gt911_int_isr(void *arg)
{
    s_data_ready = true;
}

// ── I2C helpers ───────────────────────────────────────────────────────────────

static esp_err_t gt911_write_reg(uint16_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[2 + len];
    buf[0] = (uint8_t)(reg >> 8);
    buf[1] = (uint8_t)(reg & 0xFF);
    memcpy(&buf[2], data, len);
    return i2c_master_transmit(s_dev_handle, buf, sizeof(buf), pdMS_TO_TICKS(50));
}

static esp_err_t gt911_read_reg(uint16_t reg, uint8_t *data, size_t len)
{
    uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return i2c_master_transmit_receive(s_dev_handle, addr, sizeof(addr),
                                       data, len, pdMS_TO_TICKS(50));
}

static esp_err_t gt911_clear_status(void)
{
    uint8_t zero = 0x00;
    return gt911_write_reg(GT911_REG_STATUS, &zero, 1);
}

// Rewrite the chip's own X/Y_OUTPUT_MAX config to match this panel
// (320x240 landscape) instead of whatever portrait/wrong value it shipped
// with — recomputing the checksum and setting the config-fresh flag so the
// chip actually reloads it, rather than silently ignoring an isolated
// register poke. Logs the before/after resolution either way.
static void gt911_fix_resolution(void)
{
    uint8_t cfg[GT911_CONFIG_SIZE];
    if (gt911_read_reg(GT911_CONFIG_START, cfg, sizeof(cfg)) != ESP_OK) {
        ESP_LOGW(TAG, "could not read config block — skipping resolution fix");
        return;
    }

    uint16_t old_x_max = (uint16_t)cfg[GT911_CFG_OFS_X_MAX_L] |
                         ((uint16_t)cfg[GT911_CFG_OFS_X_MAX_H] << 8);
    uint16_t old_y_max = (uint16_t)cfg[GT911_CFG_OFS_Y_MAX_L] |
                         ((uint16_t)cfg[GT911_CFG_OFS_Y_MAX_H] << 8);

    if (old_x_max == GT911_LCD_WIDTH && old_y_max == GT911_LCD_HEIGHT) {
        ESP_LOGI(TAG, "config X/Y_OUTPUT_MAX already %ux%u — no fix needed",
                 old_x_max, old_y_max);
        s_resolution_fixed = true;
        return;
    }

    cfg[GT911_CFG_OFS_X_MAX_L] = (uint8_t)(GT911_LCD_WIDTH & 0xFF);
    cfg[GT911_CFG_OFS_X_MAX_H] = (uint8_t)((GT911_LCD_WIDTH >> 8) & 0xFF);
    cfg[GT911_CFG_OFS_Y_MAX_L] = (uint8_t)(GT911_LCD_HEIGHT & 0xFF);
    cfg[GT911_CFG_OFS_Y_MAX_H] = (uint8_t)((GT911_LCD_HEIGHT >> 8) & 0xFF);

    uint8_t sum = 0;
    for (size_t i = 0; i < sizeof(cfg); i++) sum = (uint8_t)(sum + cfg[i]);
    uint8_t checksum = (uint8_t)((~sum) + 1); // two's complement, sum+checksum == 0 mod 256

    esp_err_t e1 = gt911_write_reg(GT911_CONFIG_START, cfg, sizeof(cfg));
    uint8_t chk_buf = checksum;
    esp_err_t e2 = gt911_write_reg(GT911_REG_CONFIG_CHKSUM, &chk_buf, 1);
    uint8_t fresh = 0x01;
    esp_err_t e3 = gt911_write_reg(GT911_REG_CONFIG_FRESH, &fresh, 1);

    if (e1 == ESP_OK && e2 == ESP_OK && e3 == ESP_OK) {
        ESP_LOGI(TAG, "config X/Y_OUTPUT_MAX corrected %ux%u -> %ux%u",
                 old_x_max, old_y_max, GT911_LCD_WIDTH, GT911_LCD_HEIGHT);
        s_resolution_fixed = true;
    } else {
        ESP_LOGW(TAG, "config write failed (block=%s chk=%s fresh=%s)",
                 esp_err_to_name(e1), esp_err_to_name(e2), esp_err_to_name(e3));
    }
}

static void gt911_keepalive(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_keepalive_at != 0 && now < s_keepalive_at) return;
    s_keepalive_at = now + GT911_KEEPALIVE_MS;
    uint8_t zero = 0x00;
    gt911_write_reg(GT911_REG_COMMAND, &zero, 1);
}

// ── catcall_touch_t implementation ────────────────────────────────────────────

static esp_err_t gt911_init(const touch_config_t *cfg)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    int sda   = cfg ? cfg->sda_pin  : GT911_DEFAULT_SDA;
    int scl   = cfg ? cfg->scl_pin  : GT911_DEFAULT_SCL;
    s_int_pin = cfg ? (cfg->int_pin != 0xFF ? (int)cfg->int_pin : -1) : GT911_DEFAULT_INT;
    s_rst_pin = cfg ? (cfg->rst_pin != 0xFF ? (int)cfg->rst_pin : -1) : GT911_DEFAULT_RST;
    int port  = cfg ? cfg->i2c_port : 0;

    s_poll_mode = (s_int_pin < 0);

    // RST pulse if pin is connected
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
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(s_rst_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // I2C master bus
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = (i2c_port_num_t)port,
        .sda_io_num        = sda,
        .scl_io_num        = scl,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus_handle),
                        TAG, "I2C bus init failed");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = GT911_I2C_ADDR_PRIMARY,
        .scl_speed_hz    = GT911_I2C_FREQ_HZ,
    };
    i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &s_dev_handle);

    // Read product ID to verify device is alive — log result either way
    uint8_t pid[4] = {0};
    esp_err_t probe = gt911_read_reg(0x8140, pid, sizeof(pid));
    if (probe == ESP_OK) {
        ESP_LOGI(TAG, "GT911 at 0x5D — ID: %.4s", pid);
    } else {
        ESP_LOGW(TAG, "0x5D read ID: %s — trying 0x14", esp_err_to_name(probe));
        i2c_master_bus_rm_device(s_dev_handle);
        s_dev_handle = NULL;
        dev_cfg.device_address = GT911_I2C_ADDR_ALT;
        i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &s_dev_handle);
        probe = gt911_read_reg(0x8140, pid, sizeof(pid));
        if (probe == ESP_OK) {
            ESP_LOGI(TAG, "GT911 at 0x14 — ID: %.4s", pid);
        } else {
            ESP_LOGE(TAG, "GT911 not found at 0x5D or 0x14 (%s)", esp_err_to_name(probe));
            i2c_master_bus_rm_device(s_dev_handle);
            i2c_del_master_bus(s_bus_handle);
            s_dev_handle = NULL;
            s_bus_handle = NULL;
            return probe;
        }
    }

    gt911_clear_status();
    gt911_fix_resolution();

    // INT pin setup
    if (!s_poll_mode) {
        gpio_config_t int_cfg = {
            .pin_bit_mask = (1ULL << s_int_pin),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_NEGEDGE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&int_cfg), TAG, "INT gpio config failed");
        gpio_install_isr_service(0);
        gpio_isr_handler_add((gpio_num_t)s_int_pin, gt911_int_isr, NULL);
    }

    s_initialized = true;
    ESP_LOGI(TAG, "GT911 initialized (SDA=%d, SCL=%d, INT=%d, RST=%d, poll=%d)",
             sda, scl, s_int_pin, s_rst_pin, s_poll_mode);
    purr_kernel_register_touch(&s_catcall);
    return ESP_OK;
}

static bool gt911_is_pressed(void)
{
    if (!s_initialized) return false;
    gt911_keepalive();

    if (!s_poll_mode)
        return gpio_get_level(s_int_pin) == 0;

    uint8_t status = 0;
    if (gt911_read_reg(GT911_REG_STATUS, &status, 1) != ESP_OK) return false;
    if (!(status & GT911_STATUS_BUFFER_STAT)) return false;

    bool down = (status & GT911_STATUS_TOUCH_MASK) > 0;
    if (!down) {
        // Buffer-ready bit set with a 0 touch count is GT911's "release"
        // event. It must be cleared here or the buffer-ready flag stays
        // latched and the chip never reports the next touch at all (this
        // was "works once while held, then permanently dead" — the chip
        // thinks the host hasn't consumed the release report, so it never
        // generates a new one). read_point() already clears this on its
        // own release path, but is_pressed() is checked first and was
        // returning early without clearing — same fix already proven
        // necessary on the Arduino-kernel GT911 driver.
        gt911_clear_status();
        s_have_last_point = false;
        s_settle_count = 0;
    }
    return down;
}

static bool gt911_read_point(uint16_t *x, uint16_t *y)
{
    if (!s_initialized || !x || !y) return false;
    gt911_keepalive();

    if (!s_poll_mode && !s_data_ready) {
        if (gpio_get_level(s_int_pin) != 0) return false;
    }

    uint8_t status = 0;
    if (gt911_read_reg(GT911_REG_STATUS, &status, 1) != ESP_OK) return false;

    if (!(status & GT911_STATUS_BUFFER_STAT)) return false;
    uint8_t touch_count = status & GT911_STATUS_TOUCH_MASK;
    if (touch_count == 0) {
        gt911_clear_status();
        s_data_ready = false;
        s_have_last_point = false;
        s_settle_count = 0;
        return false;
    }

    gt911_point_t pt;
    esp_err_t ret = gt911_read_reg(GT911_REG_POINT1, (uint8_t *)&pt, sizeof(pt));
    if (ret != ESP_OK) {
        gt911_clear_status();
        s_data_ready = false;
        return false;
    }

    // This unit's GT911 does NOT report the commonly-assumed 0-4095 — raw
    // capture showed X spanning close to the full 16-bit range and Y a much
    // smaller ~1280-10240ish range, with no axis swap (a clean horizontal
    // swipe held Y constant while X swept tens of thousands of units).
    // Scale to actual screen pixels here, in the driver, once — rather than
    // every UI HAL re-deriving the same per-axis math. Hard-clamped so a
    // raw value beyond the assumed max can never overshoot past the display
    // edge (the earlier "cursor wraps past the edge" bug was exactly this:
    // an unclamped scaled value briefly exceeding the screen before the
    // caller's own clamp ran).
    uint16_t raw_x = (uint16_t)pt.x_low | ((uint16_t)pt.x_high << 8);
    uint16_t raw_y = (uint16_t)pt.y_low | ((uint16_t)pt.y_high << 8);
    s_last_true_raw_x = raw_x;
    s_last_true_raw_y = raw_y;

    // PURR OS 0.11's hal_touch.cpp (devices/tdeck_plus) cast these bytes to
    // SIGNED int16_t, not unsigned — a real difference from this "plug and
    // play" driver, found by direct comparison rather than guessing.
    // Interpreting the same bits as signed means an internal coordinate
    // counter overflow (past 32767) naturally lands as a small *negative*
    // number — "just past the edge, still going the same direction" — and
    // clamps cleanly to 0. Unsigned interpretation turns that exact same
    // overflow into a small *positive* number near zero instead, which
    // looks exactly like the "resets and climbs again" bug we've been
    // chasing. Same per-axis ranges as before, just signed.
    int16_t signed_x = (int16_t)raw_x;
    int16_t signed_y = (int16_t)raw_y;

    // Confirmed on hardware: no-swap reads back as the screen rotated 90
    // degrees, and a swap+mirror with no rescale capped one axis short of
    // the real edge. Both pieces are needed together: LilyGo's reference
    // rotation (swap, mirror the post-swap Y) *and* a proper rescale,
    // since the two raw fields' native ranges (signed_x ~0-311, signed_y
    // ~0-235) don't match the screen dimension they each end up driving
    // after the swap.
    #define GT911_NATIVE_X_MAX 311
    #define GT911_NATIVE_Y_MAX 235
    int32_t sx = ((int32_t)signed_y * GT911_LCD_WIDTH) / GT911_NATIVE_Y_MAX;
    int32_t sy_raw = ((int32_t)signed_x * GT911_LCD_HEIGHT) / GT911_NATIVE_X_MAX;
    int32_t sy = (GT911_LCD_HEIGHT - 1) - sy_raw;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx >= GT911_LCD_WIDTH)  sx = GT911_LCD_WIDTH  - 1;
    if (sy >= GT911_LCD_HEIGHT) sy = GT911_LCD_HEIGHT - 1;

    // No filtering, no jump-rejection, no settle-counting — 0.11 never had
    // any of this and worked fine; that extra logic was mine, added during
    // this debugging session, and it introduced its own new bugs (capped
    // movement speed) on top of whatever the real underlying issue is.
    // Report exactly what the chip says, every time, like 0.11 did.
    *x = (uint16_t)sx;
    *y = (uint16_t)sy;
    s_last_x = (uint16_t)sx;
    s_last_y = (uint16_t)sy;
    s_have_last_point = true;

    gt911_clear_status();
    s_data_ready = false;
    return true;
}

void gt911_get_last_true_raw(uint16_t *x, uint16_t *y)
{
    if (x) *x = s_last_true_raw_x;
    if (y) *y = s_last_true_raw_y;
}

static esp_err_t gt911_deinit(void)
{
    if (!s_initialized) return ESP_OK;

    if (!s_poll_mode && s_int_pin >= 0)
        gpio_isr_handler_remove((gpio_num_t)s_int_pin);
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
    .name            = "gt911",
    .catcall_version = CATCALL_TOUCH_VERSION,
    .init            = gt911_init,
    .read_point      = gt911_read_point,
    .is_pressed      = gt911_is_pressed,
    .deinit          = gt911_deinit,
};

// ── Module lifecycle ──────────────────────────────────────────────────────────

void gt911_configure(int sda, int scl, int int_pin, int rst_pin, int i2c_port)
{
    // A loaded .purr v2 quirk package's "gt911.pins" block, if present and
    // correctly sized, overrides the CALLER's own values — self-contained,
    // no change needed at any kernel_*_boot.c call site (every one of them
    // currently passes hardcoded literals; a quirk package is simply the
    // more specific, more current override, same precedence
    // adc_battery_configure()'s own quirk check already uses).
    size_t qsz = 0;
    const void *qblk = purr_quirk_get_block("gt911.pins", &qsz);
    if (qblk && qsz == sizeof(gt911_pin_quirk_t)) {
        const gt911_pin_quirk_t *q = (const gt911_pin_quirk_t *)qblk;
        sda = q->sda; scl = q->scl; int_pin = q->int_pin;
        rst_pin = q->rst_pin; i2c_port = q->i2c_port;
        ESP_LOGI(TAG, "using loaded quirk package's pin assignment instead of caller's "
                      "(sda=%d scl=%d int=%d rst=%d port=%d)", sda, scl, int_pin, rst_pin, i2c_port);
    } else if (qblk) {
        ESP_LOGW(TAG, "quirk block 'gt911.pins' has wrong size (%u, expected %u) — ignoring",
                 (unsigned)qsz, (unsigned)sizeof(gt911_pin_quirk_t));
    }

    s_cfg_sda      = sda;
    s_cfg_scl      = scl;
    s_cfg_int      = int_pin;
    s_cfg_rst      = rst_pin;
    s_cfg_i2c_port = i2c_port;
}

int gt911_drv_init(void)
{
    touch_config_t cfg = {
        .i2c_port = s_cfg_i2c_port,
        .sda_pin  = (uint8_t)s_cfg_sda,
        .scl_pin  = (uint8_t)s_cfg_scl,
        .int_pin  = (uint8_t)(s_cfg_int < 0 ? 0xFF : s_cfg_int),
        .rst_pin  = (uint8_t)(s_cfg_rst < 0 ? 0xFF : s_cfg_rst),
    };
    esp_err_t ret = gt911_init(&cfg);
    return (ret == ESP_OK) ? 0 : -1;
}

void gt911_drv_deinit(void)
{
    gt911_deinit();
}

// ── PURR module header ────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(gt911) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .load_priority     = PURR_PRIORITY_REQUIRED,
    .name              = "gt911",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = CATCALL_FLAG_TOUCH,
    .required_catcalls = 0,
    .init              = gt911_drv_init,
    .deinit            = gt911_drv_deinit,
};
