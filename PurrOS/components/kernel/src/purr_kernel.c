#include "purr_kernel.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "kernel";
static const purr_display_v2_t *s_display;
static i2c_master_dev_handle_t s_kbd;

static void pin_out(int pin, int level)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);
    gpio_set_level((gpio_num_t)pin, level);
}

esp_err_t purr_kernel_init(void)
{
    const purr_board_t *b = purr_board();
    ESP_LOGI(TAG, "board: %s", b->name);

    /* The peripheral rail first. Nothing on it answers until it is on. */
    if (b->power_pin != PURR_PIN_NONE) {
        pin_out(b->power_pin, 1);
        ESP_LOGI(TAG, "peripheral power on (GPIO %d)", b->power_pin);
        vTaskDelay(pdMS_TO_TICKS(b->power_settle_ms));
    }

    /* Other devices on the shared SPI bus stay deselected. */
    for (int i = 0; i < PURR_MAX_IDLE_PINS; i++) {
        if (b->idle_high_pins[i] != PURR_PIN_NONE) {
            pin_out(b->idle_high_pins[i], 1);
        }
    }

    spi_bus_config_t bus = {
        .mosi_io_num = b->spi.mosi,
        .miso_io_num = b->spi.miso,
        .sclk_io_num = b->spi.sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = b->display.width * 16 * 2,
    };
    esp_err_t e = spi_bus_initialize((spi_host_device_t)b->spi.host, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "spi bus: %s", esp_err_to_name(e));
        return e;
    }

    const purr_keyboard_cfg_t *k = &b->keyboard;
    if (k->addr != 0) {
        i2c_master_bus_config_t bc = {
            .i2c_port = k->port, .sda_io_num = k->sda, .scl_io_num = k->scl,
            .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        i2c_master_bus_handle_t bus_h;
        i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = k->addr,
            .scl_speed_hz = k->hz,
        };
        if (i2c_new_master_bus(&bc, &bus_h) != ESP_OK ||
            i2c_master_bus_add_device(bus_h, &dc, &s_kbd) != ESP_OK) {
            s_kbd = NULL;
            ESP_LOGW(TAG, "keyboard bus did not start");
        }
    }

    e = purr_st7789_init(&b->display, b->spi.host, &s_display);
    if (e != ESP_OK) {
        s_display = NULL;
        ESP_LOGE(TAG, "display did not come up: %s", esp_err_to_name(e));
    }
    return e;
}

const purr_display_v2_t *purr_kernel_display(void)
{
    return s_display;
}

char purr_kernel_key(void)
{
    uint8_t c = 0;
    if (s_kbd == NULL || i2c_master_receive(s_kbd, &c, 1, 20) != ESP_OK) {
        return 0;
    }
    return (char)c;
}
