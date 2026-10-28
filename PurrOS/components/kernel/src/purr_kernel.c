#include "purr_kernel.h"

#include <string.h>

#include <inttypes.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_flash_encrypt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "purr_cfg.h"
#include "purr_vm.h"
#if CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC
#include "bootloader_common.h"
#endif

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

/* ---------------------------------------------------------------- VM info (purr_vm.h)
 * Asks the emulator's VM-info device, if there is one, what it provides. Block layout (12 bytes,
 * read from any offset): "PVM", version, flags, NIC SPI host, sclk, mosi, miso, cs (0xFF = none),
 * two reserved. The first call does the probe; reading it also logs the eFuse state, so a
 * soft-burned fuse (emulator --efuse) can be seen taking effect without real hardware. */
static purr_vm_info_t s_vm;
static bool s_vm_probed;

static int8_t vm_pin(uint8_t raw)
{
    return raw == 0xff ? -1 : (int8_t)raw;
}

const purr_vm_info_t *purr_vm_info(void)
{
    if (s_vm_probed) {
        return &s_vm;
    }
    s_vm_probed = true;
    const purr_board_t *b = purr_board();
    if (b == NULL || b->keyboard.addr == 0) {
        return &s_vm;
    }
    i2c_master_bus_handle_t bus;
    if (shared_i2c_bus(b->keyboard.port, b->keyboard.sda, b->keyboard.scl, &bus) != ESP_OK) {
        return &s_vm;
    }
    if (i2c_master_probe(bus, PURR_VM_I2C_ADDR, 50) != ESP_OK) {
        return &s_vm;                                   /* nothing there: a real board */
    }
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = PURR_VM_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK) {
        return &s_vm;
    }
    uint8_t raw[PURR_VM_INFO_LEN];
    esp_err_t e = i2c_master_receive(dev, raw, sizeof(raw), 100);
    i2c_master_bus_rm_device(dev);
    if (e != ESP_OK || raw[0] != 'P' || raw[1] != 'V' || raw[2] != 'M') {
        return &s_vm;
    }
    s_vm.present = 1;
    s_vm.version = raw[3];
    s_vm.flags = raw[4];
    s_vm.nic_host = raw[5];
    s_vm.nic_sclk = vm_pin(raw[6]);
    s_vm.nic_mosi = vm_pin(raw[7]);
    s_vm.nic_miso = vm_pin(raw[8]);
    s_vm.nic_cs = vm_pin(raw[9]);
    ESP_LOGI("vm", "running in a VM (info v%u, flags 0x%02x)", s_vm.version, s_vm.flags);

    /* Show the eFuses the OS and bootloader care about. */
    uint32_t usr[2] = {0, 0};
    esp_efuse_read_block(EFUSE_BLK3, usr, 0, 64);
    ESP_LOGI("vm", "eFuse: secure boot %s, flash encryption %s, user data %08" PRIx32 " %08" PRIx32,
             esp_efuse_read_field_bit(ESP_EFUSE_SECURE_BOOT_EN) ? "ON" : "off",
             esp_flash_encryption_enabled() ? "ON" : "off", usr[0], usr[1]);
    return &s_vm;
}

/* st7789 (display) is gone from here too now -- converted to a real loadable driver module
 * (Modules/drivers/st7789_module.c), same as keyboard and touch before it. Unlike those two,
 * display's real logic never lived outside its own probe()/the returned purr_display_v2_t
 * ops table, so this was a more direct port -- the two real gaps were a DMA-capable allocator
 * and backlight PWM, neither of which any other driver needed yet (purr_driver_bus_table_t's
 * dma_alloc()/backlight_init()/backlight_set()/gpio_write()). */
