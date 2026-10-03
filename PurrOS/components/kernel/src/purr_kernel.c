#include "purr_kernel.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "kernel";

static void pin_out(int pin, int level)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);
    gpio_set_level((gpio_num_t)pin, level);
}

static void pin_in_pullup(int pin)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT,
                        .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&io);
}

/* ---------------------------------------------------------------- keyboard driver (F-02) */
/*
 * Not its own source file yet -- the whole thing is this one i2c_master_bus/device bring-up,
 * unchanged from what purr_kernel_init() used to do inline. Promoted to a probe()/remove()
 * pair so it goes through the registry (compatible="kbd-i2c") the same as the display,
 * catching a pin conflict with anything else on this bus instead of silently wiring up
 * regardless.
 */
static int kbd_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    (void)board;
    const purr_keyboard_cfg_t *k = (const purr_keyboard_cfg_t *)dev->cfg;
    if (k->addr == 0) {
        return -1;                /* this board has no keyboard */
    }
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
    i2c_master_dev_handle_t kbd;
    if (i2c_new_master_bus(&bc, &bus_h) != ESP_OK ||
        i2c_master_bus_add_device(bus_h, &dc, &kbd) != ESP_OK) {
        ESP_LOGW(TAG, "keyboard bus did not start");
        return -1;
    }
    *out_handle = (void *)kbd;
    return 0;
}

static void kbd_remove(void *handle)
{
    (void)handle;
}

static const purr_driver_t s_st7789_driver = {
    .name = "st7789", .compatible = "st7789",
    .probe = purr_st7789_probe, .remove = purr_st7789_remove,
};
static const purr_driver_t s_kbd_driver = {
    .name = "kbd-i2c", .compatible = "kbd-i2c",
    .probe = kbd_probe, .remove = kbd_remove,
};
static const purr_driver_t s_epd1in54_driver = {
    .name = "epd1in54", .compatible = "epd1in54",
    .probe = purr_epd1in54_probe, .remove = purr_epd1in54_remove,
};

/* The drivers this build links in -- one line per driver (purr_driver.h's own comment on why
 * this is a plain table, not yet the spec's linker-section self-registration). A new real
 * driver (a future board's touch controller, ...) is a new entry here plus its own source
 * file -- no other kernel code changes. */
static const purr_driver_t *const s_drivers[] = {
    &s_st7789_driver,
    &s_kbd_driver,
    &s_epd1in54_driver,
};

const purr_driver_t *const *purr_driver_table(int *count)
{
    *count = sizeof(s_drivers) / sizeof(s_drivers[0]);
    return s_drivers;
}

void purr_drivers_bind_all(const purr_board_t *board)
{
    int count = 0;
    const purr_driver_t *const *table = purr_driver_table(&count);
    purr_drivers_bind(board, table, count);
}

/* ---------------------------------------------------------------- init */

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

    /* A board with no keyboard (waveshare154/InkyTuxedo) still needs purr_kernel_key() to
     * return something for purr_menu_t-driven UI -- see purr_board.h's own comment on
     * btn_boot_pin/btn_pwr_pin. Active-low, same convention bootpkg's own button reading
     * uses on this same hardware. */
    if (b->btn_boot_pin != PURR_PIN_NONE) {
        pin_in_pullup(b->btn_boot_pin);
    }
    if (b->btn_pwr_pin != PURR_PIN_NONE) {
        pin_in_pullup(b->btn_pwr_pin);
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

    /* F-02: every device in the board profile is matched, pin-checked and probed here,
     * instead of the display and keyboard each being brought up by a hardcoded, named call.
     * A device's own failure is recorded (purr_device_state()) and never stops the rest. */
    purr_drivers_bind_all(b);

    if (purr_device_state("display") != PURR_DEV_BOUND) {
        ESP_LOGE(TAG, "display did not come up");
        return ESP_FAIL;
    }
    return ESP_OK;
}

const purr_display_v2_t *purr_kernel_display(void)
{
    return (const purr_display_v2_t *)purr_device_handle("display");
}

/* BOOT (short press): down. BOOT (held btn_hold_ms): select. PWR (press): up -- the exact
 * mapping bootpkg's own hw_key() (pkg_hw_epd.c) uses on this same hardware, onto
 * purr_menu.c's existing w/s/d convention (purr_key_from_char()), so purr_menu_t-driven UI
 * (the install confirm screen, commands.c) works identically whether this function is
 * reading a real keyboard or these two buttons. Timed with esp_timer_get_time() rather than
 * an external tick: unlike bootpkg's freestanding environment, this runs under FreeRTOS and
 * already has a real clock. */
#define KERNEL_BTN_HOLD_MS_DEFAULT 500u

static char poll_buttons(const purr_board_t *b)
{
    static int s_boot_was_down, s_boot_hold_fired;
    static uint32_t s_boot_down_ms;
    static int s_pwr_was_down;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t hold_ms = b->btn_hold_ms != 0 ? b->btn_hold_ms : KERNEL_BTN_HOLD_MS_DEFAULT;

    int boot_down = gpio_get_level((gpio_num_t)b->btn_boot_pin) == 0;      /* active-low */
    int pwr_down = b->btn_pwr_pin != PURR_PIN_NONE &&
                   gpio_get_level((gpio_num_t)b->btn_pwr_pin) == 0;
    char result = 0;

    if (boot_down && !s_boot_was_down) {
        s_boot_down_ms = now_ms;
        s_boot_hold_fired = 0;
    } else if (boot_down && s_boot_was_down && !s_boot_hold_fired &&
              (now_ms - s_boot_down_ms) >= hold_ms) {
        s_boot_hold_fired = 1;
        result = 'd';            /* hold -> select/enter */
    } else if (!boot_down && s_boot_was_down && !s_boot_hold_fired) {
        result = 's';            /* short press -> down */
    }
    s_boot_was_down = boot_down;

    if (pwr_down && !s_pwr_was_down) {
        result = 'w';             /* press -> up */
    }
    s_pwr_was_down = pwr_down;

    return result;
}

char purr_kernel_key(void)
{
    i2c_master_dev_handle_t kbd = (i2c_master_dev_handle_t)purr_device_handle("keyboard");
    uint8_t c = 0;
    if (kbd != NULL && i2c_master_receive(kbd, &c, 1, 20) == ESP_OK) {
        return (char)c;
    }
    const purr_board_t *b = purr_board();
    if (b->btn_boot_pin != PURR_PIN_NONE) {
        return poll_buttons(b);
    }
    return 0;
}
