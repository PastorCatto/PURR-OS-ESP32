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

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_KERNEL_TABLE_ABI_VERSION 1u

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
