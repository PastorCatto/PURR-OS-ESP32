/*
 * purr_drvpart.h - the `drivers` partition: the drivers a board cannot start without (Drivers/SPEC.md, "The driver partition").
 *
 * The display, the keyboard, the touch panel and the like used to be byte blobs compiled into every kernel. They are ordinary signed driver modules (.cat) now, kept in
 * a small raw partition of their own that is written when the image is made, and again whenever one is added or replaced: the kernel is not rebuilt for it. The
 * recovery images (the loader and KittenOS) have no file system, and read the same partition. Only the W5500 stays inside the kernel (a virtual device's flag).
 *
 *   ten slots of 8 KB, one driver each:      slot n is at n * PURR_DRVPART_SLOT_SIZE
 *     64-byte header:   magic "PDRV", version, flags, length of the .cat, crc32 of the .cat, the file name it is planted under, reserved (0xFF)
 *     the .cat:         the signed container, exactly as it is shipped (the signature is checked when it is loaded, as for every module)
 *     the rest:         0xFF
 *   a slot that is blank (all 0xFF in its header) is empty; a slot with a header that does not hold together is damaged (and skipped).
 *
 * Plain C with no ESP-IDF dependency, tested on the PC (test_drvpart.c); purrstrap/scripts/drvpart.py writes the same bytes.
 */
#ifndef PURR_DRVPART_H
#define PURR_DRVPART_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_DRVPART_SLOTS      10
#define PURR_DRVPART_SLOT_SIZE  8192u
#define PURR_DRVPART_SIZE       (PURR_DRVPART_SLOTS * PURR_DRVPART_SLOT_SIZE)     /* 0x14000: the partition's size */
#define PURR_DRVPART_HEADER     64u
#define PURR_DRVPART_MAX_CAT    (PURR_DRVPART_SLOT_SIZE - PURR_DRVPART_HEADER)
#define PURR_DRVPART_NAME_LEN   32                                                /* a file name, null-terminated */
#define PURR_DRVPART_MAGIC      0x56524450u                                       /* "PDRV" little-endian */
#define PURR_DRVPART_VERSION    1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;                      /* 0 for now */
    uint32_t length;                     /* bytes of .cat that follow the header */
    uint32_t crc32;                      /* purr_crc32 of those bytes */
    char file[PURR_DRVPART_NAME_LEN];    /* "gt911.cat" */
    uint8_t reserved[16];                /* 0xFF */
} purr_drvslot_hdr_t;

typedef struct {
    int slot;
    char file[PURR_DRVPART_NAME_LEN];
    uint32_t length;
} purr_drvslot_info_t;

/* Reads len bytes at `offset` of the partition into buf; 0 on success. */
typedef int (*purr_drvpart_read_fn)(void *ctx, uint32_t offset, void *buf, uint32_t len);

#define PURR_DRVPART_EMPTY    1
#define PURR_DRVPART_DAMAGED  (-1)

/* The header of one slot: 0 and *info filled, PURR_DRVPART_EMPTY for a blank slot, PURR_DRVPART_DAMAGED when the header is there but wrong (bad magic or version, a file name
 * that is not terminated or has a slash, a length of 0 or more than the slot holds), or a negative read error. */
int purr_drvpart_slot_info(purr_drvpart_read_fn rd, void *ctx, int slot, purr_drvslot_info_t *info);

/* The .cat of one slot into buf (cap bytes), its crc checked: 0 and *len, or PURR_DRVPART_EMPTY / PURR_DRVPART_DAMAGED (also for a crc that does not match, or a cap that is too
 * small), or the read error. */
int purr_drvpart_slot_read(purr_drvpart_read_fn rd, void *ctx, int slot, uint8_t *buf, uint32_t cap, uint32_t *len, purr_drvslot_info_t *info);

/* Fills one 8 KB slot image (slot_out must hold PURR_DRVPART_SLOT_SIZE bytes) for a file name and its .cat: 0, or -1 if the name or the size does not fit. */
int purr_drvpart_build_slot(uint8_t *slot_out, const char *file, const uint8_t *cat, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* PURR_DRVPART_H */
