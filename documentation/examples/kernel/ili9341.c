/*
 * ILI9341 display driver, providing the native `display` catcall (v2.0).
 *
 * Modelled on st7789.c: the panel setup is data (an init table), and everything that
 * differs per board (size, offsets, orientation, colour order, inversion) comes from the
 * board profile. NOT yet run on real hardware. The init table is the widely used
 * Adafruit_ILI9341 sequence; check it against your panel's datasheet.
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

static const char *TAG = "ili9341";

#define CHUNK_LINES   16                     /* lines sent per SPI transaction */

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
    uint8_t data[16];
} step_t;

static const step_t s_init[] = {
    {0x01, 150, 0, {0}},                                          /* software reset */
    {0xEF, 0, 3, {0x03, 0x80, 0x02}},
    {0xCF, 0, 3, {0x00, 0xC1, 0x30}},
    {0xED, 0, 4, {0x64, 0x03, 0x12, 0x81}},
    {0xE8, 0, 3, {0x85, 0x00, 0x78}},
    {0xCB, 0, 5, {0x39, 0x2C, 0x00, 0x34, 0x02}},
    {0xF7, 0, 1, {0x20}},
    {0xEA, 0, 2, {0x00, 0x00}},
    {0xC0, 0, 1, {0x23}},                                         /* power control 1 */
    {0xC1, 0, 1, {0x10}},                                         /* power control 2 */
    {0xC5, 0, 2, {0x3E, 0x28}},                                   /* VCOM 1 */
    {0xC7, 0, 1, {0x86}},                                         /* VCOM 2 */
    {CMD_MADCTL, 0, 1, {0}},                                      /* orientation: from the profile */
    {0x3A, 0, 1, {0x55}},                                         /* 16 bits per pixel */
    {0xB1, 0, 2, {0x00, 0x18}},                                   /* frame rate */
    {0xB6, 0, 3, {0x08, 0x82, 0x27}},                             /* display function control */
    {0xF2, 0, 1, {0x00}},                                         /* 3-gamma off */
    {0x26, 0, 1, {0x01}},                                         /* gamma curve 1 */
    {0xE0, 0, 15, {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1,
                   0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00}},    /* positive gamma */
    {0xE1, 0, 15, {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1,
                   0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F}},    /* negative gamma */
    {CMD_SLPOUT, 120, 0, {0}},                                    /* sleep out */
};

static struct {
    spi_device_handle_t dev;
    const purr_display_cfg_t *cfg;
    uint8_t *dma;                            /* one chunk of pixel bytes, DMA-capable */
    uint32_t dma_bytes;
    int backlight_ready;
} s;

/* ---------------------------------------------------------------- SPI */

static void IRAM_ATTR pre_cb(spi_transaction_t *t)
{
    gpio_set_level((gpio_num_t)s.cfg->dc, (int)(intptr_t)t->user);
}

static esp_err_t send(const void *buf, size_t len, int dc)
{
    if (len == 0) {
        return ESP_OK;
    }
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = buf,
        .user = (void *)(intptr_t)dc,
    };
    return spi_device_transmit(s.dev, &t);
}

static esp_err_t cmd(uint8_t c)
{
    return send(&c, 1, 0);
}

static esp_err_t cmd_data(uint8_t c, const uint8_t *d, size_t n)
{
    esp_err_t e = cmd(c);
    return (e == ESP_OK && n) ? send(d, n, 1) : e;
}

