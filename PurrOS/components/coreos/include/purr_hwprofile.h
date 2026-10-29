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
 *   bus=i2c                 i2c (the default for an old file), or uart, spi, gpio or any word for a device configured by named values
 *   sda=21
 *   scl=22
 *   addr=0x76
 *
 * A device that is not I2C lists its settings as more key=value lines ("tx=43", "rx=44", "baud=38400"), up to 12 of them, keys up to 7 letters. Keys
 * that name a pin (tx rx cs rst busy dio1 dio2 pwr pps int en irq mosi miso sclk bck ws din dout mclk) must hold -1 (not connected) or a pin the chip
 * can drive. The driver reads them back with purr_kv(). Example, a GPS:
 *
 *   device=gps0
 *   compatible=gnss-nmea
 *   label=GPS receiver
 *   bus=uart
 *   port=1
 *   tx=43
 *   rx=44
 *   baud=38400
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

#define PURR_HWP_KV 12                 /* the same as PURR_KV_MAX in purr_board.h (the kernel's own header; this component cannot include it) */
#define PURR_HWP_BUS 6

typedef struct {
    char key[8];
    int32_t val;
} purr_hwp_kv_t;

typedef struct {
    char name[PURR_HWP_NAME];
    char compatible[PURR_HWP_COMPAT];
    char label[PURR_HWP_LABEL];
    int sda, scl;
    uint8_t addr;
    char bus[PURR_HWP_BUS];            /* "i2c" (or empty, an old entry), "uart", "spi", ... */
    uint8_t nkv;                       /* the named values below, for a device that is not I2C */
    purr_hwp_kv_t kv[PURR_HWP_KV];
} purr_hwp_dev_t;

/* 1 if the key names a pin (and so is checked against the reserved pins). */
int purr_hwp_key_is_pin(const char *key);
/* The value of a key, or the fallback. */
int32_t purr_hwp_kv_get(const purr_hwp_dev_t *d, const char *key, int32_t fallback);
/* Sets or adds a key. 0, or -1 if the key is bad (empty, too long, characters) or the list is full. */
int purr_hwp_kv_set(purr_hwp_dev_t *d, const char *key, int32_t val);

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
