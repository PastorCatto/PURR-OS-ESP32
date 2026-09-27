// axs15231b.c — AXS15231B QSPI display driver for PURR OS
// Board: JC3248W535 (480x320, RGB565)
// CS=45, PCLK=47, D0=21, D1=48, D2=40, D3=39, BL=1
//
// ESP-IDF v5.x, QSPI panel IO via esp_lcd APIs.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#include "../../kernel/catcalls/catcall_display.h"
#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_kernel.h"

// ── Board pin defaults ────────────────────────────────────────────────────────

#define AXS_PIN_CS    45
#define AXS_PIN_PCLK  47
#define AXS_PIN_D0    21
#define AXS_PIN_D1    48
#define AXS_PIN_D2    40
#define AXS_PIN_D3    39
#define AXS_PIN_BL     1

#define AXS_WIDTH    480
#define AXS_HEIGHT   320

// LEDC channel for backlight PWM
#define AXS_LEDC_TIMER    LEDC_TIMER_0
#define AXS_LEDC_CHANNEL  LEDC_CHANNEL_0
#define AXS_LEDC_FREQ_HZ  5000
#define AXS_LEDC_RES      LEDC_TIMER_8_BIT   // 0-255

// SPI host used for QSPI panel
#define AXS_SPI_HOST      SPI2_HOST

static const char *TAG = "axs15231b";

// ── Forward declarations ──────────────────────────────────────────────────────

static esp_err_t axs15231b_init(const display_config_t *cfg);
static esp_err_t axs15231b_push_pixels(int x, int y, int w, int h, const uint16_t *data);
static esp_err_t axs15231b_fill_rect(int x, int y, int w, int h, uint16_t color);
static esp_err_t axs15231b_set_brightness(uint8_t level);
static void      axs15231b_get_info(display_info_t *out);
static esp_err_t axs15231b_deinit(void);

// ── Module state ──────────────────────────────────────────────────────────────

static esp_lcd_panel_io_handle_t s_io_handle   = NULL;
static esp_lcd_panel_handle_t    s_panel_handle = NULL;
static uint16_t                  s_bl_pin       = AXS_PIN_BL;
static bool                      s_initialized  = false;

// ── AXS15231B init sequence ───────────────────────────────────────────────────
// The AXS15231B uses a 3-byte command prefix on QSPI:
//   byte0=0x02, byte1=0x00, byte2=<cmd>
// Then data bytes follow.
// We drive this manually via esp_lcd_panel_io_tx_param().

// Helper: send a command + data bytes via panel IO
static esp_err_t axs_send_cmd(uint8_t cmd, const uint8_t *data, size_t len)
{
    // panel IO cmd_bits=8, the cmd byte is the 3rd byte of the prefix.
    // We pack the prefix into the "command" field as a 24-bit value:
    //   [23:16]=0x02  [15:8]=0x00  [7:0]=cmd
    // Set cmd_bits to 24 in panel IO config to achieve this.
    return esp_lcd_panel_io_tx_param(s_io_handle, cmd, data, len);
}

static esp_err_t axs15231b_run_init_sequence(void)
{
    esp_err_t ret;

    // Sleep out
    ret = axs_send_cmd(0x11, NULL, 0);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(120));

    // Pixel format: 0x55 = 16bpp RGB565 for both DBI and DPI
    const uint8_t pf[] = { 0x55 };
    ret = axs_send_cmd(0x3A, pf, sizeof(pf));
    if (ret != ESP_OK) return ret;

    // MADCTL: 0x00 = normal portrait
    const uint8_t madctl[] = { 0x00 };
    ret = axs_send_cmd(0x36, madctl, sizeof(madctl));
    if (ret != ESP_OK) return ret;

    // Set column address (full width 0..479)
    const uint8_t caset[] = { 0x00, 0x00, 0x01, 0xDF };
    ret = axs_send_cmd(0x2A, caset, sizeof(caset));
    if (ret != ESP_OK) return ret;

    // Set row address (full height 0..319)
    const uint8_t raset[] = { 0x00, 0x00, 0x01, 0x3F };
    ret = axs_send_cmd(0x2B, raset, sizeof(raset));
    if (ret != ESP_OK) return ret;

    // Display on
    ret = axs_send_cmd(0x29, NULL, 0);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(20));

    return ESP_OK;
}

