/*
 * purr_kernel_table.h - the first real slice of the kernel-to-CoreOS call table
 * (PurrOS/components/coreos/SPEC.md section 4.1), one layer BELOW purr_module_abi.h's
 * CoreOS-to-module table, not the same thing.
 *
 * This project doesn't have a separate flashed kernel binary yet (PurrOS/SPEC.md section 11's
 * "not yet built" list) -- the current monolith plays both roles for now: it hosts this
 * table's real implementation (backed by real heap_caps_ and esp_timer_get_time calls, see
 * commands.c's s_kernel_table) AND loads modules against it from a dedicated /kernelmods
 * folder on LittleFS, kept separate from /system (the CoreOS-to-module boundary,
 * Modules/SPEC.md) on purpose, so the two boundaries stay conceptually distinct even while
 * both are hosted in one binary today.
 *
 * Grows field by field as real CoreOS commands convert, same as purr_core_table_t did --
 * this is the first slice, sized to what `mem` and `uptime` (the first real commands ported
 * this way, Modules/coreos/sysinfo_module.c) actually need, not the full placeholder sketch
 * in coreos/SPEC.md section 4.1.
 */
#ifndef PURR_KERNEL_TABLE_H
#define PURR_KERNEL_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "purr_cli.h"
#include "purr_fs.h"    /* purr_fs_list_fn/purr_fs_read_fn -- already proven safe for a
                          * freestanding module build (purr_module_abi.h pulls it in
                          * transitively via purr_appmgr.h, and all four real modules build
                          * fine today), so fs_list/fs_read below just reuse its callback
                          * types instead of redeclaring them. */

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped from 1: the table grew (fs_*, console_flush) for the filesystem command sweep
 * (`ls`/`cat`/`mkdir`/`rm`/`mv`/`write`/`df`/`format`, Modules/coreos/fs_module.c). */
#define PURR_KERNEL_TABLE_ABI_VERSION 2u

typedef struct {
    void (*puts)(purr_cli_t *cli, const char *s);
    void (*printf)(purr_cli_t *cli, const char *fmt, ...);

    /* Real heap, the same as purr_core_table_t's -- the kernel already owns it either way. */
    void *(*heap_alloc)(size_t n);
    void (*heap_free)(void *p);

    /* What `mem` needs: read-only heap queries, real numbers from the running allocator, not
     * something CoreOS could compute or fake on its own. */
    uint32_t (*heap_free_internal)(void);
    uint32_t (*heap_largest_free_internal)(void);
    uint32_t (*heap_free_psram)(void);
    uint32_t (*heap_total_psram)(void);

    /* What `uptime` needs: a real running timer the kernel owns. */
    uint64_t (*uptime_us)(void);

    /* The mounted root filesystem: the kernel owns the flash/LittleFS driver
     * (PurrOS/components/coreos/SPEC.md responsibility #1), CoreOS never touches it
     * directly. Same shapes purr_fs.h's own calls already have; reached through the table
     * instead. Errors are plain purr_fs.h codes, turned into words by fs_strerror -- not
     * decomposed further, since that's already what the direct calls returned before. */
    int (*fs_list)(const char *path, purr_fs_list_fn cb, void *ctx);
    int (*fs_read)(const char *path, purr_fs_read_fn cb, void *ctx);
    int (*fs_write)(const char *path, const void *data, uint32_t len);
    int (*fs_mkdir)(const char *path);
    int (*fs_remove)(const char *path);
    int (*fs_rename)(const char *from, const char *to);
    int (*fs_usage)(uint32_t *used, uint32_t *total);
    int (*fs_mounted)(void);
    const char *(*fs_strerror)(int err);
    /* Deliberately one opaque call, not decomposed into flash_bd/unmount/format primitives
     * (same reasoning as netinstall's net_install, Modules/SPEC.md): formatting needs the
     * raw block device and an unmount first, real low-level access a module is never
     * handed. Returns 0 on success, a purr_fs.h-shaped negative error otherwise. */
    int (*fs_format)(void);

    /* Flushing the display before a slow, blocking call (format) -- kernel owns the
     * console/display, same as the filesystem. */
    void (*console_flush)(void);
} purr_kernel_table_t;

typedef struct {
    uint32_t abi_version;      /* must equal PURR_KERNEL_TABLE_ABI_VERSION */
    const purr_cmd_t *cmds;
    uint32_t cmd_count;
} purr_kernel_module_table_t;

typedef const purr_kernel_module_table_t *(*purr_kernel_module_entry_fn)(const purr_kernel_table_t *kernel);

#ifdef __cplusplus
}
#endif

#endif /* PURR_KERNEL_TABLE_H */
