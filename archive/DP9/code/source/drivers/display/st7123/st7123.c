// st7123.c — ST7123 MIPI-DSI display + touch driver for PURR OS (M5Stack Tab5)
// Catcall-compatible, ESP-IDF v5.5+, esp32p4 only.
//
// The ST7123 is a combined display+touch IC (same pattern as axs15231b): this
// one driver provides both the display and touch catcalls. Panel is natively
// 720×1280 portrait; PURR uses the Tab5 landscape (keyboard attached), so this
// driver presents a 1280×720 surface and rotates in hardware:
//
//   push_pixels/fill_rect → landscape PSRAM back buffer → (flush task) →
//   PPA SRM 90° rotate → 720×1280 DPI framebuffer → panel
//
// Rotation lives here, per the repo's display contract: get_info() reports the
// final rotated 1280×720 and push_pixels coordinates are final screen space
// (same rule st7789 implements with MADCTL — the DSI/DPI pipeline has no
// MADCTL-equivalent that rotates the scanout, hence the PPA).
//
// Init parameters (DSI lanes/bitrate/DPI clock/porches, IO-expander register
// writes, vendor init-cmd table) are copied from M5Stack's official
// M5Tab5-UserDemo board support (m5stack_tab5.c) — the only vendor code that
// supports the post-Oct-2025 ST7123 hardware revision — and match the user's
// independently working Tab5 projects.

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/ppa.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_st7123.h"
#include "esp_lcd_touch_st7123.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"

#include "../../../kernel/core/purr_module.h"
#include "../../../kernel/core/purr_kernel.h"
#include "../../../kernel/catcalls/catcall_display.h"
#include "../../../kernel/catcalls/catcall_touch.h"

static const char *TAG = "drv:st7123";

// ── Fixed Tab5 wiring ─────────────────────────────────────────────────────────
// Hard-coded per repo convention (native display drivers own their pins; the
// [pins] block in device.pcat is documentation). The Tab5 has one hardware
// layout — there are no variants to configure for.
#define TAB5_I2C_PORT        I2C_NUM_0   // system bus: touch + IO expanders
#define TAB5_I2C_SDA         31
#define TAB5_I2C_SCL         32
#define TAB5_PIN_BL          22
#define TAB5_TOUCH_ADDR      0x55
#define TAB5_TOUCH_INT       23          // wired, but we poll — see touch init
#define TAB5_PI4IOE1_ADDR    0x43        // SPK_EN/EXT5V/LCD_RST/TP_RST/CAM_RST
#define TAB5_PI4IOE2_ADDR    0x44        // WLAN_PWR/USB5V/CHG_EN

// PI4IOE5V6408 IO expander registers
#define PI4IO_REG_CHIP_RESET 0x01
#define PI4IO_REG_IO_DIR     0x03
#define PI4IO_REG_OUT_SET    0x05
#define PI4IO_REG_OUT_H_IM   0x07
#define PI4IO_REG_IN_DEF_STA 0x09
#define PI4IO_REG_PULL_EN    0x0B
#define PI4IO_REG_PULL_SEL   0x0D
#define PI4IO_REG_INT_MASK   0x11

// Panel geometry
#define ST7123_PHYS_W        720          // native portrait
#define ST7123_PHYS_H        1280

// UI scale. MiniWin's fonts/icons/hit-targets are pixel-fixed (tuned for
// ~320x240 panels — there is no DPI knob in the framework), so at the native
// 1280x720 everything renders quarter-size on the 5" panel: confirmed
// unusably tiny on hardware. Instead the driver presents a 640x360 logical
// surface and the PPA scales 2x during the same SRM pass that rotates it
// into the portrait framebuffer — widgets come out physically 2x, and
// 640x360 still gives MiniWin more room than the 480x320 it ships on today.
#define ST7123_UI_SCALE      2
#define ST7123_LAND_W        (1280 / ST7123_UI_SCALE)   // what PURR sees
#define ST7123_LAND_H        (720  / ST7123_UI_SCALE)

// DSI/DPI parameters (M5 UserDemo values — ST7123-specific, don't reuse
// ILI9881C porches)
#define ST7123_DSI_LANES     2
#define ST7123_DSI_MBPS      965
#define ST7123_DPI_CLK_MHZ   70

// DSI PHY power rail
#define ST7123_LDO_CHAN      3
#define ST7123_LDO_MV        2500

