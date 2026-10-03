/*
 * purr_fs.h - the root filesystem (LittleFS) behind a small API.
 *
 * The block device is abstract (read, program, erase), so the same code runs on a flash
 * partition on the device and on a RAM disk in the host tests. Paths are absolute, like
 * "/boot/kernel.kitt". All functions return 0 on success or a negative error (a LittleFS
 * error code); purr_fs_strerror turns one into words.
 */
#ifndef PURR_FS_H
#define PURR_FS_H

#include <stdint.h>

#include "lfs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int (*read)(void *ctx, uint32_t block, uint32_t off, void *buf, uint32_t size);
    int (*prog)(void *ctx, uint32_t block, uint32_t off, const void *buf, uint32_t size);
    int (*erase)(void *ctx, uint32_t block);
    int (*sync)(void *ctx);
    void *ctx;
    uint32_t block_size;                  /* erase unit, 4096 on the boards so far */
    uint32_t block_count;
} purr_bd_t;

#define PURR_FS_CACHE 256

typedef struct {
    lfs_t lfs;
    struct lfs_config cfg;
    purr_bd_t bd;
    uint8_t read_buf[PURR_FS_CACHE];
    uint8_t prog_buf[PURR_FS_CACHE];
    uint8_t look_buf[32];
    uint8_t file_buf[PURR_FS_CACHE];      /* one file open at a time */
    int mounted;
} purr_fs_t;

/* Mount an existing filesystem. Fails on a blank or damaged device: it never formats. */
int purr_fs_mount(purr_fs_t *fs, const purr_bd_t *bd);

/* Erase and create an empty filesystem, then mount it. Destroys everything on the device. */
int purr_fs_format(purr_fs_t *fs, const purr_bd_t *bd);

void purr_fs_unmount(purr_fs_t *fs);
int  purr_fs_mounted(const purr_fs_t *fs);

/* Blocks in use and in total (used space is approximate: it counts whole blocks). */
int purr_fs_usage(purr_fs_t *fs, uint32_t *used_blocks, uint32_t *total_blocks);

typedef void (*purr_fs_list_fn)(void *ctx, const char *name, int is_dir, uint32_t size);
int purr_fs_list(purr_fs_t *fs, const char *path, purr_fs_list_fn cb, void *ctx);

/* Stream a file to `cb` in pieces. If cb returns nonzero, reading stops with that value. */
typedef int (*purr_fs_read_fn)(void *ctx, const void *data, uint32_t len);
int purr_fs_read(purr_fs_t *fs, const char *path, purr_fs_read_fn cb, void *ctx);

/* Create or replace a file. The old contents stay until the new file is closed. */
int purr_fs_write(purr_fs_t *fs, const char *path, const void *data, uint32_t len);

int purr_fs_mkdir(purr_fs_t *fs, const char *path);
int purr_fs_remove(purr_fs_t *fs, const char *path);      /* a file or an empty directory */
int purr_fs_rename(purr_fs_t *fs, const char *from, const char *to);
int purr_fs_stat(purr_fs_t *fs, const char *path, int *is_dir, uint32_t *size);

const char *purr_fs_strerror(int err);

/*
 * File ownership (documentation/FINDINGS.md F-10, PurrOS/components/coreos/SPEC.md 4.2):
 * one LittleFS custom attribute per path, {owner_uid, mode}. mode is two bits -- there are
 * no groups, so nothing richer is meaningful. Root (uid 0) and an admin acting on their own
 * behalf always have full access regardless of mode; that check is account-aware and lives
 * above this layer (commands.c), not here.
 */
#define PURR_FS_MODE_OREAD  (1u << 0)   /* others besides the owner may read */
#define PURR_FS_MODE_OWRITE (1u << 1)   /* others besides the owner may write */

/* Not a LittleFS error -- the account-aware layer above returns this when ownership
 * refuses an access; purr_fs_strerror() still turns it into words since callers use that
 * for every fs_* error uniformly, this one included. Chosen to not collide with any
 * LFS_ERR_* value above (EACCES's usual POSIX number, -13, is free here). */
#define PURR_FS_ERR_DENIED (-13)

typedef struct {
    uint8_t owner_uid;    /* 0 = root */
    uint8_t mode;         /* PURR_FS_MODE_* bits */
} purr_fs_owner_t;

/* The owner/mode stored on exactly this path. LFS_ERR_NOATTR if this exact path has none
 * of its own -- see purr_fs_effective_owner, which walks up to find one. */
int purr_fs_get_owner(purr_fs_t *fs, const char *path, purr_fs_owner_t *out);

/* Sets (or replaces) the owner/mode stored on exactly this path. */
int purr_fs_set_owner(purr_fs_t *fs, const char *path, const purr_fs_owner_t *owner);

/* path's own owner/mode if set, otherwise the nearest ancestor directory's, otherwise
 * {0, 0} (root, no outside access) if nothing up to "/" has one set. Always fills *out. */
void purr_fs_effective_owner(purr_fs_t *fs, const char *path, purr_fs_owner_t *out);

/* The flash partition named `label` as a block device (device only, src/purr_fs_flash.c). */
int purr_fs_flash_bd(const char *label, purr_bd_t *bd);

#ifdef __cplusplus
}
#endif

#endif /* PURR_FS_H */
