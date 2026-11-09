/* purr_sd.h - the SD card as a folder: /sd.
 *
 * The card itself is a block driver (class PURR_DRV_CLASS_BLOCK, the "sd-spi" module, device "sd0"): it moves 512-byte sectors and knows nothing of files. This puts
 * a FAT file system on those sectors (FatFs through the C library's file calls, mounted at /sd) and offers the few file operations the Store and the shell need.
 * The card is looked for lazily: purr_sd_present() mounts it the first time it is asked, tries again at most every few seconds while there is no card, and lets
 * go when the card stops answering. Nothing here ever formats a card.
 *
 * Only the full profile has it; the others compile the same calls to "no card".
 */
#ifndef PURR_SD_H
#define PURR_SD_H

#include <stddef.h>
#include <stdint.h>

#include "purr_fs.h"

typedef struct {
    int driver;               /* a block driver is bound to the device */
    int mounted;
    uint8_t card;             /* PURR_BLOCK_CARD_*: 0 none, 1 SDSC, 2 SDHC/SDXC */
    uint32_t sectors;
    uint64_t total_bytes, free_bytes;
    char why[96];             /* in words, when it is not mounted */
} purr_sd_status_t;

/* Mounts the card at /sd now. 0 on success; otherwise -1 and `why` says what is missing (no driver, no card, no FAT file system). */
int purr_sd_mount(char *why, size_t cap);
void purr_sd_unmount(void);

/* 1 if /sd is usable: mounted, or mountable now (retried at most every 3 seconds). A card that stopped answering is let go. Cheap when it is mounted. */
int purr_sd_present(void);
void purr_sd_status(purr_sd_status_t *out);

/* File operations on a path under /sd, shaped like purr_fs's. 0 or a positive count on success, negative on failure. */
int purr_sd_read(const char *path, purr_fs_read_fn cb, void *ctx);
int purr_sd_write(const char *path, const void *data, uint32_t len);
int purr_sd_mkdir(const char *path);
int purr_sd_remove(const char *path);                  /* a file or an empty folder */
int purr_sd_stat(const char *path, int *is_dir, uint32_t *size);
int purr_sd_list(const char *path, purr_fs_list_fn cb, void *ctx);

/* True for "/sd" and anything under it. */
int purr_sd_is_path(const char *path);

#endif