// ── Backlight via LEDC ────────────────────────────────────────────────────────

static esp_err_t axs15231b_ledc_init(uint16_t bl_pin)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = AXS_LEDC_RES,
        .timer_num       = AXS_LEDC_TIMER,
        .freq_hz         = AXS_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "LEDC timer config failed");

    ledc_channel_config_t ch_cfg = {
        .gpio_num   = (int)bl_pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = AXS_LEDC_CHANNEL,
        .timer_sel  = AXS_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_cfg), TAG, "LEDC channel config failed");
    return ESP_OK;
}

// ── catcall_display_t implementation ──────────────────────────────────────────

static esp_err_t axs15231b_init(const display_config_t *cfg)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    s_bl_pin = (cfg && cfg->backlight_pin != 0) ? cfg->backlight_pin : AXS_PIN_BL;

    // Install SPI bus (QSPI — 4 data lines)
    spi_bus_config_t bus_cfg = {
        .sclk_io_num     = AXS_PIN_PCLK,
        .data0_io_num    = AXS_PIN_D0,
        .data1_io_num    = AXS_PIN_D1,
        .data2_io_num    = AXS_PIN_D2,
        .data3_io_num    = AXS_PIN_D3,
        .mosi_io_num     = -1,
        .miso_io_num     = -1,
        .quadhd_io_num   = -1,
        .quadwp_io_num   = -1,
        .max_transfer_sz = AXS_WIDTH * AXS_HEIGHT * 2 + 8,
        .flags           = SPICOMMON_BUSFLAG_MASTER,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(AXS_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    // Panel IO: QSPI, cmd_bits=24 to carry the 3-byte AXS prefix
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num         = AXS_PIN_CS,
        .dc_gpio_num         = -1,           // no D/C line in QSPI mode
        .lcd_cmd_bits        = 24,           // 0x02 0x00 <cmd>
        .lcd_param_bits      = 8,
        .flags = {
            .quad_mode = true,               // SPI_TRANS_MODE_QIO
        },
        .on_color_trans_done = NULL,
        .user_ctx            = NULL,
        .pclk_hz             = 80 * 1000 * 1000,
        .trans_queue_depth   = 10,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)AXS_SPI_HOST,
                                 &io_cfg, &s_io_handle),
        TAG, "panel IO init failed");

    // The AXS15231B does not have a vendor-specific component in esp-idf yet.
    // We use the generic RGB/SPI panel ops stub — init sequence is run manually.
    // We only need the panel handle for draw_bitmap; create a minimal one.
    // Use esp_lcd_new_panel_st7789 as structural stand-in and override via IO calls.
    // If the project ships its own axs15231b panel component, swap this out.

    // For now: no panel_handle needed — we drive everything through s_io_handle.
    // draw_bitmap is implemented directly via panel IO write_color.

    // Run hardware init sequence
    ESP_RETURN_ON_ERROR(axs15231b_run_init_sequence(), TAG, "init sequence failed");

    // Backlight
    ESP_RETURN_ON_ERROR(axs15231b_ledc_init(s_bl_pin), TAG, "backlight init failed");
    ledc_set_duty(LEDC_LOW_SPEED_MODE, AXS_LEDC_CHANNEL, 200);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, AXS_LEDC_CHANNEL);

    s_initialized = true;
    ESP_LOGI(TAG, "AXS15231B initialized (%dx%d)", AXS_WIDTH, AXS_HEIGHT);
    purr_kernel_register_display(&(catcall_display_t){
        .name            = "axs15231b",
        .catcall_version = CATCALL_DISPLAY_VERSION,
        .init            = axs15231b_init,
        .push_pixels     = axs15231b_push_pixels,
        .fill_rect       = axs15231b_fill_rect,
        .set_brightness  = axs15231b_set_brightness,
        .get_info        = axs15231b_get_info,
        .deinit          = axs15231b_deinit,
    });
    return ESP_OK;
}

