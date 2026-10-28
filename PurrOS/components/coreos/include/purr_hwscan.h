/*
 * purr_hwscan.h - finding out what hardware is attached (Install/SPEC.md section 3, plan8.md "Find hardware").
 *
 * Policy and tables only, no ESP-IDF: which pins may be touched at all, which pins to try, and which chip an I2C address plus its ID
 * register is. The part that actually reads pins and talks on a bus is main/purr_hwscan_plat.c; a host test drives this part with a
 * fake bus (test_hwscan.c).
 *
 * The rule the whole feature follows: a scan only reads. It never drives a pin the board profile or the pin registry already owns, never
 * touches the pins of the flash, PSRAM, USB or the boot straps, and the only thing it does to a free pin is to look at it as an input.
 */
#ifndef PURR_HWSCAN_H
#define PURR_HWSCAN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t purr_pinmask_t;

#define PURR_PINBIT(n) ((n) >= 0 && (n) < 64 ? ((purr_pinmask_t)1 << (n)) : 0)

/* GPIOs a scan must never touch on an ESP32-S3: the strapping pins (0, 3, 45, 46), USB (19, 20), the flash and PSRAM lines (26-37),
 * and numbers that do not exist (22-25, 49 and up). */
purr_pinmask_t purr_hw_reserved_s3(void);

/* The pins a scan may look at, lowest first: everything except `reserved` and `in_use`. Returns how many were written. */
int purr_hw_candidate_pins(purr_pinmask_t reserved, purr_pinmask_t in_use, int *out, int max);

/* Reads len bytes from register `reg` (1 or 2 address bytes) of the device at addr. Returns 0 on success. */
typedef int (*purr_hw_reg_read_fn)(void *ctx, uint8_t addr, uint16_t reg, int reg_bytes, uint8_t *out, int len);

typedef struct {
    const char *name;          /* "GT911 touch controller" */
    const char *compatible;    /* the driver that serves it: "gt911" */
    const char *catcall;       /* what it provides: "touch", "display", "sensor", ... */
    int by_id;                 /* 1 if the ID register confirmed it, 0 if only the address matched */
} purr_hw_i2c_match_t;

/* What is at this address? Tries every table entry for it, ID register first. Returns 1 and fills out, or 0 for an address the table
 * does not know (listed as unknown). rd may be NULL: then only the address is used. */
int purr_hw_i2c_identify(uint8_t addr, purr_hw_reg_read_fn rd, void *ctx, purr_hw_i2c_match_t *out);

/* Number of entries in the table, for tests. */
int purr_hw_i2c_table_count(void);

#ifdef __cplusplus
}
#endif

#endif /* PURR_HWSCAN_H */
