// m5tab5_bsp.c — M5Stack Tab5 display+touch+SD, native driver.
//
// Replaces an earlier espp/m5stack-tab5-wrapped C++ version: that dependency
// was dropped because (1) espp/display_drivers hard-requires lvgl>=9.2.2
// even at the low-level transport layer, and (2) espp/logger transitively
// requires esp_log_timestamp.h, which does not exist in this project's
// pinned ESP-IDF v5.3.5 — a second, independent blocker. Both go away by
// talking to the hardware directly instead of through espp's C++ objects.
//
// Every hardware fact below (I2C detection addresses, IO-expander register
// map + bit assignments, MIPI-DSI/DPI timing, DCS init command tables) was
// sourced from espp's real, hardware-tested source (esp-cpp/espp, MIT
// licensed, cloned read-only for reference — never a build dependency) and
// cross-checked byte-for-byte against M5Stack's own official M5GFX
// (m5stack/M5GFX, Panel_ILI9881C.hpp / Panel_ST7123.hpp) — not guessed.
// The two sources agree exactly on every register write in both DCS init
// tables. The one place they diverge is the final power-on tail: this file
// uses espp's ordering (SLPOUT, then DISPON, then TE_ON — the standard MIPI
// DCS sequence, and espp's version carries a hardware-specific comment
// about the ST7123's touch-scan timing suggesting real board bring-up).
// M5GFX instead sends DISPON+TE_ON first and SLPOUT last with a single
// 120ms delay after. If the panel doesn't come up on real hardware, that's
// the first thing to try swapping (see st7123_tail[] below).
//
// No physical Tab5 in hand yet — this file is build-verified only.

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#include "../../../kernel/core/purr_kernel.h"
#include "../../../kernel/catcalls/catcall_display.h"
#include "../../../kernel/catcalls/catcall_touch.h"
#include "m5tab5_bsp.h"

static const char *TAG = "m5tab5_bsp";

// ── Panel geometry ───────────────────────────────────────────────────────
// The DPI interface scans this panel as 720 wide x 1280 tall (its native
// gate/source driver orientation) — the panel is physically mounted rotated
// in the Tab5 enclosure so this reads as a landscape 720x1280 image, not a
// portrait one. MADCTL is left at its base/identity value (0x00, no
// mirror/swap) to match — confirmed against espp's config (mirror_x/
// mirror_y/swap_xy all false for the "LANDSCAPE" rotation it ships with).
#define PANEL_WIDTH  720
#define PANEL_HEIGHT 1280

// ── Pins (source/devices/tab5/device.pcat) ──────────────────────────────
#define I2C_PORT        0
#define I2C_SDA_PIN     31
#define I2C_SCL_PIN     32
#define TOUCH_INT_PIN   23
#define BACKLIGHT_PIN   22

#define SD_CLK_PIN      43
#define SD_CMD_PIN      44
#define SD_DAT0_PIN     39
#define SD_DAT1_PIN     40
#define SD_DAT2_PIN     41
#define SD_DAT3_PIN     42

// ── IO-expander (PI4IOE5V6408, two instances) ───────────────────────────
// LCD_RST/TP_RST/SPK_EN are NOT direct GPIO — routed through this I2C
// expander. Register map + bit assignments confirmed against espp's
// pi4ioe5v.hpp (matches the public PI4IOE5V6408 datasheet) and
// m5stack-tab5.hpp's own pin table.
#define IOEXP_ADDR_0x43   0x43
#define IOEXP_ADDR_0x44   0x44

enum {
    IOEXP_REG_CHIP_ID_CTRL = 0x01,
    IOEXP_REG_DIRECTION    = 0x03,   // 0=input, 1=output
    IOEXP_REG_OUTPUT       = 0x05,
    IOEXP_REG_OUTPUT_HIZ   = 0x07,   // 1 = high-Z
    IOEXP_REG_INPUT_DEF    = 0x09,
    IOEXP_REG_PULL_ENABLE  = 0x0B,
    IOEXP_REG_PULL_SELECT  = 0x0D,   // 1=pull-up, 0=pull-down
    IOEXP_REG_INPUT        = 0x0F,
    IOEXP_REG_INT_MASK     = 0x11,   // 1=disabled
};

// 0x43: SPK_EN=P1, EXT_5V_EN=P2, LCD_RST=P4, TP_RST=P5, CAM_RST=P6(unused), HP_DET=P7(input)
#define IO43_BIT_SPK_EN     (1 << 1)
#define IO43_BIT_EXT_5V_EN  (1 << 2)
#define IO43_BIT_CAM_RST    (1 << 6)
#define IO43_BIT_LCD_RST    (1 << 4)
#define IO43_BIT_TP_RST     (1 << 5)
#define IO43_BIT_HP_DET     (1 << 7)
#define IOX_0x43_OUTPUTS    (IO43_BIT_SPK_EN | IO43_BIT_EXT_5V_EN | IO43_BIT_CAM_RST | IO43_BIT_LCD_RST | IO43_BIT_TP_RST)

// 0x44: WLAN_PWR_EN=P0, PWROFF_PULSE=P4, N_CHG_QC_EN=P5, CHG_STAT=P6(input), CHG_EN=P7
#define IO44_BIT_WLAN_PWR_EN   (1 << 0)
#define IO44_BIT_USB_5V_EN     (1 << 3)
#define IO44_BIT_PWROFF_PULSE  (1 << 4)
#define IO44_BIT_N_CHG_QC_EN   (1 << 5)
#define IO44_BIT_CHG_STAT      (1 << 6)
#define IO44_BIT_CHG_EN        (1 << 7)
#define IOX_0x44_OUTPUTS       (IO44_BIT_WLAN_PWR_EN | IO44_BIT_USB_5V_EN | IO44_BIT_PWROFF_PULSE | IO44_BIT_N_CHG_QC_EN | IO44_BIT_CHG_EN)
#define IOX_0x44_HIGH_Z        ((1 << 2) | (1 << 1))  // P2/P1 unused, high-Z (matches espp defaults)
#define IOX_0x44_DEFAULT_OUT   (IO44_BIT_WLAN_PWR_EN | IO44_BIT_USB_5V_EN)

