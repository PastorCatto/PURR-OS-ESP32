/*
 * purr_driver_bus.h - what a loaded driver module gets handed (Drivers/SPEC.md section 4,
 * kernel/SPEC.md section 11's own rule: "the kernel talks to the platform only through the
 * host API table", scaled down to just what a hardware driver needs).
 *
 * A driver module is freestanding (Modules/SPEC.md section 2/4): no libc, no linkage against
 * the kernel's own compiled code. It can only touch hardware through this table, the same
 * discipline purr_core_table_t already imposes on a CoreOS-side module, just scoped to raw
 * bus/pin primitives instead of filesystem/login/app calls. Plain C, no ESP-IDF types in this
 * header, so a driver module's own source can include it freely -- the real implementation
 * (purr_kernel.c) is where the ESP-IDF coupling lives.
 *
 * A device handle here is already resolved to the one bus/chip-select/address the board
 * profile granted that device -- a driver module never sees a raw pin number for its own bus
 * wiring, only the handle. GPIO is the exception (reset/backlight/interrupt lines): those are
 * raw pin numbers, meant to only ever be the pins the board profile's extra_pins gave this
 * device. Not yet enforced per call here (this table has no notion of "which driver is
 * calling" to check against) -- the real guarantee today is at bind time: purr_drivers_bind()
 * already refuses to bind two devices that claim the same pin (purr_driver.c's pin registry),
 * so a misconfigured board profile is caught before any driver runs. A misbehaving driver
 * deliberately poking a pin outside its own claim is a real, separate hardening gap, not
 * covered yet.
 */
#ifndef PURR_DRIVER_BUS_H
#define PURR_DRIVER_BUS_H

#include <stddef.h>
#include <stdint.h>

#include "purr_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_DRIVER_BUS_ABI_VERSION 4u
/* The oldest bus table layout this kernel still serves. The table only ever grows at the end (see Drivers/SPEC.md section 4.1), so a driver
 * built against any version from this one up to the current works unchanged: the driver's version is independent of the OS's. Raise this
 * only if a slot is ever moved, which the append-only rule forbids. */
#define PURR_DRIVER_BUS_ABI_MIN     4u

typedef struct {
    uint32_t abi_version;

    /* SPI: one device handle per probe() call, already bound to the board profile's bus/cs/
     * clock for this device. 0 returned from spi_add_device on failure (never a valid handle,
     * since a handle of 0 would be indistinguishable from "no device"). */
    uint32_t (*spi_add_device)(int host, int cs_pin, int mode, int clock_hz);
    int (*spi_transfer)(uint32_t handle, const uint8_t *tx, uint8_t *rx, size_t len);

    /* I2C, same shape. timeout_ms matters on all three: a device a driver polls in a tight
     * loop (a keyboard's single-byte read, a touch controller's status/point registers via
     * write_read) needs a short, bounded timeout -- an unresponsive chip must never be able to
     * hang whatever task is polling it. */
    uint32_t (*i2c_add_device)(int port, int sda_pin, int scl_pin, uint16_t addr, int clock_hz);
    int (*i2c_write)(uint32_t handle, const uint8_t *data, size_t len, int timeout_ms);
    int (*i2c_read)(uint32_t handle, uint8_t *data, size_t len, int timeout_ms);
    int (*i2c_write_read)(uint32_t handle, const uint8_t *tx, size_t tx_len,
                          uint8_t *rx, size_t rx_len, int timeout_ms);
    /* Removes a device handle added via i2c_add_device -- a driver that probes more than one
     * possible address (GT911's primary/alt address fallback) needs to drop the failed one
     * before adding the real address, or it leaks a bus-table handle slot forever. */
    void (*i2c_remove_device)(uint32_t handle);

    /* GPIO: only ever the pins this device's own board-profile entry already claimed. */
    void (*gpio_set_output)(int pin, int level);
    void (*gpio_set_input_pullup)(int pin);
    int (*gpio_get)(int pin);
    /* gpio_set_output() above reconfigures the pin (gpio_config()) on every call -- fine for
     * a reset pulse, too slow for a line toggled per SPI chunk (a display's D/C line, flipped
     * on every command/data boundary during a blit). gpio_write() just sets the level on a
     * pin gpio_set_output() has already configured once. */
    void (*gpio_write)(int pin, int level);

    void (*delay_ms)(uint32_t ms);
    uint64_t (*now_us)(void);

    /* A DMA-capable buffer, allocated once and kept forever -- same lifetime as the module
     * itself (Modules/SPEC.md: a loaded module is never unloaded today). NULL on failure.
     * Freestanding modules have no malloc/heap_caps_malloc of their own, and the one driver
     * that needs this (a display, chunking pixel data to the panel over SPI/DMA) needs
     * memory the DMA engine can actually reach -- ordinary module .bss is not guaranteed to
     * be. No dma_free(): nothing in this project frees a loaded module's own allocations
     * today, display included. */
    void *(*dma_alloc)(size_t bytes);

    /* Display backlight PWM -- scoped to exactly this one real need (not a general PWM
     * subsystem) since it is the only driver so far that wants anything beyond plain GPIO
     * on/off. Only one backlight channel exists on any board this project targets today,
     * so there is no handle -- backlight_init() claims it, backlight_set() drives it. 0 on
     * success, nonzero if the channel could not be configured (backlight_set() is then a
     * silent no-op, same as the compiled st7789 driver's own s.backlight_ready check). */
    int (*backlight_init)(int pin);
    void (*backlight_set)(uint8_t level);
} purr_driver_bus_table_t;

/* core -> module direction (Modules/SPEC.md section 6, scoped to drivers): called once per
 * loaded driver module, right after it's relocated and mapped executable. Returns the one
 * purr_driver_t this module provides -- NULL on any failure the module wants to report before
 * ever being matched against a device. */
typedef const purr_driver_t *(*purr_driver_entry_fn)(const purr_driver_bus_table_t *bus);

/* The real one (purr_kernel.c) -- a loaded driver module's entry() is called with this. */
const purr_driver_bus_table_t *purr_driver_bus_table(void);

#ifdef __cplusplus
}
#endif

#endif /* PURR_DRIVER_BUS_H */