static const purr_driver_t s_epd1in54_driver = {
    .name = "epd1in54", .compatible = "epd1in54",
    .probe = purr_epd1in54_probe, .remove = purr_epd1in54_remove,
};
/* GT911 (touch) is gone from here too now -- converted to a real loadable driver module
 * (Modules/drivers/gt911_module.c), same as keyboard before it. See that file's own comment
 * and purr_kernel_touch_read() below for where the chip logic (status/point reads, the
 * keepalive quirk, the swap+mirror+rescale math) actually lives now. */

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
    if (h == NULL) {
        return 0;                  /* 0 always means "no handle" -- 1-based on purpose */
    }
    for (int i = 0; i < s_bus_handle_count; i++) {
        if (s_bus_handles[i] == NULL) {
            s_bus_handles[i] = h;  /* reuse a slot a prior remove_device() freed */
            return (uint32_t)(i + 1);
        }
    }
    if (s_bus_handle_count >= MAX_BUS_HANDLES) {
        return 0;
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
        .clock_speed_hz = clock_hz, .mode = mode, .spics_io_num = cs_pin, .queue_size = 2,
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

/* The I2C driver's interrupt reads and writes the caller's buffer directly. A buffer in PSRAM (a big allocation, or a task
 * stack placed there) cannot be touched while the flash cache is off, e.g. during a purrcfg or settings write, and the
 * interrupt then faults (LoadStorePIFAddrError in i2c_ll_read_rxfifo, plan7.md "VM panic"). So anything not in internal
 * RAM goes through a short-lived internal bounce buffer. */
static uint8_t *i2c_internal(const void *p, size_t len, int *bounced)
{
    *bounced = 0;
    if (p == NULL || len == 0 || esp_ptr_internal(p)) {
        return (uint8_t *)p;
    }
    uint8_t *b = heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    *bounced = b != NULL;
    return b;
}

static int bus_i2c_write(uint32_t handle, const uint8_t *data, size_t len, int timeout_ms)
{
    i2c_master_dev_handle_t dev = (i2c_master_dev_handle_t)handle_at(handle);
    if (dev == NULL) {
        return -1;
    }
    int bounced;
    uint8_t *tx = i2c_internal(data, len, &bounced);
    if (tx == NULL) {
        return -1;
    }
    if (bounced) {
        memcpy(tx, data, len);
    }
    int r = i2c_master_transmit(dev, tx, len, timeout_ms) == ESP_OK ? 0 : -1;
    if (bounced) {
        heap_caps_free(tx);
    }
    return r;
}

static int bus_i2c_read(uint32_t handle, uint8_t *data, size_t len, int timeout_ms)
{
    i2c_master_dev_handle_t dev = (i2c_master_dev_handle_t)handle_at(handle);
    if (dev == NULL) {
        return -1;
    }
    int bounced;
    uint8_t *rx = i2c_internal(data, len, &bounced);
    if (rx == NULL) {
        return -1;
    }
    int r = i2c_master_receive(dev, rx, len, timeout_ms) == ESP_OK ? 0 : -1;
    if (bounced) {
        if (r == 0) {
            memcpy(data, rx, len);
        }
        heap_caps_free(rx);
    }
    return r;
}

static int bus_i2c_write_read(uint32_t handle, const uint8_t *tx, size_t tx_len,
                              uint8_t *rx, size_t rx_len, int timeout_ms)
{
    i2c_master_dev_handle_t dev = (i2c_master_dev_handle_t)handle_at(handle);
    if (dev == NULL) {
        return -1;
    }
    int btx, brx;
    uint8_t *t = i2c_internal(tx, tx_len, &btx);
    uint8_t *r = i2c_internal(rx, rx_len, &brx);
    int res = -1;
    if (t != NULL && r != NULL) {
        if (btx) {
            memcpy(t, tx, tx_len);
        }
        res = i2c_master_transmit_receive(dev, t, tx_len, r, rx_len, timeout_ms) == ESP_OK ? 0 : -1;
        if (brx && res == 0) {
            memcpy(rx, r, rx_len);
        }
    }
    if (btx && t != NULL) {
        heap_caps_free(t);
    }
    if (brx && r != NULL) {
        heap_caps_free(r);
    }
    return res;
}

static void bus_i2c_remove_device(uint32_t handle)
{
    i2c_master_dev_handle_t dev = (i2c_master_dev_handle_t)handle_at(handle);
    if (dev == NULL) {
        return;
    }
    i2c_master_bus_rm_device(dev);
    if (handle >= 1 && handle <= (uint32_t)s_bus_handle_count) {
        s_bus_handles[handle - 1] = NULL;
    }
}

static void bus_gpio_set_output(int pin, int level) { pin_out(pin, level); }
static void bus_gpio_set_input_pullup(int pin) { pin_in_pullup(pin); }
static int bus_gpio_get(int pin) { return gpio_get_level((gpio_num_t)pin); }
static void bus_gpio_write(int pin, int level) { gpio_set_level((gpio_num_t)pin, level); }
static void bus_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
static uint64_t bus_now_us(void) { return (uint64_t)esp_timer_get_time(); }

static void *bus_dma_alloc(size_t bytes)
{
    return heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
}

/* Only one backlight channel exists on any board this project targets today -- same LEDC
 * timer/channel the compiled st7789 driver always used, just reached through the bus table
 * instead of linking driver/ledc.h directly into a module. */
static int s_backlight_ready;

static int bus_backlight_init(int pin)
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
        s_backlight_ready = 1;
        return 0;
    }
    return -1;
}