// ── Display controller detection ────────────────────────────────────────
// Probe address 0x14 → GT911 present → this is the ILI9881C panel.
// Probe address 0x55 → ST7123's integrated touch present → this is ST7123.
#define PROBE_ADDR_GT911    0x14
#define PROBE_ADDR_ST7123   0x55

typedef enum { PANEL_UNKNOWN, PANEL_ILI9881, PANEL_ST7123 } panel_kind_t;

// GT911 (standalone touch, ILI9881 boards) — DEFAULT_ADDRESS_2 (INT high at
// power-on, which is what this board's expander-controlled TP_RST leaves it
// as by default).
#define GT911_I2C_ADDR      0x14
// ST7123 integrated touch engine.
#define ST7123_TOUCH_ADDR   0x55

// ── I2C state ────────────────────────────────────────────────────────────

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_ioexp43_dev;
static i2c_master_dev_handle_t s_ioexp44_dev;
static i2c_master_dev_handle_t s_touch_dev;

static esp_err_t ioexp_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), pdMS_TO_TICKS(50));
}

static esp_err_t ioexp_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *out)
{
    return i2c_master_transmit_receive(dev, &reg, 1, out, 1, pdMS_TO_TICKS(50));
}

static esp_err_t ioexp_set_bits(i2c_master_dev_handle_t dev, uint8_t mask, bool level)
{
    uint8_t cur = 0;
    esp_err_t err = ioexp_read_reg(dev, IOEXP_REG_OUTPUT, &cur);
    if (err != ESP_OK) return err;
    uint8_t next = level ? (uint8_t)(cur | mask) : (uint8_t)(cur & ~mask);
    return ioexp_write_reg(dev, IOEXP_REG_OUTPUT, next);
}

// direction_mask: 1=output. default_output: initial OUTPUT register value
// (applied before direction, avoiding output glitches — matches espp's
// pi4ioe5v.hpp init() ordering). high_z_mask: 1=high-impedance output.
static esp_err_t ioexp_configure(i2c_master_dev_handle_t dev, uint8_t direction_mask,
                                  uint8_t default_output, uint8_t high_z_mask)
{
    esp_err_t err;
    // Software reset (bit0 of CHIP_ID_CTRL) — start from a known state.
    err = ioexp_write_reg(dev, IOEXP_REG_CHIP_ID_CTRL, 0x01);
    if (err != ESP_OK) return err;

    err = ioexp_write_reg(dev, IOEXP_REG_OUTPUT, default_output);
    if (err != ESP_OK) return err;
    err = ioexp_write_reg(dev, IOEXP_REG_OUTPUT_HIZ, high_z_mask);
    if (err != ESP_OK) return err;
    // Pull-ups on every output pin (matches espp defaults — these pins
    // drive active peripherals, a floating input-during-transition should
    // not happen).
    err = ioexp_write_reg(dev, IOEXP_REG_PULL_ENABLE, direction_mask);
    if (err != ESP_OK) return err;
    err = ioexp_write_reg(dev, IOEXP_REG_PULL_SELECT, direction_mask);
    if (err != ESP_OK) return err;
    err = ioexp_write_reg(dev, IOEXP_REG_DIRECTION, direction_mask);
    if (err != ESP_OK) return err;
    err = ioexp_write_reg(dev, IOEXP_REG_INPUT_DEF, 0x00);
    if (err != ESP_OK) return err;
    // Disable all interrupts — this driver polls, it doesn't use the
    // expander's own interrupt line.
    return ioexp_write_reg(dev, IOEXP_REG_INT_MASK, 0xFF);
}

static esp_err_t i2c_bus_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "i2c bus init failed");

    // Standard-mode (100kHz), not the 400kHz+ this bus could nominally run
    // at — confirmed necessary against espp's own hard-won finding: at
    // higher speed the ST7123 touch reads time out and a hung transaction
    // corrupts concurrent reads of other devices sharing this bus. Every
    // device on this bus (touch, IO expanders) is configured at 100kHz.
    i2c_device_config_t io43_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = IOEXP_ADDR_0x43,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &io43_cfg, &s_ioexp43_dev),
                        TAG, "ioexp 0x43 add failed");

    i2c_device_config_t io44_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = IOEXP_ADDR_0x44,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &io44_cfg, &s_ioexp44_dev),
                        TAG, "ioexp 0x44 add failed");

    return ESP_OK;
}

static esp_err_t io_expanders_init(void)
{
    // 0x43: all defined outputs driven HIGH by default — releases LCD_RST
    // and TP_RST out of reset as soon as this completes (both are
    // active-low). initialize_lcd()'s own explicit LCD_RST pulse still runs
    // afterward for a clean, timed reset — this just establishes a safe
    // starting state.
    esp_err_t err = ioexp_configure(s_ioexp43_dev, IOX_0x43_OUTPUTS, IOX_0x43_OUTPUTS, 0x00);
    if (err != ESP_OK) { ESP_LOGE(TAG, "ioexp 0x43 config failed: %s", esp_err_to_name(err)); return err; }

    err = ioexp_configure(s_ioexp44_dev, IOX_0x44_OUTPUTS, IOX_0x44_DEFAULT_OUT, IOX_0x44_HIGH_Z);
    if (err != ESP_OK) { ESP_LOGE(TAG, "ioexp 0x44 config failed: %s", esp_err_to_name(err)); return err; }

    ESP_LOGI(TAG, "IO expanders (0x43, 0x44) initialized");
    return ESP_OK;
}

