#include "purr_kernel.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "purr_driver_bus.h"

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

/* ---------------------------------------------------------------- shared I2C bus
 *
 * The keyboard used to live here too (compiled-in kbd_probe/kbd_read/kbd_remove) -- it's gone
 * now, converted to a real loadable driver module (Modules/drivers/kbd_i2c_module.c, loaded
 * and purr_driver_register()'d from commands.c's purr_modules_setup(), well after this file's
 * own purr_kernel_init() returns -- Drivers/SPEC.md section 4/kernel/SPEC.md section 5's
 * "separate driver modules... come later" condition, now met). GT911 (below) still shares
 * this same I2C bus (T-Deck/T-Deck Plus wire sda=18/scl=8 to both), and still needs this
 * helper directly since it is still compiled in -- whichever probe runs first (the touch
 * driver here, or the loaded keyboard module calling through purr_driver_bus_table_t's own
 * i2c_add_device, which reuses this exact same function) creates s_shared_i2c_bus; the other
 * reuses it. Bind order is neither guaranteed nor needed to be -- this works either way. */
static i2c_master_bus_handle_t s_shared_i2c_bus;

static esp_err_t shared_i2c_bus(int port, int sda, int scl, i2c_master_bus_handle_t *out)
{
    if (s_shared_i2c_bus != NULL) {
        *out = s_shared_i2c_bus;
        return ESP_OK;
    }
    i2c_master_bus_config_t bc = {
        .i2c_port = port, .sda_io_num = sda, .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t e = i2c_new_master_bus(&bc, &s_shared_i2c_bus);
    if (e == ESP_OK) {
        *out = s_shared_i2c_bus;
    }
    return e;
}

/* ---------------------------------------------------------------- GT911 touch driver
 *
 * Ported from the PURR-OS 0.11 archive's own driver (archive/DP9/code/source/drivers/touch/
 * gt911/gt911.c), which spent real hardware debugging time on exactly this chip on exactly
 * this board -- every register, address, and the swap+mirror+rescale math below is proven on
 * the real unit, not derived from the generic GT911 datasheet. Adapted to this project's
 * conventions (the driver registry instead of a catcall_touch_t table, board cfg instead of
 * hardcoded constants, purr_device_handle() instead of a module-static handle), not
 * reinvented. purr_board.h's own comment on the two cfg fields the archive had to discover by
 * direct capture (native_x_max/native_y_max -- NOT the commonly-assumed 0-4095).
 */
#define GT911_I2C_ADDR_PRIMARY   0x5D
#define GT911_I2C_ADDR_ALT       0x14

#define GT911_REG_STATUS         0x814E
#define GT911_REG_POINT1         0x8150
#define GT911_REG_COMMAND        0x8040
#define GT911_REG_CONFIG_START   0x8047
#define GT911_CONFIG_SIZE        184     /* 0x8047..0x80FE inclusive */
#define GT911_REG_CONFIG_CHKSUM  0x80FF
#define GT911_REG_CONFIG_FRESH   0x8100
#define GT911_CFG_OFS_X_MAX_L    1       /* 0x8048 - 0x8047 */
#define GT911_CFG_OFS_X_MAX_H    2
#define GT911_CFG_OFS_Y_MAX_L    3       /* 0x804A - 0x8047 */
#define GT911_CFG_OFS_Y_MAX_H    4

#define GT911_STATUS_TOUCH_MASK  0x0F
#define GT911_STATUS_BUFFER_STAT 0x80

#define GT911_KEEPALIVE_MS       2000

/* No leading track_id byte -- confirmed against the GT911's real register map: track_id
 * lives at 0x814F, one byte *before* GT911_REG_POINT1 (0x8150), so a read starting at
 * 0x8150 already sits on x_low. An earlier version of this driver (0.11) assumed a
 * track_id byte here, shifting every field over by one and corrupting both axes. */
typedef struct __attribute__((packed)) {
    uint8_t x_low, x_high, y_low, y_high;
} gt911_point_t;

static esp_err_t gt911_write_reg(i2c_master_dev_handle_t dev, uint16_t reg,
                                 const uint8_t *data, size_t len)
{
    uint8_t buf[2 + 8];           /* GT911_CONFIG_SIZE writes go through gt911_fix_resolution
                                    * below a different way (len is small there too -- the
                                    * largest single write this driver does is 1 byte) */
    buf[0] = (uint8_t)(reg >> 8);
    buf[1] = (uint8_t)(reg & 0xFF);
    memcpy(&buf[2], data, len);
    return i2c_master_transmit(dev, buf, 2 + len, pdMS_TO_TICKS(50));
}

static esp_err_t gt911_read_reg(i2c_master_dev_handle_t dev, uint16_t reg,
                                uint8_t *data, size_t len)
{
    uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    return i2c_master_transmit_receive(dev, addr, sizeof(addr), data, len, pdMS_TO_TICKS(50));
}

static esp_err_t gt911_clear_status(i2c_master_dev_handle_t dev)
{
    uint8_t zero = 0x00;
    return gt911_write_reg(dev, GT911_REG_STATUS, &zero, 1);
}

/* Rewrites the chip's own X/Y_OUTPUT_MAX config to match the real panel instead of whatever
 * it shipped with (seen configured for portrait on a landscape panel -- confirmed by the
 * wraparound happening at exactly 3/4 of the screen width). A single register poke is
 * silently ignored unless the whole config block's checksum is recomputed and the
 * "config fresh" flag is set, telling the chip to reload. */
static void gt911_fix_resolution(i2c_master_dev_handle_t dev, uint16_t width, uint16_t height)
{
    uint8_t cfg[GT911_CONFIG_SIZE];
    if (gt911_read_reg(dev, GT911_REG_CONFIG_START, cfg, sizeof(cfg)) != ESP_OK) {
        ESP_LOGW(TAG, "gt911: could not read config block -- skipping resolution fix");
        return;
    }
    uint16_t old_x = (uint16_t)cfg[GT911_CFG_OFS_X_MAX_L] | ((uint16_t)cfg[GT911_CFG_OFS_X_MAX_H] << 8);
    uint16_t old_y = (uint16_t)cfg[GT911_CFG_OFS_Y_MAX_L] | ((uint16_t)cfg[GT911_CFG_OFS_Y_MAX_H] << 8);
    if (old_x == width && old_y == height) {
        return;                   /* already correct */
    }
    cfg[GT911_CFG_OFS_X_MAX_L] = (uint8_t)(width & 0xFF);
    cfg[GT911_CFG_OFS_X_MAX_H] = (uint8_t)((width >> 8) & 0xFF);
    cfg[GT911_CFG_OFS_Y_MAX_L] = (uint8_t)(height & 0xFF);
    cfg[GT911_CFG_OFS_Y_MAX_H] = (uint8_t)((height >> 8) & 0xFF);

    uint8_t sum = 0;
    for (size_t i = 0; i < sizeof(cfg); i++) sum = (uint8_t)(sum + cfg[i]);
    uint8_t checksum = (uint8_t)((~sum) + 1);   /* two's complement: sum+checksum == 0 mod 256 */

    /* This write is bigger than gt911_write_reg's small fixed buffer -- its own transmit,
     * same shape. */
    uint8_t buf[2 + GT911_CONFIG_SIZE];
    buf[0] = (uint8_t)(GT911_REG_CONFIG_START >> 8);
    buf[1] = (uint8_t)(GT911_REG_CONFIG_START & 0xFF);
    memcpy(&buf[2], cfg, sizeof(cfg));
    esp_err_t e1 = i2c_master_transmit(dev, buf, sizeof(buf), pdMS_TO_TICKS(50));
    uint8_t chk = checksum;
    esp_err_t e2 = gt911_write_reg(dev, GT911_REG_CONFIG_CHKSUM, &chk, 1);
    uint8_t fresh = 0x01;
    esp_err_t e3 = gt911_write_reg(dev, GT911_REG_CONFIG_FRESH, &fresh, 1);
    if (e1 == ESP_OK && e2 == ESP_OK && e3 == ESP_OK) {
        ESP_LOGI(TAG, "gt911: config X/Y_OUTPUT_MAX corrected %ux%u -> %ux%u",
                 old_x, old_y, width, height);
    } else {
        ESP_LOGW(TAG, "gt911: resolution-fix write failed");
    }
}

static int gt911_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    (void)board;
    const purr_touch_cfg_t *t = (const purr_touch_cfg_t *)dev->cfg;

    if (t->rst_pin != PURR_PIN_NONE) {
        gpio_config_t rc = {.pin_bit_mask = 1ULL << t->rst_pin, .mode = GPIO_MODE_OUTPUT};
        gpio_config(&rc);
        gpio_set_level((gpio_num_t)t->rst_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level((gpio_num_t)t->rst_pin, 1);
    }
    /* GT911 needs real settle time after the peripheral rail/reset before it answers on I2C
     * (confirmed on hardware: ~300-400ms from power-on; display init eats some of that
     * budget already, this makes up the rest with margin rather than trying to track exactly
     * how much elapsed). */
    vTaskDelay(pdMS_TO_TICKS(350));

    i2c_master_bus_handle_t bus_h;
    if (shared_i2c_bus(t->port, t->sda, t->scl, &bus_h) != ESP_OK) {
        ESP_LOGW(TAG, "gt911: i2c bus did not start");
        return -1;
    }

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = GT911_I2C_ADDR_PRIMARY,
        .scl_speed_hz = t->hz,
    };
    i2c_master_dev_handle_t gt;
    if (i2c_master_bus_add_device(bus_h, &dc, &gt) != ESP_OK) {
        ESP_LOGW(TAG, "gt911: could not add i2c device");
        return -1;
    }

    uint8_t pid[4] = {0};
    if (gt911_read_reg(gt, 0x8140, pid, sizeof(pid)) == ESP_OK) {
        ESP_LOGI(TAG, "gt911: found at 0x%02X, id %.4s", GT911_I2C_ADDR_PRIMARY, pid);
    } else {
        i2c_master_bus_rm_device(gt);
        dc.device_address = GT911_I2C_ADDR_ALT;
        if (i2c_master_bus_add_device(bus_h, &dc, &gt) != ESP_OK ||
            gt911_read_reg(gt, 0x8140, pid, sizeof(pid)) != ESP_OK) {
            ESP_LOGW(TAG, "gt911: not found at 0x%02X or 0x%02X",
                     GT911_I2C_ADDR_PRIMARY, GT911_I2C_ADDR_ALT);
            return -1;
        }
        ESP_LOGI(TAG, "gt911: found at 0x%02X, id %.4s", GT911_I2C_ADDR_ALT, pid);
    }

    gt911_clear_status(gt);
    gt911_fix_resolution(gt, t->width, t->height);

    *out_handle = (void *)gt;
    return 0;
}

