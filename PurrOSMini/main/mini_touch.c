/* mini_touch.c - see mini_touch.h. */
#include "mini_touch.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs.h"

#include "mini_board.h"

static const char *TAG = "mini-touch";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

esp_err_t mini_touch_init(void)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << MINI_TP_RST, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);
    gpio_set_level(MINI_TP_RST, 0);                              /* the panel's reset: low, then high, then it needs a moment */
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(MINI_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(60));
    i2c_master_bus_config_t bus = {.i2c_port = I2C_NUM_0, .sda_io_num = MINI_TP_SDA, .scl_io_num = MINI_TP_SCL, .clk_source = I2C_CLK_SRC_DEFAULT,
                                   .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true};
    esp_err_t e = i2c_new_master_bus(&bus, &s_bus);
    if (e != ESP_OK) {
        return e;
    }
    i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = MINI_TP_ADDR, .scl_speed_hz = 400000};
    e = i2c_master_bus_add_device(s_bus, &dev, &s_dev);
    if (e != ESP_OK) {
        return e;
    }
    uint8_t dis[2] = {0xFE, 0xFF};                               /* do not go to sleep between touches */
    i2c_master_transmit(s_dev, dis, sizeof(dis), 50);
    ESP_LOGI(TAG, "CST816S at 0x%02X", MINI_TP_ADDR);
    return ESP_OK;
}

static mini_cal_t s_cal;

void mini_touch_read_raw(bool *down, int *x, int *y)
{
    *down = false;
    *x = *y = 0;
    uint8_t reg = 0x01, b[6];
    if (s_dev == NULL || i2c_master_transmit_receive(s_dev, &reg, 1, b, sizeof(b), 20) != ESP_OK || (b[1] & 0x0F) == 0) {
        return;
    }
    *down = true;
    *x = ((b[2] & 0x0F) << 8) | b[3];
    *y = ((b[4] & 0x0F) << 8) | b[5];
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void mini_touch_read(mini_touch_t *t)
{
    bool d;
    int x, y;
    mini_touch_read_raw(&d, &x, &y);
    t->down = d;
    t->x = t->y = 0;
    if (!d) {
        return;
    }
    if (s_cal.valid) {
        int rx = s_cal.swap ? y : x, ry = s_cal.swap ? x : y;
        x = MINI_CAL_INSET + (rx - s_cal.ax0) * (MINI_W - 2 * MINI_CAL_INSET) / (s_cal.ax1 - s_cal.ax0);
        y = MINI_CAL_INSET + (ry - s_cal.ay0) * (MINI_H - 2 * MINI_CAL_INSET) / (s_cal.ay1 - s_cal.ay0);
    } else {
        if (MINI_TP_SWAP_XY) {
            int s = x;
            x = y;
            y = s;
        }
        if (MINI_TP_FLIP_X) {
            x = MINI_W - 1 - x;
        }
        if (MINI_TP_FLIP_Y) {
            y = MINI_H - 1 - y;
        }
    }
    t->x = clampi(x, 0, MINI_W - 1);
    t->y = clampi(y, 0, MINI_H - 1);
}

bool mini_touch_cal_load(void)
{
    nvs_handle_t h;
    if (nvs_open("mini", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t n = sizeof(s_cal);
    mini_cal_t c;
    bool ok = nvs_get_blob(h, "cal", &c, &n) == ESP_OK && n == sizeof(c) && c.valid && c.ax1 != c.ax0 && c.ay1 != c.ay0;
    nvs_close(h);
    if (ok) {
        s_cal = c;
    }
    return ok;
}

void mini_touch_cal_set(const mini_cal_t *c)
{
    s_cal = *c;
    nvs_handle_t h;
    if (nvs_open("mini", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "cal", c, sizeof(*c));
        nvs_commit(h);
        nvs_close(h);
    }
}

void mini_touch_cal_clear(void)
{
    memset(&s_cal, 0, sizeof(s_cal));
    nvs_handle_t h;
    if (nvs_open("mini", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "cal");
        nvs_commit(h);
        nvs_close(h);
    }
}
