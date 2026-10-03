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

#include "purr_board.h"

#ifdef __cplusplus
extern "C" {
#endif

/* probe()/remove() return plain int, not esp_err_t -- esp_err_t is itself just an int typedef,
 * and using it here would pull esp_err.h into this otherwise ESP-IDF-free header. 0 is success,
 * matching ESP_OK's own value, so a real driver's esp_err_t can be returned as-is. */
typedef int (*purr_driver_probe_fn)(const purr_device_t *dev, const purr_board_t *board, void **out_handle);
typedef void (*purr_driver_remove_fn)(void *handle);

typedef struct {
    const char *name;
    const char *compatible;       /* matches a purr_device_t's own `compatible` */
    purr_driver_probe_fn probe;
    purr_driver_remove_fn remove;
} purr_driver_t;

/* One device's outcome, kernel/SPEC.md section 5. PURR_DEV_UNBOUND is a device
 * purr_drivers_bind_all() has not reached yet (or was never called) -- distinct from any
 * attempted-and-failed state. */
typedef enum {
    PURR_DEV_UNBOUND = 0,
    PURR_DEV_BOUND,
    PURR_DEV_NO_DRIVER,
    PURR_DEV_PROBE_FAILED,
    PURR_DEV_PIN_CONFLICT,
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

/* The real one: purr_driver_table() as the driver list. */
void purr_drivers_bind_all(const purr_board_t *board);

purr_device_state_t purr_device_state(const char *device_name);

/* NULL unless purr_device_state(device_name) == PURR_DEV_BOUND. */
void *purr_device_handle(const char *device_name);

#ifdef __cplusplus
}
#endif

#endif /* PURR_DRIVER_H */
