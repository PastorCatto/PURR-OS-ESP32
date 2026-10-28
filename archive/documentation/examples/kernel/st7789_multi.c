/*
 * ST7789 display driver, providing the native `display` catcall (v2.0), MULTI-INSTANCE version.
 *
 * Same driver as the single-display original (data-driven init table, board profile supplies
 * size/offsets/orientation/inversion), but the state lives in a per-slot struct instead of one
 * file-static, so two ST7789 panels can run at once (documentation/15-adding-a-second-display.md).
 *
 * purr_display_v2_t's function pointers take no context argument, so each slot needs its own set
 * of functions. SLOT_FUNCS(n) generates them with the slot number baked in. To support more than
 * two slots, add SLOT_FUNCS(2) ... and extend the table at the bottom of the function section.
 */
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "purr_board.h"
#include "purr_display.h"
#include "purr_kernel.h"

static const char *TAG = "st7789";

#define CHUNK_LINES   16                     /* lines sent per SPI transaction */
#define MAX_SLOTS     2

/* Commands */
#define CMD_SLPOUT    0x11
#define CMD_NORON     0x13
#define CMD_INVON     0x21
#define CMD_DISPON    0x29
#define CMD_CASET     0x2A
#define CMD_RASET     0x2B
#define CMD_RAMWR     0x2C
#define CMD_MADCTL    0x36

typedef struct {
    uint8_t cmd;
    uint8_t delay_ms;
    uint8_t len;
    uint8_t data[14];
} step_t;

#define MADCTL_STEP_INDEX 3                  /* the step that takes its argument from the profile */

static const step_t s_init[] = {
    {0x01, 150, 0, {0}},                                          /* software reset */
    {CMD_SLPOUT, 10, 0, {0}},                                     /* sleep out */
    {0x3A, 10, 1, {0x55}},                                        /* 16 bits per pixel */
    {CMD_MADCTL, 0, 1, {0}},                                      /* orientation: from the profile */
    {0xB2, 0, 5, {0x0C, 0x0C, 0x00, 0x33, 0x33}},                 /* porch */
    {0xB7, 0, 1, {0x35}},                                         /* gate control */
    {0xBB, 0, 1, {0x28}},                                         /* VCOM */
    {0xC0, 0, 1, {0x0C}},                                         /* LCM control */
    {0xC2, 0, 1, {0x01}},                                         /* VDV and VRH enable */
    {0xC3, 0, 1, {0x13}},                                         /* VRH */
    {0xC4, 0, 1, {0x20}},                                         /* VDV */
    {0xC6, 0, 1, {0x0F}},                                         /* 60 Hz */
    {0xD0, 0, 2, {0xA4, 0xA1}},                                   /* power control */
    {0xE0, 0, 14, {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32, 0x44,
                   0x42, 0x06, 0x0E, 0x12, 0x14, 0x17}},          /* positive gamma */
    {0xE1, 0, 14, {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31, 0x54,
                   0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E}},          /* negative gamma */
};

typedef struct {
    spi_device_handle_t dev;
    const purr_display_cfg_t *cfg;
    uint8_t *dma;                            /* one chunk of pixel bytes, DMA-capable */
    uint32_t dma_bytes;
    int backlight_ready;
    int slot;
    purr_display_v2_t ops;                   /* the table handed to callers for this slot */
} st_dev_t;

static st_dev_t g_dev[MAX_SLOTS];

/* ---------------------------------------------------------------- SPI */

/* One of these lives on the caller's stack for the duration of a (blocking) transaction. */
typedef struct {
    int dc_pin;
    int dc_level;
} dc_info_t;

static void IRAM_ATTR pre_cb(spi_transaction_t *t)
{
    const dc_info_t *u = t->user;
    gpio_set_level((gpio_num_t)u->dc_pin, u->dc_level);
}

static esp_err_t send(st_dev_t *d, const void *buf, size_t len, int dc)
{
    if (len == 0) {
        return ESP_OK;
    }
    dc_info_t u = {.dc_pin = d->cfg->dc, .dc_level = dc};
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = buf,
        .user = &u,
    };
    return spi_device_transmit(d->dev, &t);
}

static esp_err_t cmd(st_dev_t *d, uint8_t c)
{
    return send(d, &c, 1, 0);
}

static esp_err_t cmd_data(st_dev_t *d, uint8_t c, const uint8_t *data, size_t n)
{
    esp_err_t e = cmd(d, c);
    return (e == ESP_OK && n) ? send(d, data, n, 1) : e;
}