static void lcd_reset(bool assert_reset)
{
    // Active-low: assert (reset) = drive LOW; release = drive HIGH.
    ioexp_set_bits(s_ioexp43_dev, IO43_BIT_LCD_RST, !assert_reset);
}

static void touch_reset_line(bool assert_reset)
{
    ioexp_set_bits(s_ioexp43_dev, IO43_BIT_TP_RST, !assert_reset);
}

// ── Display controller detection ────────────────────────────────────────

static panel_kind_t detect_display_controller(void)
{
    if (i2c_master_probe(s_i2c_bus, PROBE_ADDR_GT911, 50) == ESP_OK) {
        return PANEL_ILI9881;
    }
    if (i2c_master_probe(s_i2c_bus, PROBE_ADDR_ST7123, 50) == ESP_OK) {
        return PANEL_ST7123;
    }
    return PANEL_UNKNOWN;
}

// ── DCS init command tables ─────────────────────────────────────────────
// Byte-for-byte sourced from espp/m5stack-tab5's Ili9881/St7123 display
// driver classes, cross-checked against M5Stack's own M5GFX
// (Panel_ILI9881C.hpp / Panel_ST7123.hpp) — every register write in both
// tables below is confirmed identical between the two independent sources.

typedef struct {
    uint8_t         cmd;
    const uint8_t  *params;
    uint8_t         param_len;
    uint16_t        delay_ms;
} panel_cmd_t;

#define PARR(...) ((const uint8_t[]){__VA_ARGS__})
#define CMD(c, d, ...) { .cmd = (uint8_t)(c), .params = PARR(__VA_ARGS__), \
                          .param_len = (uint8_t)sizeof(PARR(__VA_ARGS__)), .delay_ms = (uint16_t)(d) }
#define CMD0(c, d)     { .cmd = (uint8_t)(c), .params = NULL, .param_len = 0, .delay_ms = (uint16_t)(d) }