// Backlight: LEDC 12-bit @ 5 kHz (UserDemo scale — duty max 4095)
#define BL_SPEED_MODE        LEDC_LOW_SPEED_MODE
#define BL_TIMER_NUM         LEDC_TIMER_0
#define BL_CHANNEL           LEDC_CHANNEL_1
#define BL_FREQ_HZ           5000
#define BL_RESOLUTION        LEDC_TIMER_12_BIT
#define BL_DUTY_MAX          4095

// Flush cadence — coalesce window after the first dirty write of a frame.
#define FLUSH_COALESCE_MS    20

// Rotation direction. The back buffer is landscape; the PPA rotates it into
// the portrait scanout framebuffer. Whether "90" or "270" produces the
// keyboard-at-the-bottom orientation depends on which edge the Tab5 mounts
// the connector — if the desktop comes up upside-down on hardware, flip this
// one define (both the angle and the offset math key off it).
// #define ST7123_ROTATE_270

// ── Driver state ──────────────────────────────────────────────────────────────

static bool                     s_ready       = false;
static i2c_master_bus_handle_t  s_i2c_bus     = NULL;
static esp_ldo_channel_handle_t s_phy_ldo     = NULL;
static esp_lcd_dsi_bus_handle_t s_dsi_bus     = NULL;
static esp_lcd_panel_io_handle_t s_dbi_io     = NULL;
static esp_lcd_panel_handle_t   s_panel       = NULL;
static esp_lcd_touch_handle_t   s_touch       = NULL;
static esp_lcd_panel_io_handle_t s_touch_io   = NULL;

static uint16_t                *s_backbuf     = NULL;   // landscape, PSRAM
static uint16_t                *s_fb          = NULL;   // portrait DPI scanout FB
static ppa_client_handle_t      s_ppa         = NULL;

static SemaphoreHandle_t        s_dirty_mtx   = NULL;
static SemaphoreHandle_t        s_dirty_sem   = NULL;
static TaskHandle_t             s_flush_task  = NULL;
// Dirty rect in landscape coords; w<=0 means clean.
static int s_dx0, s_dy0, s_dx1, s_dy1;
static bool s_dirty = false;

static bool    s_bl_on      = false;   // deferred until first frame is flushed
static uint8_t s_brightness = 200;     // pending level applied at first flush

// ── PI4IOE5V6408 IO expanders ─────────────────────────────────────────────────
// Full register sequence from M5Tab5-UserDemo bsp_io_expander_pi4ioe_init()
// (not just the OUT_SET writes) — includes pull config and interrupt masks,
// and powers the rails the rest of the board needs: EXT5V, LCD_RST, TP_RST
// (expander 1) and WLAN_PWR, USB5V (expander 2, needed later for esp-hosted).

static esp_err_t pi4ioe_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, 2, 100);
}

static esp_err_t pi4ioe_init(void)
{
    i2c_master_dev_handle_t dev1 = NULL, dev2 = NULL;
    i2c_device_config_t cfg1 = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = TAB5_PI4IOE1_ADDR,
        .scl_speed_hz    = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &cfg1, &dev1), TAG, "pi4ioe1 add");
    pi4ioe_write(dev1, PI4IO_REG_CHIP_RESET, 0xFF);
    pi4ioe_write(dev1, PI4IO_REG_IO_DIR,     0b01111111);  // P0–P6 out, P7 in
    pi4ioe_write(dev1, PI4IO_REG_OUT_H_IM,   0b00000000);  // outputs out of high-Z
    pi4ioe_write(dev1, PI4IO_REG_PULL_SEL,   0b01111111);
    pi4ioe_write(dev1, PI4IO_REG_PULL_EN,    0b01111111);
    // P1 SPK_EN, P2 EXT5V_EN, P4 LCD_RST, P5 TP_RST, P6 CAM_RST high
    pi4ioe_write(dev1, PI4IO_REG_OUT_SET,    0b01110110);

    // Hard-reset the panel+touch on EVERY boot, not just cold power-on. A warm
    // reboot (esp_restart — the Restart menu, post-OTA, esptool's after-flash
    // reset) resets the P4 but leaves the ST7123 running in DSI video mode;
    // re-running the init sequence against that warm state leaves the screen
    // dark until a physical power cycle (confirmed live). The RST lines only
    // exist behind this expander (P4=LCD_RST, P5=TP_RST), so pulse them here:
    // low 20ms, back high, then the ST7123 datasheet's ~120ms wake before the
    // DSI bring-up talks to it. (M5Stack's own demo never soft-reboots — its
    // esp_restart call is commented out — so vendor code never hits this.)
    pi4ioe_write(dev1, PI4IO_REG_OUT_SET,    0b01000110);
    vTaskDelay(pdMS_TO_TICKS(20));
    pi4ioe_write(dev1, PI4IO_REG_OUT_SET,    0b01110110);
    vTaskDelay(pdMS_TO_TICKS(120));

    i2c_device_config_t cfg2 = cfg1;
    cfg2.device_address = TAB5_PI4IOE2_ADDR;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &cfg2, &dev2), TAG, "pi4ioe2 add");
    pi4ioe_write(dev2, PI4IO_REG_CHIP_RESET, 0xFF);
    pi4ioe_write(dev2, PI4IO_REG_IO_DIR,     0b10111001);
    pi4ioe_write(dev2, PI4IO_REG_OUT_H_IM,   0b00000110);
    pi4ioe_write(dev2, PI4IO_REG_PULL_SEL,   0b10111001);
    pi4ioe_write(dev2, PI4IO_REG_PULL_EN,    0b11111001);
    pi4ioe_write(dev2, PI4IO_REG_IN_DEF_STA, 0b01000000);
    pi4ioe_write(dev2, PI4IO_REG_INT_MASK,   0b10111111);
    // P0 WLAN_PWR_EN, P3 USB5V_EN high (CHG_EN P7 left low, per UserDemo)
    pi4ioe_write(dev2, PI4IO_REG_OUT_SET,    0b00001001);

    // Give LCD_RST/TP_RST time high before DSI/touch init talks to the chip.
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

