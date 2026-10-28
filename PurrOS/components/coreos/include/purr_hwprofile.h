/*
 * purr_hwprofile.h - the discovered board profile (/etc/board.conf): devices that "Find hardware" found and the owner confirmed.
 *
 * The same stanza format as the manifests (key=value lines, blank line between devices, # comments), so it can be edited by hand and
 * shared back to the repo. Plain C, no ESP-IDF. The file is read as untrusted input: a pin outside what a scan may touch, or an address
 * outside the I2C range, drops that entry, so a corrupted or hand-edited file cannot make the system drive a flash or USB line.
 *
 *   device=bme280_0         a name, unique in the file
 *   compatible=bme280       the driver that serves it
 *   label=BME280 sensor     for people
 *   bus=i2c                 only I2C so far
 *   sda=21
 *   scl=22
 *   addr=0x76
 */
#ifndef PURR_HWPROFILE_H
#define PURR_HWPROFILE_H

#include <stddef.h>
#include <stdint.h>

#include "purr_hwscan.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_HWP_MAX      8
#define PURR_HWP_NAME     24
#define PURR_HWP_COMPAT   32
#define PURR_HWP_LABEL    40

typedef struct {
    char name[PURR_HWP_NAME];
    char compatible[PURR_HWP_COMPAT];
    char label[PURR_HWP_LABEL];
    int sda, scl;
    uint8_t addr;
} purr_hwp_dev_t;

typedef struct {
    purr_hwp_dev_t dev[PURR_HWP_MAX];
    int count;
    int dropped;                       /* entries in the file that were rejected */
} purr_hwp_t;

/* Parses a file. `reserved` is purr_hw_reserved_s3() (or whatever the chip's reserved pins are): an entry using one is dropped. Returns the number kept. */
int purr_hwp_parse(purr_hwp_t *p, const char *text, size_t len, purr_pinmask_t reserved);

/* Writes the file text into out. Returns the length, or -1 if cap is too small. */
int purr_hwp_format(const purr_hwp_t *p, char *out, size_t cap);

/* Adds a device (naming it "<compatible>_<n>" if d->name is empty). 0 added, 1 an identical device (same driver, address and pins) is already there, -1 full or invalid. */
int purr_hwp_add(purr_hwp_t *p, const purr_hwp_dev_t *d, purr_pinmask_t reserved);

/* 1 if the device is valid on its own: names and driver set, a 7-bit address, two different pins, neither reserved. */
int purr_hwp_dev_valid(const purr_hwp_dev_t *d, purr_pinmask_t reserved);

#ifdef __cplusplus
}
#endif

#endif /* PURR_HWPROFILE_H */