static const panel_cmd_t s_ili9881_cmds[] = {
    // Page 1 — DSI lane config
    CMD(0xFF, 0, 0x98, 0x81, 0x01),
    CMD(0xB7, 0, 0x03),                 // 2-lane DSI mode

    // Page 0 — exit sleep
    CMD(0xFF, 0, 0x98, 0x81, 0x00),
    CMD0(0x11, 120),                    // SLPOUT
    CMD(0x36, 0, 0x00),                 // MADCTL (identity — landscape native)
    CMD(0x3A, 0, 0x55),                 // COLMOD 16bpp

    // Page 3 — GIP (gate-in-panel) timing, registers 0x01..0x44
    CMD(0xFF, 0, 0x98, 0x81, 0x03),
    CMD(0x01, 0, 0x00), CMD(0x02, 0, 0x00), CMD(0x03, 0, 0x73), CMD(0x04, 0, 0x00),
    CMD(0x05, 0, 0x00), CMD(0x06, 0, 0x08), CMD(0x07, 0, 0x00), CMD(0x08, 0, 0x00),
    CMD(0x09, 0, 0x1B), CMD(0x0a, 0, 0x01), CMD(0x0b, 0, 0x01), CMD(0x0c, 0, 0x0D),
    CMD(0x0d, 0, 0x01), CMD(0x0e, 0, 0x01), CMD(0x0f, 0, 0x26), CMD(0x10, 0, 0x26),
    CMD(0x11, 0, 0x00), CMD(0x12, 0, 0x00), CMD(0x13, 0, 0x02), CMD(0x14, 0, 0x00),
    CMD(0x15, 0, 0x00), CMD(0x16, 0, 0x00), CMD(0x17, 0, 0x00), CMD(0x18, 0, 0x00),
    CMD(0x19, 0, 0x00), CMD(0x1a, 0, 0x00), CMD(0x1b, 0, 0x00), CMD(0x1c, 0, 0x00),
    CMD(0x1d, 0, 0x00), CMD(0x1e, 0, 0x40), CMD(0x1f, 0, 0x00), CMD(0x20, 0, 0x06),
    CMD(0x21, 0, 0x01), CMD(0x22, 0, 0x00), CMD(0x23, 0, 0x00), CMD(0x24, 0, 0x00),
    CMD(0x25, 0, 0x00), CMD(0x26, 0, 0x00), CMD(0x27, 0, 0x00), CMD(0x28, 0, 0x33),
    CMD(0x29, 0, 0x03), CMD(0x2a, 0, 0x00), CMD(0x2b, 0, 0x00), CMD(0x2c, 0, 0x00),
    CMD(0x2d, 0, 0x00), CMD(0x2e, 0, 0x00), CMD(0x2f, 0, 0x00), CMD(0x30, 0, 0x00),
    CMD(0x31, 0, 0x00), CMD(0x32, 0, 0x00), CMD(0x33, 0, 0x00), CMD(0x34, 0, 0x00),
    CMD(0x35, 0, 0x00), CMD(0x36, 0, 0x00), CMD(0x37, 0, 0x00), CMD(0x38, 0, 0x00),
    CMD(0x39, 0, 0x00), CMD(0x3a, 0, 0x00), CMD(0x3b, 0, 0x00), CMD(0x3c, 0, 0x00),
    CMD(0x3d, 0, 0x00), CMD(0x3e, 0, 0x00), CMD(0x3f, 0, 0x00), CMD(0x40, 0, 0x00),
    CMD(0x41, 0, 0x00), CMD(0x42, 0, 0x00), CMD(0x43, 0, 0x00), CMD(0x44, 0, 0x00),
    // Forward scan signal outputs (0x50..0x5D)
    CMD(0x50, 0, 0x01), CMD(0x51, 0, 0x23), CMD(0x52, 0, 0x45), CMD(0x53, 0, 0x67),
    CMD(0x54, 0, 0x89), CMD(0x55, 0, 0xab), CMD(0x56, 0, 0x01), CMD(0x57, 0, 0x23),
    CMD(0x58, 0, 0x45), CMD(0x59, 0, 0x67), CMD(0x5a, 0, 0x89), CMD(0x5b, 0, 0xab),
    CMD(0x5c, 0, 0xcd), CMD(0x5d, 0, 0xef),
    // Backward scan signal outputs (0x5E..0x73)
    CMD(0x5e, 0, 0x11), CMD(0x5f, 0, 0x02), CMD(0x60, 0, 0x00), CMD(0x61, 0, 0x07),
    CMD(0x62, 0, 0x06), CMD(0x63, 0, 0x0E), CMD(0x64, 0, 0x0F), CMD(0x65, 0, 0x0C),
    CMD(0x66, 0, 0x0D), CMD(0x67, 0, 0x02), CMD(0x68, 0, 0x02), CMD(0x69, 0, 0x02),
    CMD(0x6a, 0, 0x02), CMD(0x6b, 0, 0x02), CMD(0x6c, 0, 0x02), CMD(0x6d, 0, 0x02),
    CMD(0x6e, 0, 0x02), CMD(0x6f, 0, 0x02), CMD(0x70, 0, 0x02), CMD(0x71, 0, 0x02),
    CMD(0x72, 0, 0x02), CMD(0x73, 0, 0x05),
    // Right side signal outputs (0x74..0x89) + gate equalization (0x8A)
    CMD(0x74, 0, 0x01), CMD(0x75, 0, 0x02), CMD(0x76, 0, 0x00), CMD(0x77, 0, 0x07),
    CMD(0x78, 0, 0x06), CMD(0x79, 0, 0x0E), CMD(0x7a, 0, 0x0F), CMD(0x7b, 0, 0x0C),
    CMD(0x7c, 0, 0x0D), CMD(0x7d, 0, 0x02), CMD(0x7e, 0, 0x02), CMD(0x7f, 0, 0x02),
    CMD(0x80, 0, 0x02), CMD(0x81, 0, 0x02), CMD(0x82, 0, 0x02), CMD(0x83, 0, 0x02),
    CMD(0x84, 0, 0x02), CMD(0x85, 0, 0x02), CMD(0x86, 0, 0x02), CMD(0x87, 0, 0x02),
    CMD(0x88, 0, 0x02), CMD(0x89, 0, 0x05), CMD(0x8A, 0, 0x01),

    // Page 4 — power control
    CMD(0xFF, 0, 0x98, 0x81, 0x04),
    CMD(0x38, 0, 0x01),                 // VREG2 output enable
    CMD(0x39, 0, 0x00),                 // VREG1 output control
    CMD(0x6C, 0, 0x15),                 // VGH clamp
    CMD(0x6E, 0, 0x1A),                 // VGL clamp
    CMD(0x6F, 0, 0x25),                 // charge pump clamp
    CMD(0x3A, 0, 0xA4),                 // power control setting
    CMD(0x8D, 0, 0x20),                 // VCL voltage level
    CMD(0x87, 0, 0xBA),                 // VCORE voltage
    CMD(0x3B, 0, 0x98),                 // VGH/VGL timing

    // Page 1 — VREG/bias + gamma
    CMD(0xFF, 0, 0x98, 0x81, 0x01),
    CMD(0x22, 0, 0x0A), CMD(0x31, 0, 0x00),
    CMD(0x50, 0, 0x6B), CMD(0x51, 0, 0x66), CMD(0x53, 0, 0x73), CMD(0x55, 0, 0x8B),
    CMD(0x60, 0, 0x1B), CMD(0x61, 0, 0x01), CMD(0x62, 0, 0x0C), CMD(0x63, 0, 0x00),
    // Positive gamma (20 points)
    CMD(0xA0, 0, 0x00), CMD(0xA1, 0, 0x15), CMD(0xA2, 0, 0x1F), CMD(0xA3, 0, 0x13),
    CMD(0xA4, 0, 0x11), CMD(0xA5, 0, 0x21), CMD(0xA6, 0, 0x17), CMD(0xA7, 0, 0x1B),
    CMD(0xA8, 0, 0x6B), CMD(0xA9, 0, 0x1E), CMD(0xAA, 0, 0x2B), CMD(0xAB, 0, 0x5D),
    CMD(0xAC, 0, 0x19), CMD(0xAD, 0, 0x14), CMD(0xAE, 0, 0x4B), CMD(0xAF, 0, 0x1D),
    CMD(0xB0, 0, 0x27), CMD(0xB1, 0, 0x49), CMD(0xB2, 0, 0x5D), CMD(0xB3, 0, 0x39),
    // Negative gamma (20 points)
    CMD(0xC0, 0, 0x00), CMD(0xC1, 0, 0x01), CMD(0xC2, 0, 0x0C), CMD(0xC3, 0, 0x11),
    CMD(0xC4, 0, 0x15), CMD(0xC5, 0, 0x28), CMD(0xC6, 0, 0x1B), CMD(0xC7, 0, 0x1C),
    CMD(0xC8, 0, 0x62), CMD(0xC9, 0, 0x1C), CMD(0xCA, 0, 0x29), CMD(0xCB, 0, 0x60),
    CMD(0xCC, 0, 0x16), CMD(0xCD, 0, 0x17), CMD(0xCE, 0, 0x4A), CMD(0xCF, 0, 0x23),
    CMD(0xD0, 0, 0x24), CMD(0xD1, 0, 0x4F), CMD(0xD2, 0, 0x5F), CMD(0xD3, 0, 0x39),

    // Page 0 — final
    CMD(0xFF, 0, 0x98, 0x81, 0x00),
    CMD0(0x35, 0),                      // TE ON
    CMD0(0xFE, 0),                      // extended NOP
    CMD0(0x11, 120),                    // SLPOUT (again — harmless, matches espp exactly)
    CMD(0x36, 0, 0x00),                 // MADCTL
    CMD(0x3A, 0, 0x55),                 // COLMOD
    CMD0(0x29, 20),                     // DISPON
};