static void gt911_remove(void *handle)
{
    (void)handle;
}

static const purr_driver_t s_st7789_driver = {
    .name = "st7789", .compatible = "st7789",
    .probe = purr_st7789_probe, .remove = purr_st7789_remove,
};
static const purr_driver_t s_epd1in54_driver = {
    .name = "epd1in54", .compatible = "epd1in54",
    .probe = purr_epd1in54_probe, .remove = purr_epd1in54_remove,
};
static const purr_driver_t s_gt911_driver = {
    .name = "gt911", .compatible = "gt911",
    .probe = gt911_probe, .remove = gt911_remove,
};

/* ---------------------------------------------------------------- driver bus table
 * (purr_driver_bus.h) -- what a loaded driver module gets, built on exactly the same ESP-IDF
 * calls a compiled-in driver already makes directly (shared_i2c_bus() above for I2C,
 * spi_bus_add_device for SPI): a loaded module just reaches them through function pointers
 * instead of linking against them, since it can't link against this file's own code at all.
 * Handles are opaque uint32_t here, not raw ESP-IDF handle types -- a driver module's ABI
 * can't carry a pointer-sized ESP-IDF type across the relocation boundary portably, and this
 * project's modules already use plain ints/pointers in every other call table for the same
 * reason. */
#define MAX_BUS_HANDLES 8
static void *s_bus_handles[MAX_BUS_HANDLES];
static int s_bus_handle_count;