// Set column/row window and begin write
static esp_err_t axs15231b_set_window(int x, int y, int x2, int y2)
{
    const uint8_t caset[] = {
        (uint8_t)(x  >> 8), (uint8_t)(x  & 0xFF),
        (uint8_t)(x2 >> 8), (uint8_t)(x2 & 0xFF),
    };
    const uint8_t raset[] = {
        (uint8_t)(y  >> 8), (uint8_t)(y  & 0xFF),
        (uint8_t)(y2 >> 8), (uint8_t)(y2 & 0xFF),
    };
    ESP_RETURN_ON_ERROR(axs_send_cmd(0x2A, caset, sizeof(caset)), TAG, "CASET failed");
    ESP_RETURN_ON_ERROR(axs_send_cmd(0x2B, raset, sizeof(raset)), TAG, "RASET failed");
    return ESP_OK;
}

static esp_err_t axs15231b_push_pixels(int x, int y, int w, int h, const uint16_t *data)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(axs15231b_set_window(x, y, x + w - 1, y + h - 1),
                        TAG, "set_window failed");
    // RAM write command, then pixel data
    return esp_lcd_panel_io_tx_color(s_io_handle, 0x2C, data, (size_t)w * h * 2);
}

static esp_err_t axs15231b_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    // Allocate a single-row buffer
    uint16_t *row = malloc((size_t)w * sizeof(uint16_t));
    if (!row) return ESP_ERR_NO_MEM;
    for (int i = 0; i < w; i++) row[i] = color;

    ESP_RETURN_ON_ERROR(axs15231b_set_window(x, y, x + w - 1, y + h - 1),
                        TAG, "set_window failed");

    // Send RAMWR once, then push rows
    esp_err_t ret = axs_send_cmd(0x2C, NULL, 0);
    for (int row_idx = 0; row_idx < h && ret == ESP_OK; row_idx++) {
        ret = esp_lcd_panel_io_tx_color(s_io_handle, -1, row, (size_t)w * 2);
    }
    free(row);
    return ret;
}

static esp_err_t axs15231b_set_brightness(uint8_t level)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, AXS_LEDC_CHANNEL, level);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, AXS_LEDC_CHANNEL);
    return ESP_OK;
}

static void axs15231b_get_info(display_info_t *out)
{
    if (!out) return;
    out->width         = AXS_WIDTH;
    out->height        = AXS_HEIGHT;
    out->bits_per_pixel = 16;
    strncpy(out->name, "AXS15231B", sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = '\0';
}

static esp_err_t axs15231b_deinit(void)
{
    if (!s_initialized) return ESP_OK;
    ledc_stop(LEDC_LOW_SPEED_MODE, AXS_LEDC_CHANNEL, 0);
    esp_lcd_panel_io_del(s_io_handle);
    s_io_handle  = NULL;
    spi_bus_free(AXS_SPI_HOST);
    s_initialized = false;
    return ESP_OK;
}

// ── Static catcall descriptor ─────────────────────────────────────────────────

static const catcall_display_t s_catcall = {
    .name            = "axs15231b",
    .catcall_version = CATCALL_DISPLAY_VERSION,
    .init            = axs15231b_init,
    .push_pixels     = axs15231b_push_pixels,
    .fill_rect       = axs15231b_fill_rect,
    .set_brightness  = axs15231b_set_brightness,
    .get_info        = axs15231b_get_info,
    .deinit          = axs15231b_deinit,
};

// ── Module lifecycle ──────────────────────────────────────────────────────────

static int axs15231b_drv_init(void)
{
    display_config_t default_cfg = {
        .backlight_pin = AXS_PIN_BL,
        .rotation      = 0,
        .flags         = 0,
    };
    esp_err_t ret = axs15231b_init(&default_cfg);
    if (ret != ESP_OK) return -1;
    purr_kernel_register_display(&s_catcall);
    return 0;
}

static void axs15231b_drv_deinit(void)
{
    axs15231b_deinit();
}

// ── PURR module header ────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(axs15231b) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .load_priority     = PURR_PRIORITY_REQUIRED,
    .name              = "axs15231b",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = CATCALL_FLAG_DISPLAY,
    .required_catcalls = 0,
    .init              = axs15231b_drv_init,
    .deinit            = axs15231b_drv_deinit,
};
