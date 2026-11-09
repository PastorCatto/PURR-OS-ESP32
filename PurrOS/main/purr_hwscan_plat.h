/*
 * purr_hwscan_plat.h - the hardware report (plan8.md "Find hardware"): what is attached, found without asking questions.
 *
 * Read-only. Pins the board profile or the pin registry owns, and pins that belong to the flash, PSRAM, USB or the boot straps, are never
 * touched. A free pin is only looked at as an input (with a weak pull-down: a pin that still reads high has a pull-up resistor on the
 * board, which is what an I2C bus has). Each pair of such pins is then tried as SDA/SCL on a spare I2C port and every address is probed.
 * Blocks for a few seconds: call from a worker task or the shell.
 */
#ifndef PURR_HWSCAN_PLAT_H
#define PURR_HWSCAN_PLAT_H

#include <stdint.h>

#include "purr_cli.h"
#include "purr_hwscan.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_HWSCAN_MAX_PULLED 12
#define PURR_HWSCAN_MAX_FOUND  24

typedef struct {
    int sda, scl;
    uint8_t addr;
    int known;                 /* 1 if the table knows the address */
    purr_hw_i2c_match_t match;
} purr_hwscan_found_t;

typedef struct {
    int looked_at;                         /* free pins sensed */
    int skipped;                           /* reserved or in use, not touched */
    int pulled[PURR_HWSCAN_MAX_PULLED];    /* pins with a pull-up on the board */
    int npulled;
    purr_hwscan_found_t found[PURR_HWSCAN_MAX_FOUND];
    int nfound;
    int bus_error;                         /* 1 if the spare I2C port could not be used */
} purr_hwscan_report_t;

/* Runs the scan. power_pin: a GPIO to drive high first because the board gates its peripherals (only when the owner named it), or -1. */
int purr_hwscan_run(int power_pin, purr_hwscan_report_t *out);

/* The shell command: hwscan [power <gpio>] */
int purr_hwscan_cmd(purr_cli_t *cli, int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* PURR_HWSCAN_PLAT_H */