static uint32_t store_handle(void *h)
{
    if (h == NULL || s_bus_handle_count >= MAX_BUS_HANDLES) {
        return 0;                  /* 0 always means "no handle" -- 1-based on purpose */
    }
    s_bus_handles[s_bus_handle_count] = h;
    return (uint32_t)(++s_bus_handle_count);
}

static void *handle_at(uint32_t h)
{
    return (h >= 1 && h <= (uint32_t)s_bus_handle_count) ? s_bus_handles[h - 1] : NULL;
}

static uint32_t bus_spi_add_device(int host, int cs_pin, int mode, int clock_hz)
{
    spi_device_interface_config_t dc = {
        .clock_speed_hz = clock_hz, .mode = mode, .spics_io_num = cs_pin, .queue_size = 1,
    };
    spi_device_handle_t dev;
    if (spi_bus_add_device((spi_host_device_t)host, &dc, &dev) != ESP_OK) {
        return 0;
    }
    return store_handle(dev);
}

static int bus_spi_transfer(uint32_t handle, const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_device_handle_t dev = (spi_device_handle_t)handle_at(handle);
    if (dev == NULL || len == 0) {
        return -1;
    }
    spi_transaction_t t = {0};
    t.length = len * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    return spi_device_transmit(dev, &t) == ESP_OK ? 0 : -1;
}