// ── Backlight ─────────────────────────────────────────────────────────────────

static void bl_init(void)
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
        .gpio_num   = TAB5_PIN_BL,
        .speed_mode = BL_SPEED_MODE,
        .channel    = BL_CHANNEL,
        .timer_sel  = BL_TIMER_NUM,
        .duty       = 0,             // off until the first frame is on-screen
        .hpoint     = 0,
    };
    ledc_channel_config(&ccfg);
}

static void bl_set(uint8_t level)
{
    uint32_t duty = (uint32_t)level * BL_DUTY_MAX / 255;
    ledc_set_duty(BL_SPEED_MODE, BL_CHANNEL, duty);
    ledc_update_duty(BL_SPEED_MODE, BL_CHANNEL);
}

// ── ST7123 vendor init commands (M5Tab5-UserDemo table, verbatim) ─────────────

static const st7123_lcd_init_cmd_t s_vendor_init[] = {
    {0x60, (uint8_t[]){0x71, 0x23, 0xa2}, 3, 0},
    {0x60, (uint8_t[]){0x71, 0x23, 0xa3}, 3, 0},
    {0x60, (uint8_t[]){0x71, 0x23, 0xa4}, 3, 0},
    {0xA4, (uint8_t[]){0x31}, 1, 0},
    {0xD7, (uint8_t[]){0x10, 0x0A, 0x10, 0x2A, 0x80, 0x80}, 6, 0},
    {0x90, (uint8_t[]){0x71, 0x23, 0x5A, 0x20, 0x24, 0x09, 0x09}, 7, 0},
    {0xA3, (uint8_t[]){0x80, 0x01, 0x88, 0x30, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46, 0x00, 0x00,
                       0x1E, 0x5C, 0x1E, 0x80, 0x00, 0x4F, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46,
                       0x00, 0x00, 0x1E, 0x5C, 0x1E, 0x80, 0x00, 0x6F, 0x58, 0x00, 0x00, 0x00, 0xFF},
     40, 0},
    {0xA6, (uint8_t[]){0x03, 0x00, 0x24, 0x55, 0x36, 0x00, 0x39, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x00, 0x24,
                       0x55, 0x38, 0x00, 0x37, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x00, 0x24, 0x11, 0x00, 0x00,
                       0x00, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x00, 0xEC, 0x11, 0x00, 0x03, 0x00, 0x03, 0x6E,
                       0x6E, 0xFF, 0xFF, 0x00, 0x08, 0x80, 0x08, 0x80, 0x06, 0x00, 0x00, 0x00, 0x00},
     55, 0},
    {0xA7, (uint8_t[]){0x19, 0x19, 0x80, 0x64, 0x40, 0x07, 0x16, 0x40, 0x00, 0x44, 0x03, 0x6E, 0x6E, 0x91, 0xFF,
                       0x08, 0x80, 0x64, 0x40, 0x25, 0x34, 0x40, 0x00, 0x02, 0x01, 0x6E, 0x6E, 0x91, 0xFF, 0x08,
                       0x80, 0x64, 0x40, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x08, 0x80,
                       0x64, 0x40, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x6E, 0x6E, 0x84, 0xFF, 0x08, 0x80, 0x44},
     60, 0},
    {0xAC, (uint8_t[]){0x03, 0x19, 0x19, 0x18, 0x18, 0x06, 0x13, 0x13, 0x11, 0x11, 0x08, 0x08, 0x0A, 0x0A, 0x1C,
                       0x1C, 0x07, 0x07, 0x00, 0x00, 0x02, 0x02, 0x01, 0x19, 0x19, 0x18, 0x18, 0x06, 0x12, 0x12,
                       0x10, 0x10, 0x09, 0x09, 0x0B, 0x0B, 0x1C, 0x1C, 0x07, 0x07, 0x03, 0x03, 0x01, 0x01},
     44, 0},
    {0xAD, (uint8_t[]){0xF0, 0x00, 0x46, 0x00, 0x03, 0x50, 0x50, 0xFF, 0xFF, 0xF0, 0x40, 0x06, 0x01,
                       0x07, 0x42, 0x42, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF},
     25, 0},
    {0xAE, (uint8_t[]){0xFE, 0x3F, 0x3F, 0xFE, 0x3F, 0x3F, 0x00}, 7, 0},
    {0xB2,
     (uint8_t[]){0x15, 0x19, 0x05, 0x23, 0x49, 0xAF, 0x03, 0x2E, 0x5C, 0xD2, 0xFF, 0x10, 0x20, 0xFD, 0x20, 0xC0, 0x00},
     17, 0},
    {0xE8, (uint8_t[]){0x20, 0x6F, 0x04, 0x97, 0x97, 0x3E, 0x04, 0xDC, 0xDC, 0x3E, 0x06, 0xFA, 0x26, 0x3E}, 15, 0},
    {0x75, (uint8_t[]){0x03, 0x04}, 2, 0},
    {0xE7, (uint8_t[]){0x3B, 0x00, 0x00, 0x7C, 0xA1, 0x8C, 0x20, 0x1A, 0xF0, 0xB1, 0x50, 0x00,
                       0x50, 0xB1, 0x50, 0xB1, 0x50, 0xD8, 0x00, 0x55, 0x00, 0xB1, 0x00, 0x45,
                       0xC9, 0x6A, 0xFF, 0x5A, 0xD8, 0x18, 0x88, 0x15, 0xB1, 0x01, 0x01, 0x77},
     36, 0},
    {0xEA, (uint8_t[]){0x13, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x2C}, 8, 0},
    {0xB0, (uint8_t[]){0x22, 0x43, 0x11, 0x61, 0x25, 0x43, 0x43}, 7, 0},
    {0xb7, (uint8_t[]){0x00, 0x00, 0x73, 0x73}, 0x04, 0},
    {0xBF, (uint8_t[]){0xA6, 0XAA}, 2, 0},
    {0xA9, (uint8_t[]){0x00, 0x00, 0x73, 0xFF, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03}, 10, 0},
    {0xC8, (uint8_t[]){0x00, 0x00, 0x10, 0x1F, 0x36, 0x00, 0x5D, 0x04, 0x9D, 0x05, 0x10, 0xF2, 0x06,
                       0x60, 0x03, 0x11, 0xAD, 0x00, 0xEF, 0x01, 0x22, 0x2E, 0x0E, 0x74, 0x08, 0x32,
                       0xDC, 0x09, 0x33, 0x0F, 0xF3, 0x77, 0x0D, 0xB0, 0xDC, 0x03, 0xFF},
     37, 0},
    {0xC9, (uint8_t[]){0x00, 0x00, 0x10, 0x1F, 0x36, 0x00, 0x5D, 0x04, 0x9D, 0x05, 0x10, 0xF2, 0x06,
                       0x60, 0x03, 0x11, 0xAD, 0x00, 0xEF, 0x01, 0x22, 0x2E, 0x0E, 0x74, 0x08, 0x32,
                       0xDC, 0x09, 0x33, 0x0F, 0xF3, 0x77, 0x0D, 0xB0, 0xDC, 0x03, 0xFF},
     37, 0},
    {0x36, (uint8_t[]){0x00}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 100},
    {0x29, (uint8_t[]){0x00}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 100},
};