static const panel_cmd_t s_st7123_cmds[] = {
    CMD(0x60, 0, 0x71, 0x23, 0xa2),
    CMD(0x60, 0, 0x71, 0x23, 0xa3),
    CMD(0x60, 0, 0x71, 0x23, 0xa4),
    CMD(0xA4, 0, 0x31),
    CMD(0xD7, 0, 0x10, 0x0A, 0x10, 0x2A, 0x80, 0x80),
    CMD(0x90, 0, 0x71, 0x23, 0x5A, 0x20, 0x24, 0x09, 0x09),
    CMD(0xA3, 0,
        0x80, 0x01, 0x88, 0x30, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46, 0x00, 0x00,
        0x1E, 0x5C, 0x1E, 0x80, 0x00, 0x4F, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46,
        0x00, 0x00, 0x1E, 0x5C, 0x1E, 0x80, 0x00, 0x6F, 0x58, 0x00, 0x00, 0x00, 0xFF),
    CMD(0xA6, 0,
        0x03, 0x00, 0x24, 0x55, 0x36, 0x00, 0x39, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x00, 0x24,
        0x55, 0x38, 0x00, 0x37, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x00, 0x24, 0x11, 0x00, 0x00,
        0x00, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x00, 0xEC, 0x11, 0x00, 0x03, 0x00, 0x03, 0x6E,
        0x6E, 0xFF, 0xFF, 0x00, 0x08, 0x80, 0x08, 0x80, 0x06, 0x00, 0x00, 0x00, 0x00),
    CMD(0xA7, 0,
        0x19, 0x19, 0x80, 0x64, 0x40, 0x07, 0x16, 0x40, 0x00, 0x44, 0x03, 0x6E, 0x6E, 0x91, 0xFF,
        0x08, 0x80, 0x64, 0x40, 0x25, 0x34, 0x40, 0x00, 0x02, 0x01, 0x6E, 0x6E, 0x91, 0xFF, 0x08,
        0x80, 0x64, 0x40, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x08, 0x80,
        0x64, 0x40, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x6E, 0x6E, 0x84, 0xFF, 0x08, 0x80, 0x44),
    CMD(0xAC, 0,
        0x03, 0x19, 0x19, 0x18, 0x18, 0x06, 0x13, 0x13, 0x11, 0x11, 0x08, 0x08, 0x0A, 0x0A, 0x1C,
        0x1C, 0x07, 0x07, 0x00, 0x00, 0x02, 0x02, 0x01, 0x19, 0x19, 0x18, 0x18, 0x06, 0x12, 0x12,
        0x10, 0x10, 0x09, 0x09, 0x0B, 0x0B, 0x1C, 0x1C, 0x07, 0x07, 0x03, 0x03, 0x01, 0x01),
    CMD(0xAD, 0,
        0xF0, 0x00, 0x46, 0x00, 0x03, 0x50, 0x50, 0xFF, 0xFF, 0xF0, 0x40, 0x06, 0x01,
        0x07, 0x42, 0x42, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF),
    CMD(0xAE, 0, 0xFE, 0x3F, 0x3F, 0xFE, 0x3F, 0x3F, 0x00),
    CMD(0xB2, 0,
        0x15, 0x19, 0x05, 0x23, 0x49, 0xAF, 0x03, 0x2E, 0x5C, 0xD2, 0xFF, 0x10, 0x20, 0xFD, 0x20,
        0xC0, 0x00),
    CMD(0xE8, 0,
        0x20, 0x6F, 0x04, 0x97, 0x97, 0x3E, 0x04, 0xDC, 0xDC, 0x3E, 0x06, 0xFA, 0x26, 0x3E),
    CMD(0x75, 0, 0x03, 0x04),
    CMD(0xE7, 0,
        0x3B, 0x00, 0x00, 0x7C, 0xA1, 0x8C, 0x20, 0x1A, 0xF0, 0xB1, 0x50, 0x00,
        0x50, 0xB1, 0x50, 0xB1, 0x50, 0xD8, 0x00, 0x55, 0x00, 0xB1, 0x00, 0x45,
        0xC9, 0x6A, 0xFF, 0x5A, 0xD8, 0x18, 0x88, 0x15, 0xB1, 0x01, 0x01, 0x77),
    CMD(0xEA, 0, 0x13, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x2C),
    CMD(0xB0, 0, 0x22, 0x43, 0x11, 0x61, 0x25, 0x43, 0x43),
    CMD(0xB7, 0, 0x00, 0x00, 0x73, 0x73),
    CMD(0xBF, 0, 0xA6, 0xAA),
    CMD(0xA9, 0, 0x00, 0x00, 0x73, 0xFF, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03),
    CMD(0xC8, 0,
        0x00, 0x00, 0x10, 0x1F, 0x36, 0x00, 0x5D, 0x04, 0x9D, 0x05, 0x10, 0xF2, 0x06,
        0x60, 0x03, 0x11, 0xAD, 0x00, 0xEF, 0x01, 0x22, 0x2E, 0x0E, 0x74, 0x08, 0x32,
        0xDC, 0x09, 0x33, 0x0F, 0xF3, 0x77, 0x0D, 0xB0, 0xDC, 0x03, 0xFF),
    CMD(0xC9, 0,
        0x00, 0x00, 0x10, 0x1F, 0x36, 0x00, 0x5D, 0x04, 0x9D, 0x05, 0x10, 0xF2, 0x06,
        0x60, 0x03, 0x11, 0xAD, 0x00, 0xEF, 0x01, 0x22, 0x2E, 0x0E, 0x74, 0x08, 0x32,
        0xDC, 0x09, 0x33, 0x0F, 0xF3, 0x77, 0x0D, 0xB0, 0xDC, 0x03, 0xFF),
    // Power-on tail — see file header comment: M5GFX's official source
    // orders this DISPON+TE_ON, then SLPOUT-last-with-120ms-delay instead.
    CMD(0x11, 100, 0x00),               // SLPOUT
    CMD(0x29, 0, 0x00),                 // DISPON
    CMD(0x35, 100, 0x00),               // TE ON
    CMD(0x36, 0, 0x00),                 // MADCTL (identity)
    CMD(0x3A, 0, 0x55),                 // COLMOD 16bpp
};