static uint32_t bus_i2c_add_device(int port, int sda_pin, int scl_pin, uint16_t addr, int clock_hz)
{
    i2c_master_bus_handle_t bus_h;
    if (shared_i2c_bus(port, sda_pin, scl_pin, &bus_h) != ESP_OK) {
        return 0;
    }
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr,
        .scl_speed_hz = (uint32_t)clock_hz,
    };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bus_h, &dc, &dev) != ESP_OK) {
        return 0;
    }
    return store_handle(dev);
}

static int bus_i2c_write(uint32_t handle, const uint8_t *data, size_t len)
{
    i2c_master_dev_handle_t dev = (i2c_master_dev_handle_t)handle_at(handle);
    return (dev != NULL && i2c_master_transmit(dev, data, len, -1) == ESP_OK) ? 0 : -1;
}

static int bus_i2c_read(uint32_t handle, uint8_t *data, size_t len, int timeout_ms)
{
    i2c_master_dev_handle_t dev = (i2c_master_dev_handle_t)handle_at(handle);
    return (dev != NULL && i2c_master_receive(dev, data, len, timeout_ms) == ESP_OK) ? 0 : -1;
}

static int bus_i2c_write_read(uint32_t handle, const uint8_t *tx, size_t tx_len,
                              uint8_t *rx, size_t rx_len)
{
    i2c_master_dev_handle_t dev = (i2c_master_dev_handle_t)handle_at(handle);
    return (dev != NULL &&
            i2c_master_transmit_receive(dev, tx, tx_len, rx, rx_len, -1) == ESP_OK) ? 0 : -1;
}

static void bus_gpio_set_output(int pin, int level) { pin_out(pin, level); }
static void bus_gpio_set_input_pullup(int pin) { pin_in_pullup(pin); }
static int bus_gpio_get(int pin) { return gpio_get_level((gpio_num_t)pin); }
static void bus_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
static uint64_t bus_now_us(void) { return (uint64_t)esp_timer_get_time(); }

static const purr_driver_bus_table_t s_bus_table = {
    .abi_version = PURR_DRIVER_BUS_ABI_VERSION,
    .spi_add_device = bus_spi_add_device,
    .spi_transfer = bus_spi_transfer,
    .i2c_add_device = bus_i2c_add_device,
    .i2c_write = bus_i2c_write,
    .i2c_read = bus_i2c_read,
    .i2c_write_read = bus_i2c_write_read,
    .gpio_set_output = bus_gpio_set_output,
    .gpio_set_input_pullup = bus_gpio_set_input_pullup,
    .gpio_get = bus_gpio_get,
    .delay_ms = bus_delay_ms,
    .now_us = bus_now_us,
};

const purr_driver_bus_table_t *purr_driver_bus_table(void)
{
    return &s_bus_table;
}

/* The drivers this build links in -- one line per driver (purr_driver.h's own comment on why
 * this is a plain table, not yet the spec's linker-section self-registration). A new real
 * driver is a new entry here plus its own source file -- no other kernel code changes. */
