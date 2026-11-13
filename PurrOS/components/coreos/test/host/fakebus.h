/* A fake driver bus for host tests of loadable drivers: a UART fed from a script, an SPI whose transactions go to a callback, GPIO levels, a clock.
 * A driver is plain C that only touches hardware through purr_driver_bus_table_t, so it can be run here exactly as it is built for the device. */
#ifndef FAKEBUS_H
#define FAKEBUS_H

#include <stdint.h>
#include <string.h>

/* A test uses only some of these helpers. */
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"

#include "purr_board.h"
#include "purr_driver.h"
#include "purr_driver_bus.h"
#include "purr_driver_class.h"

/* ---- UART: what the driver reads comes from rx[], what it writes lands in tx[] */
static uint8_t fb_rx[8192], fb_tx[8192];
static size_t fb_rx_len, fb_rx_pos, fb_tx_len;
static int fb_uart_open_calls, fb_uart_port, fb_uart_tx_pin, fb_uart_rx_pin, fb_uart_baud, fb_uart_fail;
/* An optional reactor: called after every write, it may push a reply into rx (a modem answering a command). */
static void (*fb_on_write)(const uint8_t *data, size_t len);

static void fb_feed(const char *s)
{
    size_t n = strlen(s);
    memcpy(fb_rx + fb_rx_len, s, n);
    fb_rx_len += n;
}
static void fb_feed_bytes(const uint8_t *b, size_t n)
{
    memcpy(fb_rx + fb_rx_len, b, n);
    fb_rx_len += n;
}
static void fb_reset(void)
{
    fb_rx_len = fb_rx_pos = fb_tx_len = 0;
    fb_uart_open_calls = 0;
    fb_uart_fail = 0;
    fb_on_write = NULL;
}

static uint32_t fb_uart_open(int port, int tx, int rx, int baud, size_t buf)
{
    (void)buf;
    fb_uart_open_calls++;
    fb_uart_port = port;
    fb_uart_tx_pin = tx;
    fb_uart_rx_pin = rx;
    fb_uart_baud = baud;
    return fb_uart_fail ? 0 : 2;
}
static int fb_uart_write(uint32_t h, const uint8_t *d, size_t n)
{
    (void)h;
    memcpy(fb_tx + fb_tx_len, d, n);
    fb_tx_len += n;
    if (fb_on_write) {
        fb_on_write(d, n);
    }
    return (int)n;
}
static int fb_uart_read(uint32_t h, uint8_t *b, size_t cap, uint32_t timeout_ms)
{
    (void)h;
    (void)timeout_ms;
    size_t n = fb_rx_len - fb_rx_pos;
    if (n > cap) n = cap;
    memcpy(b, fb_rx + fb_rx_pos, n);
    fb_rx_pos += n;
    return (int)n;
}
static int fb_uart_baud_set(uint32_t h, int baud) { (void)h; fb_uart_baud = baud; return 0; }
static void fb_uart_close(uint32_t h) { (void)h; }

/* ---- I2C: one device at a time. fb_i2c_hook is called for every write (tx, n, NULL, 0) and every write-read (tx, n, rx, rn); it returns 0, or nonzero for a NACK. */
static int (*fb_i2c_hook)(const uint8_t *tx, size_t n, uint8_t *rx, size_t rn);
static int fb_i2c_port, fb_i2c_sda, fb_i2c_scl, fb_i2c_hz, fb_i2c_removed;
static uint16_t fb_i2c_addr;
static uint32_t fb_i2c_add(int port, int sda, int scl, uint16_t addr, int hz)
{
    fb_i2c_port = port;
    fb_i2c_sda = sda;
    fb_i2c_scl = scl;
    fb_i2c_addr = addr;
    fb_i2c_hz = hz;
    return 5;
}
static int fb_i2c_write(uint32_t h, const uint8_t *d, size_t n, int ms)
{
    (void)h;
    (void)ms;
    return fb_i2c_hook ? fb_i2c_hook(d, n, NULL, 0) : 0;
}
static int fb_i2c_read(uint32_t h, uint8_t *d, size_t n, int ms)
{
    (void)h;
    (void)ms;
    memset(d, 0, n);
    return fb_i2c_hook ? fb_i2c_hook(NULL, 0, d, n) : 0;
}
static int fb_i2c_write_read(uint32_t h, const uint8_t *tx, size_t tn, uint8_t *rx, size_t rn, int ms)
{
    (void)h;
    (void)ms;
    memset(rx, 0, rn);
    return fb_i2c_hook ? fb_i2c_hook(tx, tn, rx, rn) : 0;
}
static void fb_i2c_remove(uint32_t h) { (void)h; fb_i2c_removed++; }
static int fb_i2c_probes;
static int fb_i2c_probe(uint32_t h, int ms)
{
    (void)h;
    (void)ms;
    fb_i2c_probes++;
    return fb_i2c_hook ? fb_i2c_hook(NULL, 0, NULL, 0) : 0;      /* an empty transaction: the chip acknowledges its address, or does not */
}