// ── Dirty-rect bookkeeping ────────────────────────────────────────────────────

static void mark_dirty(int x0, int y0, int x1, int y1)
{
    xSemaphoreTake(s_dirty_mtx, portMAX_DELAY);
    bool was_clean = !s_dirty;
    if (!s_dirty) {
        s_dx0 = x0; s_dy0 = y0; s_dx1 = x1; s_dy1 = y1;
        s_dirty = true;
    } else {
        if (x0 < s_dx0) s_dx0 = x0;
        if (y0 < s_dy0) s_dy0 = y0;
        if (x1 > s_dx1) s_dx1 = x1;
        if (y1 > s_dy1) s_dy1 = y1;
    }
    xSemaphoreGive(s_dirty_mtx);
    if (was_clean) xSemaphoreGive(s_dirty_sem);
}

// ── Flush: PPA rotate dirty back-buffer region into the DPI framebuffer ──────

static void flush_dirty(void)
{
    xSemaphoreTake(s_dirty_mtx, portMAX_DELAY);
    if (!s_dirty) { xSemaphoreGive(s_dirty_mtx); return; }
    int bx = s_dx0, by = s_dy0, bx1 = s_dx1, by1 = s_dy1;
    s_dirty = false;
    xSemaphoreGive(s_dirty_mtx);

    // Round to even coords/sizes — keeps every PPA block RGB565-word aligned
    // regardless of the SRM engine's per-mode block granularity.
    bx &= ~1; by &= ~1;
    if (bx1 >= ST7123_LAND_W) bx1 = ST7123_LAND_W - 1;
    if (by1 >= ST7123_LAND_H) by1 = ST7123_LAND_H - 1;
    int bw = ((bx1 - bx + 1) + 1) & ~1;
    int bh = ((by1 - by + 1) + 1) & ~1;
    if (bx + bw > ST7123_LAND_W) bw = ST7123_LAND_W - bx;
    if (by + bh > ST7123_LAND_H) bh = ST7123_LAND_H - by;
    if (bw <= 0 || bh <= 0) return;

    // CPU wrote the back buffer through cache; the PPA reads physical RAM.
    // Sync whole dirty rows — rows are 2560 B (cache-line multiple), so a
    // row-span sync is always aligned.
    uint8_t *sync_from = (uint8_t *)(s_backbuf + (size_t)by * ST7123_LAND_W);
    size_t   sync_len  = (size_t)bh * ST7123_LAND_W * 2;
    esp_cache_msync(sync_from, sync_len, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

#ifndef ST7123_ROTATE_270
    // 90°: logical (lx,ly) → scaled portrait (px,py) = (S*ly, S*(LAND_W-lx)-1)
    ppa_srm_rotation_angle_t angle = PPA_SRM_ROTATION_ANGLE_90;
    int out_x = ST7123_UI_SCALE * by;
    int out_y = ST7123_UI_SCALE * (ST7123_LAND_W - bx - bw);
#else
    // 270°: logical (lx,ly) → scaled portrait (px,py) = (S*(LAND_H-ly)-1, S*lx)
    ppa_srm_rotation_angle_t angle = PPA_SRM_ROTATION_ANGLE_270;
    int out_x = ST7123_UI_SCALE * (ST7123_LAND_H - by - bh);
    int out_y = ST7123_UI_SCALE * bx;
#endif

    ppa_srm_oper_config_t srm = {
        .in = {
            .buffer         = s_backbuf,
            .pic_w          = ST7123_LAND_W,
            .pic_h          = ST7123_LAND_H,
            .block_w        = (uint32_t)bw,
            .block_h        = (uint32_t)bh,
            .block_offset_x = (uint32_t)bx,
            .block_offset_y = (uint32_t)by,
            .srm_cm         = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer         = s_fb,
            .buffer_size    = (size_t)ST7123_PHYS_W * ST7123_PHYS_H * 2,
            .pic_w          = ST7123_PHYS_W,
            .pic_h          = ST7123_PHYS_H,
            .block_offset_x = (uint32_t)out_x,
            .block_offset_y = (uint32_t)out_y,
            .srm_cm         = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = angle,
        .scale_x        = (float)ST7123_UI_SCALE,
        .scale_y        = (float)ST7123_UI_SCALE,
        .mode           = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa, &srm);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ppa rotate failed: %s (rect %d,%d %dx%d)",
                 esp_err_to_name(err), bx, by, bw, bh);
        return;
    }

    if (!s_bl_on) {
        // First real frame is on the panel — now light it (avoids the white/
        // garbage flash a backlight-at-init would show).
        bl_set(s_brightness);
        s_bl_on = true;
    }
}

