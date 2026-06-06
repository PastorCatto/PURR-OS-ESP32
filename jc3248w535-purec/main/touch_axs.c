#include "touch_axs.h"
#include "display_axs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_axs15231b.h"

static const char *TAG = "touch_axs";

#define TOUCH_I2C_PORT   I2C_NUM_0
#define PIN_TOUCH_SDA    4
#define PIN_TOUCH_SCL    8
#define TOUCH_I2C_HZ     (400 * 1000)

static esp_lcd_touch_handle_t s_tp = NULL;

void touch_axs_init(void) {
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t i2c_cfg = {
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .i2c_port          = TOUCH_I2C_PORT,
        .sda_io_num        = PIN_TOUCH_SDA,
        .scl_io_num        = PIN_TOUCH_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &bus));

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_AXS15231B_CONFIG();
    tp_io_cfg.scl_speed_hz = TOUCH_I2C_HZ;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(bus, &tp_io_cfg, &tp_io));

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = AXS_LCD_W,
        .y_max = AXS_LCD_H,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags  = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_axs15231b(tp_io, &tp_cfg, &s_tp));
    ESP_LOGI(TAG, "AXS15231B touch ready");
}

bool touch_axs_get_event(touch_event_t* out) {
    if (!s_tp || !out) return false;
    esp_lcd_touch_point_data_t point = {0};
    uint8_t cnt = 0;
    esp_lcd_touch_read_data(s_tp);
    esp_lcd_touch_get_data(s_tp, &point, &cnt, 1);
    out->pressed    = (cnt > 0);
    out->x          = point.x;
    out->y          = point.y;
    out->contact_id = 0;
    return true;
}
