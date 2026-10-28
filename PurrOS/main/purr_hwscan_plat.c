/* purr_hwscan_plat.c - see purr_hwscan_plat.h. */
#include "purr_hwscan_plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "login.h"
#include "purr_board.h"
#include "purr_driver.h"

static const char *TAG = "hwscan";

#define SCAN_PORT 1                    /* the kernel's own bus uses port 0 */

/* Every pin the board profile names, plus anything the pin registry has claimed. */
static purr_pinmask_t pins_in_use(void)
{
    const purr_board_t *b = purr_board();
    purr_pinmask_t m = 0;
    const int named[] = {b->power_pin, b->spi.mosi, b->spi.miso, b->spi.sclk, b->display.cs, b->display.dc, b->display.rst,
                         b->display.backlight, b->keyboard.sda, b->keyboard.scl, b->touch.sda, b->touch.scl, b->touch.int_pin,
                         b->touch.rst_pin, b->btn_boot_pin, b->btn_pwr_pin};
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        m |= PURR_PINBIT(named[i]);
    }
    for (int i = 0; i < PURR_MAX_IDLE_PINS; i++) {
        m |= PURR_PINBIT(b->idle_high_pins[i]);
    }
    for (int i = 0; i < b->device_count; i++) {
        for (int j = 0; j < PURR_MAX_EXTRA_PINS; j++) {
            m |= PURR_PINBIT(b->devices[i].extra_pins[j]);
        }
    }
    for (int p = 0; p < 49; p++) {
        if (purr_pins_owner(p) != NULL) {
            m |= PURR_PINBIT(p);
        }
    }
    return m;
}

/* Does the pin have a pull-up outside the chip? Input with a weak pull-down: it still reads high only if something pulls harder. */
static int has_pullup(int pin)
{
    gpio_config_t c = {.pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_DISABLE,
                       .pull_down_en = GPIO_PULLDOWN_ENABLE, .intr_type = GPIO_INTR_DISABLE};
    if (gpio_config(&c) != ESP_OK) {
        return 0;
    }
    esp_rom_delay_us(800);
    int high = gpio_get_level(pin);
    c.mode = GPIO_MODE_DISABLE;                          /* leave it as we found it: off, no pulls */
    c.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&c);
    return high;
}

typedef struct {
    i2c_master_bus_handle_t bus;
} rd_ctx_t;

static int reg_read(void *vctx, uint8_t addr, uint16_t reg, int reg_bytes, uint8_t *out, int len)
{
    rd_ctx_t *c = vctx;
    i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 100000};
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(c->bus, &dc, &dev) != ESP_OK) {
        return -1;
    }
    uint8_t rb[2] = {(uint8_t)(reg >> 8), (uint8_t)reg};
    const uint8_t *w = reg_bytes == 2 ? rb : &rb[1];
    esp_err_t e = i2c_master_transmit_receive(dev, w, (size_t)reg_bytes, out, (size_t)len, 50);
    i2c_master_bus_rm_device(dev);
    return e == ESP_OK ? 0 : -1;
}

static void scan_pair(int sda, int scl, purr_hwscan_report_t *r)
{
    i2c_master_bus_config_t bc = {.i2c_port = SCAN_PORT, .sda_io_num = sda, .scl_io_num = scl, .clk_source = I2C_CLK_SRC_DEFAULT,
                                  .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = false};
    rd_ctx_t ctx = {0};
    if (i2c_new_master_bus(&bc, &ctx.bus) != ESP_OK) {
        r->bus_error = 1;
        return;
    }
    for (uint8_t addr = 0x08; addr <= 0x77 && r->nfound < PURR_HWSCAN_MAX_FOUND; addr++) {
        if (i2c_master_probe(ctx.bus, addr, 25) != ESP_OK) {
            continue;
        }
        purr_hwscan_found_t *f = &r->found[r->nfound++];
        memset(f, 0, sizeof(*f));
        f->sda = sda;
        f->scl = scl;
        f->addr = addr;
        f->known = purr_hw_i2c_identify(addr, reg_read, &ctx, &f->match);
    }
    i2c_del_master_bus(ctx.bus);
}