static void flush_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_dirty_sem, portMAX_DELAY);
        // Coalesce: let the current burst of draw calls finish before rotating.
        vTaskDelay(pdMS_TO_TICKS(FLUSH_COALESCE_MS));
        flush_dirty();
    }
}

// ── Display catcall ───────────────────────────────────────────────────────────

static esp_err_t st7123_disp_init(const display_config_t *cfg)
{
    (void)cfg;   // fixed wiring; nothing to override
    if (s_ready) return ESP_OK;

    // System I2C bus (shared: IO expanders + touch)
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = TAB5_I2C_PORT,
        .sda_io_num        = TAB5_I2C_SDA,
        .scl_io_num        = TAB5_I2C_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "i2c bus");

    // IO expanders first — they hold LCD_RST/TP_RST; nothing answers until
    // this has run.
    ESP_RETURN_ON_ERROR(pi4ioe_init(), TAG, "pi4ioe");

    // DSI PHY power
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = ST7123_LDO_CHAN,
        .voltage_mv = ST7123_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &s_phy_ldo), TAG, "phy ldo");

    bl_init();   // configured but duty 0 — lit after the first flush

    // DSI bus + DBI command channel
    esp_lcd_dsi_bus_config_t dsi_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = ST7123_DSI_LANES,
        .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = ST7123_DSI_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&dsi_cfg, &s_dsi_bus), TAG, "dsi bus");

    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_cfg, &s_dbi_io), TAG, "dbi io");

    // DPI pixel pipeline — native portrait timing
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel    = 0,
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = ST7123_DPI_CLK_MHZ,
        .pixel_format       = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs            = 1,
        .video_timing = {
            .h_size            = ST7123_PHYS_W,
            .v_size            = ST7123_PHYS_H,
            .hsync_pulse_width = 2,
            .hsync_back_porch  = 40,
            .hsync_front_porch = 40,
            .vsync_pulse_width = 2,
            .vsync_back_porch  = 8,
            .vsync_front_porch = 220,
        },
    };
    st7123_vendor_config_t vendor_cfg = {
        .init_cmds      = s_vendor_init,
        .init_cmds_size = sizeof(s_vendor_init) / sizeof(s_vendor_init[0]),
        .mipi_config = {
            .dsi_bus    = s_dsi_bus,
            .dpi_config = &dpi_cfg,
            .lane_num   = ST7123_DSI_LANES,
        },
    };
    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = -1,                          // reset via IO expander
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian    = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 24,
        .vendor_config  = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7123(s_dbi_io, &dev_cfg, &s_panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on");

    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, (void **)&s_fb),
                        TAG, "get fb");

    // Landscape back buffer — 1280×720×2 ≈ 1.8 MB in PSRAM. 128-byte aligned
    // (P4 L2 cache line) so row-span cache syncs stay aligned.
    s_backbuf = heap_caps_aligned_calloc(128, (size_t)ST7123_LAND_W * ST7123_LAND_H, 2,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s_backbuf) {
        ESP_LOGE(TAG, "back buffer alloc failed (%d KB PSRAM)",
                 ST7123_LAND_W * ST7123_LAND_H * 2 / 1024);
        return ESP_ERR_NO_MEM;
    }

    ppa_client_config_t ppa_cfg = { .oper_type = PPA_OPERATION_SRM };
    ESP_RETURN_ON_ERROR(ppa_register_client(&ppa_cfg, &s_ppa), TAG, "ppa client");

    s_dirty_mtx = xSemaphoreCreateMutex();
    s_dirty_sem = xSemaphoreCreateBinary();
    if (!s_dirty_mtx || !s_dirty_sem) return ESP_ERR_NO_MEM;
    if (xTaskCreate(flush_task, "st7123_flush", 4096, NULL, 5, &s_flush_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;

    // Push one black frame so the first backlight-on shows a clean screen.
    mark_dirty(0, 0, ST7123_LAND_W - 1, ST7123_LAND_H - 1);

    ESP_LOGI(TAG, "ST7123 ready %dx%d landscape (PPA %dx scale + rotate onto %dx%d)",
             ST7123_LAND_W, ST7123_LAND_H, ST7123_UI_SCALE,
             ST7123_PHYS_W, ST7123_PHYS_H);
    return ESP_OK;
}

static esp_err_t st7123_push_pixels(int x, int y, int w, int h, const uint16_t *data)
{
    if (!s_ready || !data || w <= 0 || h <= 0) return ESP_ERR_INVALID_STATE;

    // Clip to the landscape surface
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w - 1, y1 = y + h - 1;
    if (x1 >= ST7123_LAND_W) x1 = ST7123_LAND_W - 1;
    if (y1 >= ST7123_LAND_H) y1 = ST7123_LAND_H - 1;
    if (x0 > x1 || y0 > y1) return ESP_OK;

    for (int row = y0; row <= y1; row++) {
        const uint16_t *src = data + (size_t)(row - y) * w + (x0 - x);
        memcpy(s_backbuf + (size_t)row * ST7123_LAND_W + x0,
               src, (size_t)(x1 - x0 + 1) * 2);
    }
    mark_dirty(x0, y0, x1, y1);
    return ESP_OK;
}

static esp_err_t st7123_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_ready || w <= 0 || h <= 0) return ESP_ERR_INVALID_STATE;

    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w - 1, y1 = y + h - 1;
    if (x1 >= ST7123_LAND_W) x1 = ST7123_LAND_W - 1;
    if (y1 >= ST7123_LAND_H) y1 = ST7123_LAND_H - 1;
    if (x0 > x1 || y0 > y1) return ESP_OK;

    int span = x1 - x0 + 1;
    uint16_t *first = s_backbuf + (size_t)y0 * ST7123_LAND_W + x0;
    for (int i = 0; i < span; i++) first[i] = color;
    for (int row = y0 + 1; row <= y1; row++) {
        memcpy(s_backbuf + (size_t)row * ST7123_LAND_W + x0, first, (size_t)span * 2);
    }
    mark_dirty(x0, y0, x1, y1);
    return ESP_OK;
}