static esp_err_t set_window(st_dev_t *d, int x0, int y0, int x1, int y1)
{
    x0 += d->cfg->col_off; x1 += d->cfg->col_off;
    y0 += d->cfg->row_off; y1 += d->cfg->row_off;
    uint8_t col[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
    uint8_t row[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
    esp_err_t e = cmd_data(d, CMD_CASET, col, 4);
    if (e == ESP_OK) e = cmd_data(d, CMD_RASET, row, 4);
    if (e == ESP_OK) e = cmd(d, CMD_RAMWR);
    return e;
}

/* ------------------------------------------------------------ catcall */

static esp_err_t clip(st_dev_t *d, int *x, int *y, int *w, int *h, int *skip_x, int *skip_y)
{
    int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > d->cfg->width) x1 = d->cfg->width;
    if (y1 > d->cfg->height) y1 = d->cfg->height;
    if (x1 <= x0 || y1 <= y0) {
        return ESP_ERR_NOT_FINISHED;         /* nothing visible: succeeds without doing anything */
    }
    *skip_x = x0 - *x;
    *skip_y = y0 - *y;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return ESP_OK;
}

static esp_err_t do_get_info(st_dev_t *d, purr_display_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->width = d->cfg->width;
    out->height = d->cfg->height;
    out->max_chunk_pixels = (uint16_t)(d->dma_bytes / 2);
    strncpy(out->name, d->cfg->name, sizeof(out->name) - 1);
    return ESP_OK;
}

static esp_err_t do_blit(st_dev_t *d, int x, int y, int w, int h, const uint16_t *px)
{
    int full_w = w, sx = 0, sy = 0;
    esp_err_t e = clip(d, &x, &y, &w, &h, &sx, &sy);
    if (e == ESP_ERR_NOT_FINISHED) {
        return ESP_OK;
    }
    e = set_window(d, x, y, x + w - 1, y + h - 1);
    int lines_per_chunk = (int)(d->dma_bytes / 2) / w;
    if (lines_per_chunk < 1) lines_per_chunk = 1;
    for (int row = 0; row < h && e == ESP_OK; row += lines_per_chunk) {
        int n = h - row < lines_per_chunk ? h - row : lines_per_chunk;
        for (int l = 0; l < n; l++) {
            const uint16_t *src = px + (size_t)(sy + row + l) * full_w + sx;
            uint8_t *dst = d->dma + (size_t)l * w * 2;
            for (int i = 0; i < w; i++) {
                dst[2 * i] = (uint8_t)(src[i] >> 8);        /* the panel wants big-endian */
                dst[2 * i + 1] = (uint8_t)src[i];
            }
        }
        e = send(d, d->dma, (size_t)n * w * 2, 1);
    }
    return e;
}

static esp_err_t do_fill(st_dev_t *d, int x, int y, int w, int h, uint16_t color)
{
    int sx, sy;
    esp_err_t e = clip(d, &x, &y, &w, &h, &sx, &sy);
    if (e == ESP_ERR_NOT_FINISHED) {
        return ESP_OK;
    }
    e = set_window(d, x, y, x + w - 1, y + h - 1);
    uint32_t total = (uint32_t)w * h;
    uint32_t per = d->dma_bytes / 2;
    for (uint32_t i = 0; i < per; i++) {
        d->dma[2 * i] = (uint8_t)(color >> 8);
        d->dma[2 * i + 1] = (uint8_t)color;
    }
    while (total && e == ESP_OK) {
        uint32_t n = total < per ? total : per;
        e = send(d, d->dma, (size_t)n * 2, 1);
        total -= n;
    }
    return e;
}

static esp_err_t do_set_brightness(st_dev_t *d, uint8_t level)
{
    if (!d->backlight_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    ledc_channel_t ch = (ledc_channel_t)(LEDC_CHANNEL_2 + d->slot);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, level);
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
}

/* The public function table has no context pointer, so each slot gets its own wrappers. */
#define SLOT_FUNCS(n)                                                                         \
    static esp_err_t get_info_##n(purr_display_info_t *o) { return do_get_info(&g_dev[n], o); } \
    static esp_err_t blit_##n(int x, int y, int w, int h, const uint16_t *p)                  \
    { return do_blit(&g_dev[n], x, y, w, h, p); }                                             \
    static esp_err_t fill_##n(int x, int y, int w, int h, uint16_t c)                         \
    { return do_fill(&g_dev[n], x, y, w, h, c); }                                             \
    static esp_err_t brightness_##n(uint8_t l) { return do_set_brightness(&g_dev[n], l); }

SLOT_FUNCS(0)
SLOT_FUNCS(1)

typedef struct {
    esp_err_t (*get_info)(purr_display_info_t *);
    esp_err_t (*blit)(int, int, int, int, const uint16_t *);
    esp_err_t (*fill)(int, int, int, int, uint16_t);
    esp_err_t (*brightness)(uint8_t);
} slot_funcs_t;

static const slot_funcs_t s_slot_funcs[MAX_SLOTS] = {
    {get_info_0, blit_0, fill_0, brightness_0},
    {get_info_1, blit_1, fill_1, brightness_1},
};

/* --------------------------------------------------------------- init */

static void backlight_init(st_dev_t *d, int pin)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_2,
        .duty_resolution = LEDC_TIMER_8_BIT, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t c = {
        .gpio_num = pin, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = (ledc_channel_t)(LEDC_CHANNEL_2 + d->slot),
        .timer_sel = LEDC_TIMER_2, .duty = 0, .hpoint = 0,
    };
    if (ledc_timer_config(&t) == ESP_OK && ledc_channel_config(&c) == ESP_OK) {
        d->backlight_ready = 1;
        d->ops.set_brightness = s_slot_funcs[d->slot].brightness;
        d->ops.features |= PURR_DISPLAY_F_BRIGHTNESS;
    }
}