static const purr_driver_t *const s_drivers[] = {
    &s_st7789_driver,
    &s_epd1in54_driver,
    &s_gt911_driver,
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
    uint8_t c = 0;
    if (purr_device_read("keyboard", &c, 1) == 1) {
        return (char)c;
    }
    const purr_board_t *b = purr_board();
    if (b->btn_boot_pin != PURR_PIN_NONE) {
        return poll_buttons(b);
    }
    return 0;
}

/* GT911's own "send 0x00 to the command register periodically" quirk (archive's own comment:
 * without this it stops updating its status/point registers after the first touch -- works
 * once, then goes dead). Called on every purr_kernel_touch_read(), same as the archive did. */
static void gt911_keepalive(i2c_master_dev_handle_t gt)
{
    static uint32_t s_keepalive_at;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_keepalive_at != 0 && now < s_keepalive_at) {
        return;
    }
    s_keepalive_at = now + GT911_KEEPALIVE_MS;
    uint8_t zero = 0x00;
    gt911_write_reg(gt, GT911_REG_COMMAND, &zero, 1);
}

/* 1 and (*x, *y) set if currently touched, 0 otherwise (including "no touch driver bound").
 * The signed-interpretation and swap+mirror+rescale math below is ported as-is from the
 * archive driver's own hard-won comments: this unit's GT911 does not report the commonly
 * assumed 0-4095 range (confirmed by direct capture: X spans close to the full 16-bit range,
 * Y a much smaller ~1280-10240-ish range), and the raw bytes have to be read as SIGNED, not
 * unsigned -- an internal coordinate counter overflow then lands as a small negative number
 * ("just past the edge, still going the same direction") instead of wrapping to a small
 * positive one near zero (which looks exactly like "resets and climbs again", a bug chased
 * for real before this was found). Deliberately no filtering/jump-rejection/settle-counting
 * either, same reasoning: the archive's own extra logic for that introduced new bugs on top
 * of whatever it was trying to fix, and reporting exactly what the chip says worked fine. */
int purr_kernel_touch_read(uint16_t *x, uint16_t *y)
{
    i2c_master_dev_handle_t gt = (i2c_master_dev_handle_t)purr_device_handle("touch");
    if (gt == NULL || x == NULL || y == NULL) {
        return 0;
    }
    gt911_keepalive(gt);

    uint8_t status = 0;
    if (gt911_read_reg(gt, GT911_REG_STATUS, &status, 1) != ESP_OK) {
        return 0;
    }
    if (!(status & GT911_STATUS_BUFFER_STAT)) {
        return 0;
    }
    if ((status & GT911_STATUS_TOUCH_MASK) == 0) {
        gt911_clear_status(gt);     /* GT911's "release" event -- must be cleared or the
                                      * buffer-ready flag stays latched and it never reports
                                      * the next touch at all. */
        return 0;
    }

    gt911_point_t pt;
    if (gt911_read_reg(gt, GT911_REG_POINT1, (uint8_t *)&pt, sizeof(pt)) != ESP_OK) {
        gt911_clear_status(gt);
        return 0;
    }

    uint16_t raw_x = (uint16_t)pt.x_low | ((uint16_t)pt.x_high << 8);
    uint16_t raw_y = (uint16_t)pt.y_low | ((uint16_t)pt.y_high << 8);
    int16_t signed_x = (int16_t)raw_x;
    int16_t signed_y = (int16_t)raw_y;

    const purr_touch_cfg_t *t = &purr_board()->touch;
    /* Confirmed on hardware: no-swap reads back as the screen rotated 90 degrees, and a
     * swap+mirror with no rescale caps one axis short of the real edge -- both pieces are
     * needed together, since the two raw fields' native ranges don't match the screen
     * dimension they each end up driving after the swap. */
    int32_t sx = ((int32_t)signed_y * t->width) / t->native_y_max;
    int32_t sy_raw = ((int32_t)signed_x * t->height) / t->native_x_max;
    int32_t sy = (t->height - 1) - sy_raw;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx >= t->width) sx = t->width - 1;
    if (sy >= t->height) sy = t->height - 1;

    *x = (uint16_t)sx;
    *y = (uint16_t)sy;
    gt911_clear_status(gt);
    return 1;
}