int purr_hwscan_run(int power_pin, purr_hwscan_report_t *r)
{
    memset(r, 0, sizeof(*r));
    purr_pinmask_t reserved = purr_hw_reserved_s3(), in_use = pins_in_use();
    if (power_pin >= 0) {
        if ((reserved & PURR_PINBIT(power_pin)) || power_pin > 48) {
            return -1;
        }
        gpio_config_t c = {.pin_bit_mask = 1ULL << power_pin, .mode = GPIO_MODE_OUTPUT};
        gpio_config(&c);
        gpio_set_level(power_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(100));                  /* the rail settles before anything is looked at */
        in_use |= PURR_PINBIT(power_pin);
    }
    int pins[64];
    int n = purr_hw_candidate_pins(reserved, in_use, pins, 64);
    for (int p = 0; p < 49; p++) {
        if ((reserved | in_use) & PURR_PINBIT(p)) {
            r->skipped++;
        }
    }
    for (int i = 0; i < n; i++) {
        r->looked_at++;
        if (has_pullup(pins[i]) && r->npulled < PURR_HWSCAN_MAX_PULLED) {
            r->pulled[r->npulled++] = pins[i];
        }
    }
    ESP_LOGI(TAG, "%d free pins looked at, %d with a pull-up", r->looked_at, r->npulled);
    for (int a = 0; a < r->npulled; a++) {
        for (int b = 0; b < r->npulled; b++) {
            if (a != b) {
                scan_pair(r->pulled[a], r->pulled[b], r);
            }
        }
    }
    return 0;
}

int purr_hwscan_cmd(purr_cli_t *cli, int argc, char **argv)
{
    int power = -1;
    if (argc >= 3 && strcmp(argv[1], "power") == 0) {
        power = atoi(argv[2]);
        purr_cli_printf(cli, "hwscan: driving GPIO %d high first, as you asked. Only do this if you know it is the peripheral power switch.\n", power);
    } else if (argc >= 2) {
        purr_cli_printf(cli, "usage: hwscan [power <gpio>]\n");
        return 1;
    }
    static purr_hwscan_report_t r;
    if (purr_hwscan_run(power, &r) != 0) {
        purr_cli_printf(cli, "hwscan: that pin cannot be used (reserved for flash, USB or boot)\n");
        return 1;
    }
    const purr_board_t *b = purr_board();
    purr_cli_printf(cli, "board profile \"%s\": already set up:\n", b->name);
    for (int i = 0; i < b->device_count; i++) {
        purr_cli_printf(cli, "  %-16s driver \"%s\"\n", b->devices[i].name, b->devices[i].compatible);
    }
    purr_cli_printf(cli, "looked at %d free pins (%d reserved or in use, not touched); %d have a pull-up:", r.looked_at, r.skipped, r.npulled);
    for (int i = 0; i < r.npulled; i++) {
        purr_cli_printf(cli, " %d", r.pulled[i]);
    }
    purr_cli_printf(cli, "\n");
    if (r.bus_error) {
        purr_cli_printf(cli, "(the spare I2C port could not be used for some pairs)\n");
    }
    if (r.nfound == 0) {
        purr_cli_printf(cli, "no I2C devices answered on any of those pins%s\n",
                        power < 0 && b->power_pin < 0 ? ". If the board switches its peripherals with a power pin, try: hwscan power <gpio>" : "");
        return 0;
    }
    for (int i = 0; i < r.nfound; i++) {
        const purr_hwscan_found_t *f = &r.found[i];
        if (f->known) {
            purr_cli_printf(cli, "I2C sda %d scl %d  0x%02X  %s (driver \"%s\", %s)%s\n", f->sda, f->scl, f->addr, f->match.name,
                            f->match.compatible, f->match.catcall, f->match.by_id ? "" : "  [guessed from the address]");
        } else {
            purr_cli_printf(cli, "I2C sda %d scl %d  0x%02X  unknown device\n", f->sda, f->scl, f->addr);
        }
    }
    return 0;
}
