#include "purr_kernel.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "kernel";
static const purr_display_v2_t *s_display;

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