static esp_err_t set_window(int x0, int y0, int x1, int y1)
{
    x0 += s.cfg->col_off; x1 += s.cfg->col_off;
    y0 += s.cfg->row_off; y1 += s.cfg->row_off;
    uint8_t col[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
    uint8_t row[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
    esp_err_t e = cmd_data(CMD_CASET, col, 4);
    if (e == ESP_OK) e = cmd_data(CMD_RASET, row, 4);
    if (e == ESP_OK) e = cmd(CMD_RAMWR);
    return e;
}

/* ------------------------------------------------------------ catcall */

static esp_err_t clip(int *x, int *y, int *w, int *h, int *skip_x, int *skip_y)
{
    int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s.cfg->width) x1 = s.cfg->width;
    if (y1 > s.cfg->height) y1 = s.cfg->height;
    if (x1 <= x0 || y1 <= y0) {
        return ESP_ERR_NOT_FINISHED;         /* nothing visible: succeeds without doing anything */
    }
    *skip_x = x0 - *x;
    *skip_y = y0 - *y;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return ESP_OK;
}

static esp_err_t d_get_info(purr_display_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->width = s.cfg->width;
    out->height = s.cfg->height;
    out->max_chunk_pixels = (uint16_t)(s.dma_bytes / 2);
    strncpy(out->name, s.cfg->name, sizeof(out->name) - 1);
    return ESP_OK;
}

static esp_err_t d_blit(int x, int y, int w, int h, const uint16_t *px)
{
    int full_w = w, sx = 0, sy = 0;
    esp_err_t e = clip(&x, &y, &w, &h, &sx, &sy);
    if (e == ESP_ERR_NOT_FINISHED) {
        return ESP_OK;
    }
    e = set_window(x, y, x + w - 1, y + h - 1);
    int lines_per_chunk = (int)(s.dma_bytes / 2) / w;
    if (lines_per_chunk < 1) lines_per_chunk = 1;
    for (int row = 0; row < h && e == ESP_OK; row += lines_per_chunk) {
        int n = h - row < lines_per_chunk ? h - row : lines_per_chunk;
        for (int l = 0; l < n; l++) {
            const uint16_t *src = px + (size_t)(sy + row + l) * full_w + sx;
            uint8_t *dst = s.dma + (size_t)l * w * 2;
            for (int i = 0; i < w; i++) {
                dst[2 * i] = (uint8_t)(src[i] >> 8);        /* the panel wants big-endian */
                dst[2 * i + 1] = (uint8_t)src[i];
            }
        }
        e = send(s.dma, (size_t)n * w * 2, 1);
    }
    return e;
}

static esp_err_t d_fill(int x, int y, int w, int h, uint16_t color)
{
    int sx, sy;
    esp_err_t e = clip(&x, &y, &w, &h, &sx, &sy);
    if (e == ESP_ERR_NOT_FINISHED) {
        return ESP_OK;
    }
    e = set_window(x, y, x + w - 1, y + h - 1);
    uint32_t total = (uint32_t)w * h;
    uint32_t per = s.dma_bytes / 2;
    for (uint32_t i = 0; i < per; i++) {
        s.dma[2 * i] = (uint8_t)(color >> 8);
        s.dma[2 * i + 1] = (uint8_t)color;
    }
    while (total && e == ESP_OK) {
        uint32_t n = total < per ? total : per;
        e = send(s.dma, (size_t)n * 2, 1);
        total -= n;
    }
    return e;
}

static esp_err_t d_set_brightness(uint8_t level)
{
    if (!s.backlight_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, level);
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
}

static purr_display_v2_t s_ops = {
    .struct_size = sizeof(purr_display_v2_t),
    .major = PURR_DISPLAY_MAJOR,
    .minor = PURR_DISPLAY_MINOR,
    .features = 0,
    .get_info = d_get_info,
    .blit = d_blit,
    .fill = d_fill,
    .set_brightness = NULL,
    .set_power = NULL,
};

/* --------------------------------------------------------------- init */

static void backlight_init(int pin)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_2,
        .duty_resolution = LEDC_TIMER_8_BIT, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t c = {
        .gpio_num = pin, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_2,
        .timer_sel = LEDC_TIMER_2, .duty = 0, .hpoint = 0,
    };
    if (ledc_timer_config(&t) == ESP_OK && ledc_channel_config(&c) == ESP_OK) {
        s.backlight_ready = 1;
        s_ops.set_brightness = d_set_brightness;
        s_ops.features |= PURR_DISPLAY_F_BRIGHTNESS;
    }
}

esp_err_t purr_ili9341_init(const purr_display_cfg_t *cfg, int spi_host, const purr_display_v2_t **out)
{
    memset(&s, 0, sizeof(s));
    s.cfg = cfg;
    s.dma_bytes = (uint32_t)cfg->width * CHUNK_LINES * 2;
    s.dma = heap_caps_malloc(s.dma_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s.dma) {
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t io = {.pin_bit_mask = 1ULL << cfg->dc, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);

    spi_device_interface_config_t dc = {
        .clock_speed_hz = (int)cfg->spi_hz,
        .mode = 0,
        .spics_io_num = cfg->cs,
        .queue_size = 2,
        .pre_cb = pre_cb,
    };
    esp_err_t e = spi_bus_add_device((spi_host_device_t)spi_host, &dc, &s.dev);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "add device: %s", esp_err_to_name(e));
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
        uint8_t data[16];
        memcpy(data, s_init[i].data, sizeof(data));
        if (s_init[i].cmd == CMD_MADCTL) {
            data[0] = cfg->madctl | (cfg->bgr ? 0x08 : 0x00);   /* bit 3 = BGR order */
        }
        e = cmd_data(s_init[i].cmd, data, s_init[i].len);
        if (s_init[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(s_init[i].delay_ms));
        }
    }
    if (e == ESP_OK && cfg->invert) e = cmd(CMD_INVON);
    if (e == ESP_OK) e = cmd(CMD_NORON);
    vTaskDelay(pdMS_TO_TICKS(10));
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "init: %s", esp_err_to_name(e));
        return e;
    }

    d_fill(0, 0, cfg->width, cfg->height, 0x0000);   /* clear before turning the panel on */
    cmd(CMD_DISPON);
    vTaskDelay(pdMS_TO_TICKS(10));

    if (cfg->backlight != PURR_PIN_NONE) {
        backlight_init(cfg->backlight);
    }
    *out = &s_ops;
    ESP_LOGI(TAG, "%s up, %dx%d", cfg->name, cfg->width, cfg->height);
    return ESP_OK;
}
