/* mini_lcd.c - see mini_lcd.h. Plain SPI with DMA, polling transfers: the panel is the slow part and there is nothing else for this core to do while a chunk goes out. */
#include "mini_lcd.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mini_board.h"

static const char *TAG = "mini-lcd";
#define CHUNK_PX 4096                    /* 8 KB a chunk */

static spi_device_handle_t s_dev;
static uint8_t *s_dma;                   /* CHUNK_PX * 2 bytes, DMA capable */

static void IRAM_ATTR dc_cb(spi_transaction_t *t)
{
    gpio_set_level(MINI_LCD_DC, (int)(intptr_t)t->user);
}

static void send(const void *data, int len, int is_data, int keep_cs)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = (size_t)len * 8;
    t.tx_buffer = data;
    t.user = (void *)(intptr_t)is_data;
    (void)keep_cs;                                               /* chip select is released between chunks: SPI_TRANS_CS_KEEP_ACTIVE kept the bus lock and every later chunk failed ("polling can't get buslock", found on the real board); the panel goes on writing pixels until the next command */
    spi_device_polling_transmit(s_dev, &t);
}

static void cmd(uint8_t c) { send(&c, 1, 0, 0); }

static void cmd_data(uint8_t c, const uint8_t *d, int n)
{
    cmd(c);
    if (n > 0) {
        send(d, n, 1, 0);
    }
}

static void window(int x, int y, int w, int h)
{
    uint8_t a[4];
    a[0] = (uint8_t)(x >> 8);
    a[1] = (uint8_t)x;
    a[2] = (uint8_t)((x + w - 1) >> 8);
    a[3] = (uint8_t)(x + w - 1);
    cmd_data(0x2A, a, 4);
    a[0] = (uint8_t)(y >> 8);
    a[1] = (uint8_t)y;
    a[2] = (uint8_t)((y + h - 1) >> 8);
    a[3] = (uint8_t)(y + h - 1);
    cmd_data(0x2B, a, 4);
    cmd(0x2C);
}

esp_err_t mini_lcd_init(void)
{
    gpio_config_t io = {.pin_bit_mask = (1ULL << MINI_LCD_DC) | (1ULL << MINI_LCD_BL), .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);
    gpio_set_level(MINI_LCD_BL, 0);
    spi_bus_config_t bus = {.mosi_io_num = MINI_LCD_MOSI, .miso_io_num = MINI_LCD_MISO, .sclk_io_num = MINI_LCD_SCLK, .quadwp_io_num = -1, .quadhd_io_num = -1,
                            .max_transfer_sz = CHUNK_PX * 2 + 16};
    esp_err_t e = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) {
        return e;
    }
    spi_device_interface_config_t dev = {.clock_speed_hz = MINI_LCD_HZ, .mode = 0, .spics_io_num = MINI_LCD_CS, .queue_size = 4, .pre_cb = dc_cb};
    e = spi_bus_add_device(SPI2_HOST, &dev, &s_dev);
    if (e != ESP_OK) {
        return e;
    }
    s_dma = heap_caps_malloc(CHUNK_PX * 2, MALLOC_CAP_DMA);
    if (s_dma == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cmd(0x01);                                                   /* software reset */
    vTaskDelay(pdMS_TO_TICKS(120));
    static const struct { uint8_t c; uint8_t n; uint8_t d[15]; } init[] = {
        {0xCF, 3, {0x00, 0xC1, 0x30}}, {0xED, 4, {0x64, 0x03, 0x12, 0x81}}, {0xE8, 3, {0x85, 0x00, 0x78}}, {0xCB, 5, {0x39, 0x2C, 0x00, 0x34, 0x02}},
        {0xF7, 1, {0x20}}, {0xEA, 2, {0x00, 0x00}}, {0xC0, 1, {0x23}}, {0xC1, 1, {0x10}}, {0xC5, 2, {0x3E, 0x28}}, {0xC7, 1, {0x86}},
        {0x36, 1, {MINI_LCD_MADCTL}}, {0x3A, 1, {0x55}}, {0xB1, 2, {0x00, 0x18}}, {0xB6, 3, {0x08, 0x82, 0x27}}, {0xF2, 1, {0x00}}, {0x26, 1, {0x01}},
        {0xE0, 15, {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00}},
        {0xE1, 15, {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F}},
    };
    for (size_t i = 0; i < sizeof(init) / sizeof(init[0]); i++) {
        cmd_data(init[i].c, init[i].d, init[i].n);
    }
    cmd(0x11);                                                   /* sleep out */
    vTaskDelay(pdMS_TO_TICKS(120));
    cmd(0x29);                                                   /* display on */
    mini_lcd_fill(0, 0, MINI_W, MINI_H, 0xFFFF);
    ESP_LOGI(TAG, "ILI9341 up (%dx%d, MADCTL 0x%02X)", MINI_W, MINI_H, MINI_LCD_MADCTL);
    return ESP_OK;
}

void mini_lcd_backlight(int on)
{
    gpio_set_level(MINI_LCD_BL, on ? 1 : 0);
}

static int clip(int *x, int *y, int *w, int *h)
{
    if (*x < 0) {
        *w += *x;
        *x = 0;
    }
    if (*y < 0) {
        *h += *y;
        *y = 0;
    }
    if (*x + *w > MINI_W) {
        *w = MINI_W - *x;
    }
    if (*y + *h > MINI_H) {
        *h = MINI_H - *y;
    }
    return *w > 0 && *h > 0;
}

void mini_lcd_fill(int x, int y, int w, int h, uint16_t c)
{
    if (!clip(&x, &y, &w, &h)) {
        return;
    }
    uint16_t be = (uint16_t)((c >> 8) | (c << 8));
    int total = w * h, first = total < CHUNK_PX ? total : CHUNK_PX;
    for (int i = 0; i < first; i++) {
        ((uint16_t *)s_dma)[i] = be;
    }
    window(x, y, w, h);
    while (total > 0) {
        int n = total < CHUNK_PX ? total : CHUNK_PX;
        send(s_dma, n * 2, 1, total > n);
        total -= n;
    }
}

void mini_lcd_write(int x, int y, int w, int h, const uint16_t *px)
{
    if (x < 0 || y < 0 || x + w > MINI_W || y + h > MINI_H || w <= 0 || h <= 0) {
        return;                                                  /* the callers clip; a bad rectangle is dropped, never half drawn */
    }
    window(x, y, w, h);
    int total = w * h, done = 0;
    while (done < total) {
        int n = total - done < CHUNK_PX ? total - done : CHUNK_PX;
        for (int i = 0; i < n; i++) {
            uint16_t v = px[done + i];
            ((uint16_t *)s_dma)[i] = (uint16_t)((v >> 8) | (v << 8));
        }
        done += n;
        send(s_dma, n * 2, 1, done < total);
    }
}

void mini_lcd_write_le(int x, int y, int w, int h, const uint8_t *le)
{
    if (x < 0 || y < 0 || x + w > MINI_W || y + h > MINI_H || w <= 0 || h <= 0) {
        return;
    }
    window(x, y, w, h);
    int total = w * h, done = 0;
    while (done < total) {
        int n = total - done < CHUNK_PX ? total - done : CHUNK_PX;
        for (int i = 0; i < n; i++) {                            /* the wire's low byte first; the panel wants the high byte first: swap each pair */
            s_dma[2 * i] = le[2 * (done + i) + 1];
            s_dma[2 * i + 1] = le[2 * (done + i)];
        }
        done += n;
        send(s_dma, n * 2, 1, done < total);
    }
}