static void send_panel_cmds(esp_lcd_panel_io_handle_t io, const panel_cmd_t *cmds, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        esp_err_t err = esp_lcd_panel_io_tx_param(io, cmds[i].cmd, cmds[i].params, cmds[i].param_len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "panel cmd 0x%02X failed: %s", cmds[i].cmd, esp_err_to_name(err));
        }
        if (cmds[i].delay_ms) vTaskDelay(pdMS_TO_TICKS(cmds[i].delay_ms));
    }
}

// ── Backlight (direct LEDC PWM on GPIO22, not behind the IO expander) ───

#define BACKLIGHT_LEDC_CH    LEDC_CHANNEL_0
#define BACKLIGHT_LEDC_TIMER LEDC_TIMER_0
#define BACKLIGHT_DUTY_MAX   1023   // 10-bit resolution, matches espp's config

static bool s_backlight_ready = false;

static void backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = BACKLIGHT_LEDC_TIMER,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer_cfg) != ESP_OK) return;

    ledc_channel_config_t ch_cfg = {
        .gpio_num   = BACKLIGHT_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = BACKLIGHT_LEDC_CH,
        .timer_sel  = BACKLIGHT_LEDC_TIMER,
        .duty       = BACKLIGHT_DUTY_MAX,
        .hpoint     = 0,
    };
    if (ledc_channel_config(&ch_cfg) != ESP_OK) return;

    s_backlight_ready = true;
}

static esp_err_t m5tab5_set_brightness(uint8_t level)
{
    if (!s_backlight_ready) return ESP_ERR_INVALID_STATE;
    uint32_t duty = ((uint32_t)level * BACKLIGHT_DUTY_MAX) / 255;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BACKLIGHT_LEDC_CH, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BACKLIGHT_LEDC_CH);
    return ESP_OK;
}

// ── MIPI-DSI / DPI panel state ──────────────────────────────────────────

static esp_lcd_dsi_bus_handle_t s_dsi_bus;
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_flush_sem;
static panel_kind_t s_panel_kind = PANEL_UNKNOWN;
static bool s_ready = false;

static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_handle_t panel,
                                           esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    (void)panel; (void)edata; (void)user_ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_sem, &woken);
    return woken == pdTRUE;
}

static esp_err_t dsi_display_init(void)
{
    esp_err_t ret;

    // MIPI DSI PHY power (LDO channel 3, 2500mV — fixed by the P4's own
    // hardware wiring, not board-specific).
    static esp_ldo_channel_handle_t s_phy_ldo;
    esp_ldo_channel_config_t phy_cfg = {
        .chan_id = 3,
        .voltage_mv = 2500,
    };
    ret = esp_ldo_acquire_channel(&phy_cfg, &s_phy_ldo);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DSI PHY LDO acquire failed: %s", esp_err_to_name(ret));
        return ret;
    }

    backlight_init();
    if (s_backlight_ready) m5tab5_set_brightness(255);

    // Hardware reset via IO expander.
    lcd_reset(true);
    vTaskDelay(pdMS_TO_TICKS(10));
    lcd_reset(false);
    vTaskDelay(pdMS_TO_TICKS(120));

    s_panel_kind = detect_display_controller();
    if (s_panel_kind == PANEL_UNKNOWN) {
        ESP_LOGE(TAG, "unable to detect display controller (no response at 0x%02X or 0x%02X)",
                 PROBE_ADDR_GT911, PROBE_ADDR_ST7123);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "display controller detected: %s",
             s_panel_kind == PANEL_ILI9881 ? "ILI9881C" : "ST7123");

    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = (s_panel_kind == PANEL_ILI9881) ? 730 : 965,
    };
    ret = esp_lcd_new_dsi_bus(&bus_cfg, &s_dsi_bus);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "DSI bus init failed: %s", esp_err_to_name(ret)); return ret; }

    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ret = esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_cfg, &s_panel_io);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "panel IO init failed: %s", esp_err_to_name(ret)); return ret; }

    esp_lcd_dpi_panel_config_t dpi_cfg = {0};
    dpi_cfg.virtual_channel = 0;
    dpi_cfg.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_cfg.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
    dpi_cfg.num_fbs = 1;
    dpi_cfg.video_timing.h_size = PANEL_WIDTH;
    dpi_cfg.video_timing.v_size = PANEL_HEIGHT;
    dpi_cfg.flags.use_dma2d = true;

    if (s_panel_kind == PANEL_ILI9881) {
        dpi_cfg.dpi_clock_freq_mhz = 60;
        dpi_cfg.video_timing.hsync_back_porch = 140;
        dpi_cfg.video_timing.hsync_pulse_width = 40;
        dpi_cfg.video_timing.hsync_front_porch = 40;
        dpi_cfg.video_timing.vsync_back_porch = 20;
        dpi_cfg.video_timing.vsync_pulse_width = 4;
        dpi_cfg.video_timing.vsync_front_porch = 20;
    } else {
        // ST7123 is a TDDI part: its touch engine scans during the display
        // blanking interval, timed against this exact pixel clock. Running
        // faster than 70MHz shrinks the blanking window and desyncs touch
        // (display still shows, touch silently stops working) — confirmed
        // against espp's own hard-won comment on this exact point. Keep at
        // 70MHz.
        dpi_cfg.dpi_clock_freq_mhz = 70;
        dpi_cfg.video_timing.hsync_back_porch = 40;
        dpi_cfg.video_timing.hsync_pulse_width = 2;
        dpi_cfg.video_timing.hsync_front_porch = 40;
        dpi_cfg.video_timing.vsync_back_porch = 8;
        dpi_cfg.video_timing.vsync_pulse_width = 2;
        dpi_cfg.video_timing.vsync_front_porch = 220;
    }

    ret = esp_lcd_new_panel_dpi(s_dsi_bus, &dpi_cfg, &s_panel);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "DPI panel create failed: %s", esp_err_to_name(ret)); return ret; }

    // Send the controller-specific DCS init table over the DBI panel IO
    // while the DPI video engine is still stopped.
    if (s_panel_kind == PANEL_ILI9881) {
        send_panel_cmds(s_panel_io, s_ili9881_cmds, sizeof(s_ili9881_cmds) / sizeof(s_ili9881_cmds[0]));
    } else {
        send_panel_cmds(s_panel_io, s_st7123_cmds, sizeof(s_st7123_cmds) / sizeof(s_st7123_cmds[0]));
    }

    // Start the DPI video timing engine.
    ret = esp_lcd_panel_init(s_panel);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(ret)); return ret; }

    s_flush_sem = xSemaphoreCreateBinary();
    esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_color_trans_done = on_color_trans_done,
        .on_refresh_done = NULL,
    };
    ret = esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, NULL);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "event callback register failed: %s", esp_err_to_name(ret)); return ret; }

    ESP_LOGI(TAG, "display ready (%dx%d, %s)", PANEL_WIDTH, PANEL_HEIGHT,
             s_panel_kind == PANEL_ILI9881 ? "ILI9881C" : "ST7123");
    return ESP_OK;
}

