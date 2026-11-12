/*
 * purr_driver.h - the driver registry (kernel/SPEC.md sections 2, 5, 6; F-02).
 *
 * Plain C, no ESP-IDF dependency -- host-testable with fake drivers and a fake board, the
 * same way purr_fs.c/purr_manifest.c are. A real driver's own probe()/remove() is free to
 * call ESP-IDF directly (same as st7789.c always has); only the matching, pin-ownership and
 * bind-order logic lives here.
 *
 * Driver registration is a plain static table (src/purr_driver_table.c), not yet the spec's
 * linker-section self-registration -- that's a real refinement for if/when the table
 * outgrows one hand-edited line per driver, not a blocker for plug-and-play today: adding a
 * driver is still "add one source file, add one line," no kernel logic changes.
 */
#ifndef PURR_DRIVER_H
#define PURR_DRIVER_H

#include <stddef.h>

#include "purr_board.h"

#ifdef __cplusplus
extern "C" {
#endif

/* probe()/remove() return plain int, not esp_err_t -- esp_err_t is itself just an int typedef,
 * and using it here would pull esp_err.h into this otherwise ESP-IDF-free header. 0 is success,
 * matching ESP_OK's own value, so a real driver's esp_err_t can be returned as-is. */
typedef int (*purr_driver_probe_fn)(const purr_device_t *dev, const purr_board_t *board, void **out_handle);
typedef void (*purr_driver_remove_fn)(void *handle);

/* Optional (NULL if this driver has nothing to poll/read after probe() -- a display driver,
 * say). POSIX read()'s own convention: >=0 is the number of bytes actually read (0 means
 * "nothing available right now", not an error), <0 is an error. */
typedef int (*purr_driver_read_fn)(void *handle, uint8_t *data, size_t len);

typedef struct {
    const char *name;
    const char *compatible;       /* matches a purr_device_t's own `compatible` */
    purr_driver_probe_fn probe;
    purr_driver_remove_fn remove;
    purr_driver_read_fn read;     /* optional */
    /* The purr_driver_bus_table_t layout this driver was compiled against: every loadable
     * driver sets it to PURR_DRIVER_BUS_ABI_VERSION (purr_driver_bus.h), taken from the same
     * header it was built with. The loader refuses a mismatch. Without it, a driver built
     * before a slot was inserted into the bus table silently calls the WRONG function: found
     * for real, 2026-10-25, when a stale touch module's now_us() landed on delay_ms() after
     * gpio_write() was inserted ahead of it (ABI 3 to 4) and put the whole UI task to sleep for
     * 12.8 days. Compiled-in drivers leave it 0; only the module loader checks it. */
    uint32_t bus_abi;
    /* Bus ABI 6 and later: what kind of driver this is and the interface it offers (purr_driver_class.h). Only meaningful when bus_abi is 6 or more
     * (or 0, for a compiled-in driver); a driver built for an older ABI does not have these fields, and purr_driver_class_id() never reads them. */
    uint32_t class_id;            /* PURR_DRV_CLASS_*, 0 for none */
    const void *ops;              /* the class's ops table, whose first field is its version */
} purr_driver_t;

/* The driver's class, or 0 for none or for a driver too old to have one. Always use this, never drv->class_id. */
uint32_t purr_driver_class_id(const purr_driver_t *drv);

/* One device's outcome, kernel/SPEC.md section 5. PURR_DEV_UNBOUND is a device
 * purr_drivers_bind_all() has not reached yet (or was never called) -- distinct from any
 * attempted-and-failed state. */
typedef enum {
    PURR_DEV_UNBOUND = 0,
    PURR_DEV_BOUND,
    PURR_DEV_NO_DRIVER,
    PURR_DEV_PROBE_FAILED,
    PURR_DEV_PIN_CONFLICT,
    PURR_DEV_DEFERRED,            /* flagged PURR_DEFER_NET and waiting for Wi-Fi to settle; no pins claimed, nothing probed */
} purr_device_state_t;

/* ---------------------------------------------------------------- pin registry */

/* Claims `pin` for `owner`. 0 on success. On conflict, returns nonzero and leaves the
 * existing claim untouched -- purr_pins_owner(pin) says who already has it. Claiming
 * PURR_PIN_NONE always succeeds and claims nothing (a device with no pin in that slot). */
int purr_pins_claim(int pin, const char *owner);

/* The current owner of `pin`, or NULL if free (or PURR_PIN_NONE). */
const char *purr_pins_owner(int pin);

/* Releases every claim. Real boot never calls this (claims live for the process); it exists
 * for tests, which need a clean registry between cases. */
void purr_pins_reset(void);

/* Releases every pin currently claimed by `owner` (string-compared, same as purr_pins_claim).
 * purr_driver_unregister()'s own cleanup step -- a device giving up its driver must give back
 * its pins too, or the next driver that wants them is refused for no real reason. */
void purr_pins_release_owner(const char *owner);

/* ---------------------------------------------------------------- driver table */

/* The drivers this build links in (src/purr_driver_table.c for the real one; host tests
 * supply their own). */
const purr_driver_t *const *purr_driver_table(int *count);

/* ---------------------------------------------------------------- binding */

/* Walks board->devices: for each, claims its extra_pins (a conflict stops at
 * PURR_DEV_PIN_CONFLICT without calling probe), finds a driver in `table`/`table_count`
 * whose `compatible` matches, and calls its probe(). Never panics on one device's failure --
 * records the outcome and moves on to the next (kernel/SPEC.md section 5). Resets every
 * device to PURR_DEV_UNBOUND first, so calling this twice re-binds cleanly. */
void purr_drivers_bind(const purr_board_t *board, const purr_driver_t *const *table, int table_count);

/* Declares a device the board table does not list, because only the machine it runs on knows it is there (a virtual device's Ethernet controller). `dev` and
 * everything it points to must stay valid for good. It is tracked, bound and found by class like any other device: now if a driver for its `compatible` is
 * already registered, and when one is registered later. -1 if the name is taken (by the board or an earlier call) or the table is full. */
int purr_device_add(const purr_board_t *board, const purr_device_t *dev);

/* The real one: purr_driver_table() as the driver list. */
void purr_drivers_bind_all(const purr_board_t *board);

/* Wi-Fi has settled (it has an address, or will not get one soon): binds the devices that were waiting for it (PURR_DEFER_NET). Safe to call more than once. A
 * device flagged this way that is registered after this binds at once. */
void purr_drivers_net_settled(const purr_board_t *board);
int purr_drivers_net_is_settled(void);

/* ---------------------------------------------------------------- plug-and-play
 *
 * purr_drivers_bind() above is the one-time cold-boot pass: it resets every record and
 * re-probes everything from scratch, which is correct exactly once, at the start, and wrong
 * any other time (it would re-probe already-live hardware). register()/unregister() are the
 * real plug-and-play pair for everything after that -- a driver loaded from a file well after
 * boot (or swapped out later) goes through these, never through purr_drivers_bind() again.
 */

/* Adds `drv` to the registry, then immediately matches it against `board`'s devices: any
 * device not already PURR_DEV_BOUND whose `compatible` matches gets claimed and probed right
 * here, same as purr_drivers_bind()'s own per-device logic. A device already bound to a
 * different driver is left alone -- registering a driver never steals a working device out
 * from under one that already has it; that is what unregister()+register() is for. 0 on
 * success, nonzero if the registry is full. */
int purr_driver_register(const purr_board_t *board, const purr_driver_t *drv);

/* Devices that arrived after boot (the discovered board profile, /etc/board.conf): matches each unbound device of `board` against every
 * driver registered so far, and probes it. Safe to call again after more drivers are registered. */
void purr_drivers_bind_extra(const purr_board_t *board);

/* 1 if a driver for this compatible string is registered (loaded this boot). */
int purr_driver_registered(const char *compatible);

/* Called around every driver's probe (its first contact with the hardware), with begin = 1 before and 0 after. The OS writes what is being started to a marker
 * file and clears it afterwards, so a driver that stops the device is known on the next start (main/commands.c). The default does nothing. */
void purr_driver_guard(const char *driver_name, int begin);

/* The other half: finds every device currently bound to exactly `drv`, calls its remove(),
 * releases the pins it claimed, and resets it to PURR_DEV_UNBOUND -- ready for a fresh
 * register() (a different driver, or a reloaded version of the same one) to claim it. A no-op
 * if `drv` was never registered or has nothing bound. */
void purr_driver_unregister(const purr_driver_t *drv);

/* Test-only, same spirit as purr_pins_reset(): clears the registered-drivers list without
 * calling anyone's remove(). Real boot never calls this. */
void purr_driver_registry_reset(void);

/* ---------------------------------------------------------------- driver classes */

typedef struct {
    const char *device;           /* the device's name */
    uint32_t class_id;
    const void *ops;              /* the ops table of the class; cast to purr_gnss_ops_t and the like */
    void *handle;                 /* pass to every ops function */
} purr_class_entry_t;

/* The bound devices whose driver is of this class, up to max, in the order they were bound. Returns how many there are (which may be more than max). */
int purr_class_find(uint32_t class_id, purr_class_entry_t *out, int max);

/* The first such device's ops table (and handle), or NULL if there is none. */
const void *purr_class_ops(uint32_t class_id, void **handle_out);

purr_device_state_t purr_device_state(const char *device_name);

/* NULL unless purr_device_state(device_name) == PURR_DEV_BOUND. */
void *purr_device_handle(const char *device_name);

/* The driver currently bound to this device, or NULL. Lets a caller (a future swap command)
 * find out what to unregister() before registering a replacement. */
const purr_driver_t *purr_device_driver(const char *device_name);

/* Calls the bound driver's read(handle, data, len). -1 if the device is not bound or its
 * driver has no read(). Lets a caller like purr_kernel_key() go through the device's own
 * driver instead of fetching the raw handle and guessing its real type. */
int purr_device_read(const char *device_name, uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* PURR_DRIVER_H */