/* ---- clock and GPIO */
static uint64_t fb_now;
static int fb_gpio[64];
static int fb_gpio_out[64];
static uint64_t fb_now_us(void) { return fb_now; }
static void fb_delay_ms(uint32_t ms) { fb_now += (uint64_t)ms * 1000; }
static void fb_gpio_set_output(int pin, int level) { if (pin >= 0 && pin < 64) { fb_gpio[pin] = level; fb_gpio_out[pin] = 1; } }
static void fb_gpio_set_input_pullup(int pin) { if (pin >= 0 && pin < 64) fb_gpio_out[pin] = 0; }
static int fb_gpio_get(int pin) { return pin >= 0 && pin < 64 ? fb_gpio[pin] : 0; }
static void fb_gpio_write(int pin, int level) { if (pin >= 0 && pin < 64) fb_gpio[pin] = level; }

/* ---- SPI: every transaction goes to fb_spi_hook(tx, rx, n); the default echoes zeros */
static void (*fb_spi_hook)(const uint8_t *tx, uint8_t *rx, size_t n);
static int fb_spi_cs, fb_spi_host;
static uint32_t fb_spi_add(int host, int cs, int mode, int hz)
{
    (void)mode;
    (void)hz;
    fb_spi_host = host;
    fb_spi_cs = cs;
    return 7;
}
static int fb_spi_transfer(uint32_t h, const uint8_t *tx, uint8_t *rx, size_t n)
{
    (void)h;
    if (rx) memset(rx, 0, n);
    if (fb_spi_hook) fb_spi_hook(tx, rx, n);
    return 0;
}

/* ABI 8: the driver's own SPI bus. The test sees what was asked for. */
static int fb_bus_calls, fb_bus_host, fb_bus_sclk, fb_bus_mosi, fb_bus_miso, fb_bus_fail;
static size_t fb_bus_max;
static int fb_spi_open_bus(int host, int sclk, int mosi, int miso, size_t max_transfer)
{
    fb_bus_calls++;
    fb_bus_host = host;
    fb_bus_sclk = sclk;
    fb_bus_mosi = mosi;
    fb_bus_miso = miso;
    fb_bus_max = max_transfer;
    return fb_bus_fail ? -1 : 0;
}
static int fb_lock_depth;
static void fb_spi_lock(void) { fb_lock_depth++; }
static void fb_spi_unlock(void) { fb_lock_depth--; }

static purr_driver_bus_table_t fb_table(void)
{
    purr_driver_bus_table_t t;
    memset(&t, 0, sizeof(t));
    t.abi_version = PURR_DRIVER_BUS_ABI_VERSION;
    t.uart_open = fb_uart_open;
    t.uart_write = fb_uart_write;
    t.uart_read = fb_uart_read;
    t.uart_set_baud = fb_uart_baud_set;
    t.uart_close = fb_uart_close;
    t.now_us = fb_now_us;
    t.delay_ms = fb_delay_ms;
    t.gpio_set_output = fb_gpio_set_output;
    t.gpio_set_input_pullup = fb_gpio_set_input_pullup;
    t.gpio_get = fb_gpio_get;
    t.gpio_write = fb_gpio_write;
    t.i2c_add_device = fb_i2c_add;
    t.i2c_write = fb_i2c_write;
    t.i2c_read = fb_i2c_read;
    t.i2c_write_read = fb_i2c_write_read;
    t.i2c_remove_device = fb_i2c_remove;
    t.i2c_probe = fb_i2c_probe;
    t.spi_add_device = fb_spi_add;
    t.spi_transfer = fb_spi_transfer;
    t.spi_open_bus = fb_spi_open_bus;
    t.spi_lock = fb_spi_lock;
    t.spi_unlock = fb_spi_unlock;
    return t;
}

/* A device as the kernel would hand it to probe(): named values in a purr_kv_cfg_t. */
static purr_kv_cfg_t fb_cfg;
static purr_device_t fb_dev(const char *name, const char *compatible)
{
    purr_device_t d;
    memset(&d, 0, sizeof(d));
    d.name = name;
    d.compatible = compatible;
    d.cfg = &fb_cfg;
    return d;
}
static void fb_cfg_set(const char *key, int32_t val)
{
    strcpy(fb_cfg.kv[fb_cfg.count].key, key);
    fb_cfg.kv[fb_cfg.count].val = val;
    fb_cfg.count++;
}

#endif