esp_err_t purr_st7789_init_slot(int slot, const purr_display_cfg_t *cfg, int spi_host,
                                const purr_display_v2_t **out)
{
    if (slot < 0 || slot >= MAX_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    st_dev_t *d = &g_dev[slot];
    memset(d, 0, sizeof(*d));
    d->slot = slot;
    d->cfg = cfg;
    d->dma_bytes = (uint32_t)cfg->width * CHUNK_LINES * 2;
    d->dma = heap_caps_malloc(d->dma_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!d->dma) {
        return ESP_ERR_NO_MEM;
    }

    d->ops = (purr_display_v2_t){
        .struct_size = sizeof(purr_display_v2_t),
        .major = PURR_DISPLAY_MAJOR,
        .minor = PURR_DISPLAY_MINOR,
        .features = 0,
        .get_info = s_slot_funcs[slot].get_info,
        .blit = s_slot_funcs[slot].blit,
        .fill = s_slot_funcs[slot].fill,
        .set_brightness = NULL,
        .set_power = NULL,
    };

    gpio_config_t io = {.pin_bit_mask = 1ULL << cfg->dc, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);

    spi_device_interface_config_t dc = {
        .clock_speed_hz = (int)cfg->spi_hz,
        .mode = 0,
        .spics_io_num = cfg->cs,
        .queue_size = 2,
        .pre_cb = pre_cb,
    };
    esp_err_t e = spi_bus_add_device((spi_host_device_t)spi_host, &dc, &d->dev);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "slot %d: add device: %s", slot, esp_err_to_name(e));
        return e;
    }

    if (cfg->rst != PURR_PIN_NONE) {
        gpio_config_t r = {.pin_bit_mask = 1ULL << cfg->rst, .mode = GPIO_MODE_OUTPUT};
        gpio_config(&r);
        gpio_set_level((gpio_num_t)cfg->rst, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level((gpio_num_t)cfg->rst, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    for (unsigned i = 0; i < sizeof(s_init) / sizeof(s_init[0]) && e == ESP_OK; i++) {
        uint8_t data[14];
        memcpy(data, s_init[i].data, sizeof(data));
        if (i == MADCTL_STEP_INDEX) {
            data[0] = cfg->madctl | (cfg->bgr ? 0x08 : 0x00);   /* bit 3 = BGR order */
        }
        e = cmd_data(d, s_init[i].cmd, data, s_init[i].len);
        if (s_init[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(s_init[i].delay_ms));
        }
    }
    if (e == ESP_OK && cfg->invert) e = cmd(d, CMD_INVON);
    if (e == ESP_OK) e = cmd(d, CMD_NORON);
    vTaskDelay(pdMS_TO_TICKS(10));
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "slot %d: init: %s", slot, esp_err_to_name(e));
        return e;
    }

    do_fill(d, 0, 0, cfg->width, cfg->height, 0x0000);   /* clear before turning the panel on */
    cmd(d, CMD_DISPON);
    vTaskDelay(pdMS_TO_TICKS(10));

    if (cfg->backlight != PURR_PIN_NONE) {
        backlight_init(d, cfg->backlight);
    }
    *out = &d->ops;
    ESP_LOGI(TAG, "slot %d: %s up, %dx%d", slot, cfg->name, cfg->width, cfg->height);
    return ESP_OK;
}

/* The original single-display entry point, unchanged for existing callers. */
esp_err_t purr_st7789_init(const purr_display_cfg_t *cfg, int spi_host, const purr_display_v2_t **out)
{
    return purr_st7789_init_slot(0, cfg, spi_host, out);
}