static void bus_backlight_set(uint8_t level)
{
    if (!s_backlight_ready) {
        return;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, level);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
}

static const purr_driver_bus_table_t s_bus_table = {
    .abi_version = PURR_DRIVER_BUS_ABI_VERSION,
    .spi_add_device = bus_spi_add_device,
    .spi_transfer = bus_spi_transfer,
    .i2c_add_device = bus_i2c_add_device,
    .i2c_write = bus_i2c_write,
    .i2c_read = bus_i2c_read,
    .i2c_write_read = bus_i2c_write_read,
    .i2c_remove_device = bus_i2c_remove_device,
    .gpio_set_output = bus_gpio_set_output,
    .gpio_set_input_pullup = bus_gpio_set_input_pullup,
    .gpio_get = bus_gpio_get,
    .gpio_write = bus_gpio_write,
    .delay_ms = bus_delay_ms,
    .now_us = bus_now_us,
    .dma_alloc = bus_dma_alloc,
    .backlight_init = bus_backlight_init,
    .backlight_set = bus_backlight_set,
};

const purr_driver_bus_table_t *purr_driver_bus_table(void)
{
    return &s_bus_table;
}

/* The drivers this build links in -- one line per driver (purr_driver.h's own comment on why
 * this is a plain table, not yet the spec's linker-section self-registration). A new real
 * driver is a new entry here plus its own source file -- no other kernel code changes. */
static const purr_driver_t *const s_drivers[] = {
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
     * A device's own failure is recorded (purr_device_state()) and never stops the rest.
     * display is NOT expected to be bound by this call anymore (st7789 converted to a real
     * loadable module, Modules/drivers/st7789_module.c) -- it can only load after the
     * filesystem mounts, well after this function returns, so main.c's own display-bound
     * check moved there too (purr_console.h's purr_console_init_headless() covers the
     * window in between). Returning ESP_OK here only means the board's bare hardware (power,
     * pins, buses) came up -- it says nothing about which drivers are bound yet. */
    purr_drivers_bind_all(b);
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

/* 1 and (*x, *y) set if currently touched, 0 otherwise (including "no touch driver bound" or
 * "touched but nothing new to report" -- a release event, say). All the real chip logic
 * (status/point register reads, the keepalive quirk, the signed-coordinate swap+mirror+
 * rescale math -- this unit's GT911 does not report the commonly assumed 0-4095 range) now
 * lives in the loaded driver module (Modules/drivers/gt911_module.c) -- this is just the
 * generic byte-read unpacked into the two coordinates the rest of the kernel wants, same
 * purr_device_read() indirection purr_kernel_key() already uses for the keyboard. */
int purr_kernel_touch_read(uint16_t *x, uint16_t *y)
{
    if (x == NULL || y == NULL) {
        return 0;
    }
    uint8_t buf[4];
    if (purr_device_read("touch", buf, sizeof(buf)) != 4) {
        return 0;
    }
    *x = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    *y = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    return 1;
}

int purr_kernel_handoff(purr_handoff_t *out)
{
#if CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC
    purr_handoff_t h;
    memcpy(&h, bootloader_common_get_rtc_retain_mem()->custom, sizeof(h));
    if (purr_handoff_valid(&h)) {
        *out = h;
        return 0;
    }
#endif
    (void)out;
    return -1;
}