// ── catcall_display_t ───────────────────────────────────────────────────

static esp_err_t m5tab5_display_init(const display_config_t *cfg)
{
    (void)cfg;
    // Real bring-up happens in m5tab5_bsp_drv_init() (baked-in, called
    // directly by kernel_tab5_boot.c before this catcall is even
    // registered) — matches st7789.c/gt911.c's own pattern for Layer 0
    // drivers on T-Deck Plus.
    return s_ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t draw_bitmap_blocking(int x, int y, int w, int h, const uint16_t *data)
{
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, data);
    if (err != ESP_OK) return err;
    // Block until the DPI event callback signals the transfer landed —
    // keeps push_pixels()/fill_rect() synchronous like every other
    // catcall_display_t implementation in this codebase.
    if (xSemaphoreTake(s_flush_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "draw_bitmap: flush wait timed out");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t m5tab5_push_pixels(int x, int y, int w, int h, const uint16_t *data)
{
    if (!s_ready || !data || w <= 0 || h <= 0) return ESP_ERR_INVALID_STATE;
    return draw_bitmap_blocking(x, y, w, h, data);
}

static esp_err_t m5tab5_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_ready || w <= 0 || h <= 0) return ESP_ERR_INVALID_STATE;
    uint16_t *line = (uint16_t *)heap_caps_malloc((size_t)w * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!line) return ESP_ERR_NO_MEM;
    for (int i = 0; i < w; i++) line[i] = color;

    esp_err_t err = ESP_OK;
    for (int row = 0; row < h && err == ESP_OK; row++) {
        err = draw_bitmap_blocking(x, y + row, w, 1, line);
    }
    heap_caps_free(line);
    return err;
}

static void m5tab5_get_info(display_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->width = PANEL_WIDTH;
    out->height = PANEL_HEIGHT;
    out->bits_per_pixel = 16;
    snprintf(out->name, sizeof(out->name), "m5tab5_bsp");
}

static esp_err_t m5tab5_display_deinit(void) { return ESP_OK; }

static const catcall_display_t s_display_catcall = {
    .name            = "m5tab5_bsp",
    .catcall_version = CATCALL_DISPLAY_VERSION,
    .init            = m5tab5_display_init,
    .push_pixels     = m5tab5_push_pixels,
    .fill_rect       = m5tab5_fill_rect,
    .set_brightness  = m5tab5_set_brightness,
    .get_info        = m5tab5_get_info,
    .deinit          = m5tab5_display_deinit,
};

// ── Touch ────────────────────────────────────────────────────────────────
// Polled, not interrupt-driven — matches gt911.c's own poll_mode fallback
// (already proven in this codebase for the same catcall_touch_t contract),
// avoiding the ISR-context I2C error modes espp's own comments warn about
// for interrupt-driven reads on this exact bus.

// GT911 register map (standalone touch, ILI9881 boards).
#define GT911_REG_POINT_INFO  0x814E
#define GT911_REG_POINT_1     0x814F   // 8 bytes: trackId, x(u16 LE), y(u16 LE), area(u16 LE), reserved

// ST7123 integrated touch register map.
#define ST7123_REG_ADV_INFO     0x0010   // bit3 = with_coord
#define ST7123_REG_MAX_TOUCHES  0x0009
#define ST7123_REG_REPORT_0     0x0014   // 7 bytes per contact

static esp_err_t touch_write_reg16(uint16_t reg, uint8_t val)
{
    uint8_t buf[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val };
    return i2c_master_transmit(s_touch_dev, buf, sizeof(buf), pdMS_TO_TICKS(50));
}