static esp_err_t st7123_set_brightness(uint8_t level)
{
    s_brightness = level;
    if (s_bl_on) bl_set(level);   // else applied at first flush
    return ESP_OK;
}

static void st7123_get_info(display_info_t *out)
{
    if (!out) return;
    out->width          = ST7123_LAND_W;
    out->height         = ST7123_LAND_H;
    out->bits_per_pixel = 16;
    strncpy(out->name, "ST7123 DSI", sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = '\0';
}

static esp_err_t st7123_disp_deinit(void)
{
    if (!s_ready) return ESP_OK;
    s_ready = false;
    bl_set(0);
    if (s_flush_task) { vTaskDelete(s_flush_task); s_flush_task = NULL; }
    if (s_ppa)        { ppa_unregister_client(s_ppa); s_ppa = NULL; }
    if (s_backbuf)    { free(s_backbuf); s_backbuf = NULL; }
    if (s_panel)      { esp_lcd_panel_del(s_panel); s_panel = NULL; }
    if (s_dbi_io)     { esp_lcd_panel_io_del(s_dbi_io); s_dbi_io = NULL; }
    if (s_dsi_bus)    { esp_lcd_del_dsi_bus(s_dsi_bus); s_dsi_bus = NULL; }
    if (s_phy_ldo)    { esp_ldo_release_channel(s_phy_ldo); s_phy_ldo = NULL; }
    return ESP_OK;
}

// ── Touch catcall ─────────────────────────────────────────────────────────────

static esp_err_t st7123_touch_init(const touch_config_t *cfg)
{
    (void)cfg;   // shares the fixed system I2C bus the display half owns
    if (s_touch) return ESP_OK;
    if (!s_i2c_bus) return ESP_ERR_INVALID_STATE;   // display init runs first (both P1)

    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr            = TAB5_TOUCH_ADDR,
        .control_phase_bytes = 1,
        .dc_bit_offset       = 0,
        .lcd_cmd_bits        = 16,
        .scl_speed_hz        = 400000,
        .flags = { .disable_control_phase = 1 },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c_v2(s_i2c_bus, &io_cfg, &s_touch_io),
                        TAG, "touch io");

    // int_gpio_num deliberately NOT wired (vendor wires GPIO23) — PURR's touch
    // HAL polls read_point, and a GPIO ISR here would be pure liability given
    // the known P4 GPIO-ISR/esp-hosted coexistence problem (see tab5_kbd).
    esp_lcd_touch_config_t tp_cfg = {
        .x_max        = ST7123_PHYS_W,     // reports native portrait
        .y_max        = ST7123_PHYS_H,
        .rst_gpio_num = GPIO_NUM_NC,       // reset via IO expander TP_RST
        .int_gpio_num = GPIO_NUM_NC,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags  = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    esp_err_t err = esp_lcd_touch_new_i2c_st7123(s_touch_io, &tp_cfg, &s_touch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch init failed: %s", esp_err_to_name(err));
        esp_lcd_panel_io_del(s_touch_io);
        s_touch_io = NULL;
        return err;
    }
    ESP_LOGI(TAG, "ST7123 touch ready (addr 0x%02X)", TAB5_TOUCH_ADDR);
    return ESP_OK;
}

static bool st7123_touch_read_point(uint16_t *x, uint16_t *y)
{
    if (!s_touch || !x || !y) return false;

    uint16_t rx[1], ry[1], strength[1];
    uint8_t  cnt = 0;
    if (esp_lcd_touch_read_data(s_touch) != ESP_OK) return false;
    if (!esp_lcd_touch_get_coordinates(s_touch, rx, ry, strength, &cnt, 1) || cnt == 0) {
        return false;
    }

    // Portrait-native (rx∈[0,720), ry∈[0,1280)) → logical landscape pixels:
    // the exact inverse of the display's scale+rotate mapping so touch lands
    // where pixels drew. Contract requires final screen space (see gt911.c).
#ifndef ST7123_ROTATE_270
    *x = (uint16_t)((ST7123_PHYS_H - 1 - ry[0]) / ST7123_UI_SCALE);
    *y = (uint16_t)(rx[0] / ST7123_UI_SCALE);
#else
    *x = (uint16_t)(ry[0] / ST7123_UI_SCALE);
    *y = (uint16_t)((ST7123_PHYS_W - 1 - rx[0]) / ST7123_UI_SCALE);
#endif
    if (*x >= ST7123_LAND_W) *x = ST7123_LAND_W - 1;
    if (*y >= ST7123_LAND_H) *y = ST7123_LAND_H - 1;
    return true;
}

static bool st7123_touch_is_pressed(void)
{
    uint16_t x, y;
    return st7123_touch_read_point(&x, &y);
}

static esp_err_t st7123_touch_deinit(void)
{
    if (s_touch)    { esp_lcd_touch_del(s_touch); s_touch = NULL; }
    if (s_touch_io) { esp_lcd_panel_io_del(s_touch_io); s_touch_io = NULL; }
    return ESP_OK;
}

// ── Catcall descriptors ───────────────────────────────────────────────────────

static const catcall_display_t s_disp_catcall = {
    .name            = "st7123",
    .catcall_version = CATCALL_DISPLAY_VERSION,
    .init            = st7123_disp_init,
    .push_pixels     = st7123_push_pixels,
    .fill_rect       = st7123_fill_rect,
    .set_brightness  = st7123_set_brightness,
    .get_info        = st7123_get_info,
    .deinit          = st7123_disp_deinit,
};

static const catcall_touch_t s_touch_catcall = {
    .name            = "st7123",
    .catcall_version = CATCALL_TOUCH_VERSION,
    .init            = st7123_touch_init,
    .read_point      = st7123_touch_read_point,
    .is_pressed      = st7123_touch_is_pressed,
    .deinit          = st7123_touch_deinit,
};

// ── Module lifecycle ──────────────────────────────────────────────────────────
// device.pcat lists st7123 as both display= and touch=, so the glue registers
// this module twice — the s_ready/s_touch guards make the second init a no-op
// (same tolerated pattern as axs15231b on jc3248w535).

static int st7123_drv_init(void)
{
    esp_err_t ret = st7123_disp_init(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(ret));
        return -1;
    }
    purr_kernel_register_display(&s_disp_catcall);

    // Touch is best-effort: a dead digitizer shouldn't panic the boot (this
    // device still has the keyboard).
    if (st7123_touch_init(NULL) == ESP_OK) {
        purr_kernel_register_touch(&s_touch_catcall);
    } else {
        ESP_LOGW(TAG, "touch unavailable — continuing display-only");
    }
    return 0;
}

static void st7123_drv_deinit(void)
{
    st7123_touch_deinit();
    st7123_disp_deinit();
}

PURR_MODULE_REGISTER(st7123) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .load_priority     = PURR_PRIORITY_REQUIRED,
    .name              = "st7123",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = CATCALL_FLAG_DISPLAY | CATCALL_FLAG_TOUCH,
    .required_catcalls = 0,
    .init              = st7123_drv_init,
    .deinit            = st7123_drv_deinit,
};