static esp_err_t touch_read_reg16(uint16_t reg, uint8_t *data, size_t len)
{
    uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return i2c_master_transmit_receive(s_touch_dev, addr, sizeof(addr), data, len, pdMS_TO_TICKS(50));
}

static bool gt911_read(uint16_t *x, uint16_t *y)
{
    uint8_t status = 0;
    if (touch_read_reg16(GT911_REG_POINT_INFO, &status, 1) != ESP_OK) return false;
    if (!(status & 0x80)) return false;               // no data ready

    bool have_point = false;
    uint8_t touch_count = status & 0x0F;
    if (touch_count > 0) {
        uint8_t pt[8];
        if (touch_read_reg16(GT911_REG_POINT_1, pt, sizeof(pt)) == ESP_OK) {
            *x = (uint16_t)pt[1] | ((uint16_t)pt[2] << 8);
            *y = (uint16_t)pt[3] | ((uint16_t)pt[4] << 8);
            have_point = true;
        }
    }
    touch_write_reg16(GT911_REG_POINT_INFO, 0x00);    // sync/clear signal
    return have_point;
}

static bool st7123touch_read(uint16_t *x, uint16_t *y)
{
    uint8_t adv_info = 0;
    if (touch_read_reg16(ST7123_REG_ADV_INFO, &adv_info, 1) != ESP_OK) return false;
    if (!(adv_info & (1 << 3))) return false;          // no coordinate data

    uint8_t max_touches = 0;
    if (touch_read_reg16(ST7123_REG_MAX_TOUCHES, &max_touches, 1) != ESP_OK) return false;
    if (max_touches == 0 || max_touches > 10) return false;

    uint8_t data[10 * 7];
    if (touch_read_reg16(ST7123_REG_REPORT_0, data, (size_t)max_touches * 7) != ESP_OK) return false;

    for (int i = 0; i < max_touches; i++) {
        const uint8_t *p = &data[i * 7];
        if (!(p[0] & 0x80)) continue;                  // not valid
        *x = (uint16_t)(((p[0] & 0x3F) << 8) | p[1]);
        *y = (uint16_t)((p[2] << 8) | p[3]);
        return true;
    }
    return false;
}

static esp_err_t touch_init(void)
{
    // Reset/enable sequencing differs by controller, though both pulse the
    // same IO-expander bit (0x43 P5) — GT911 needs a standard active-low
    // reset pulse; ST7123's touch engine needs a slower disable->enable
    // pulse to actually start scanning (confirmed against espp's own
    // touchpad.cpp comment — without this exact pulse the ST7123 answers
    // I2C and reports correct firmware/config registers but never produces
    // coordinates).
    i2c_device_config_t touch_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = (s_panel_kind == PANEL_ST7123) ? ST7123_TOUCH_ADDR : GT911_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &touch_cfg, &s_touch_dev);
    if (err != ESP_OK) { ESP_LOGE(TAG, "touch I2C device add failed: %s", esp_err_to_name(err)); return err; }

    if (s_panel_kind == PANEL_ST7123) {
        touch_reset_line(true);
        vTaskDelay(pdMS_TO_TICKS(20));
        touch_reset_line(false);
        vTaskDelay(pdMS_TO_TICKS(50));
    } else {
        touch_reset_line(true);
        vTaskDelay(pdMS_TO_TICKS(10));
        touch_reset_line(false);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGI(TAG, "touch controller ready (%s)", s_panel_kind == PANEL_ST7123 ? "ST7123" : "GT911");
    return ESP_OK;
}

static esp_err_t m5tab5_touch_init(const touch_config_t *cfg)
{
    (void)cfg;
    return s_ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static bool m5tab5_read_point(uint16_t *x, uint16_t *y)
{
    if (!s_ready || !x || !y || !s_touch_dev) return false;
    return (s_panel_kind == PANEL_ST7123) ? st7123touch_read(x, y) : gt911_read(x, y);
}

static bool m5tab5_is_pressed(void)
{
    uint16_t x = 0, y = 0;
    return m5tab5_read_point(&x, &y);
}

static esp_err_t m5tab5_touch_deinit(void) { return ESP_OK; }

static const catcall_touch_t s_touch_catcall = {
    .name            = "m5tab5_bsp",
    .catcall_version = CATCALL_TOUCH_VERSION,
    .init            = m5tab5_touch_init,
    .read_point      = m5tab5_read_point,
    .is_pressed      = m5tab5_is_pressed,
    .deinit          = m5tab5_touch_deinit,
};

// ── Lifecycle ────────────────────────────────────────────────────────────

int m5tab5_bsp_drv_init(void)
{
    if (i2c_bus_init() != ESP_OK) return -1;
    if (io_expanders_init() != ESP_OK) return -1;
    if (dsi_display_init() != ESP_OK) return -1;
    if (touch_init() != ESP_OK) {
        ESP_LOGW(TAG, "touch init failed — continuing without touch");
    }

    s_ready = true;
    purr_kernel_register_display(&s_display_catcall);
    purr_kernel_register_touch(&s_touch_catcall);
    ESP_LOGI(TAG, "ready (%dx%d)", PANEL_WIDTH, PANEL_HEIGHT);
    return 0;
}

void m5tab5_bsp_drv_deinit(void)
{
    s_ready = false;
}

int m5tab5_bsp_sdcard_init(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;   // 40MHz

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.clk = SD_CLK_PIN;
    slot_cfg.cmd = SD_CMD_PIN;
    slot_cfg.d0  = SD_DAT0_PIN;
    slot_cfg.d1  = SD_DAT1_PIN;
    slot_cfg.d2  = SD_DAT2_PIN;
    slot_cfg.d3  = SD_DAT3_PIN;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card = NULL;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_cfg, &mount_cfg, &card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
        return -1;
    }

    ESP_LOGI(TAG, "SD card ready");
    return 0;
}
