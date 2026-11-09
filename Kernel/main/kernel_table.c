/*
 * kernel_table.c - this kernel's real implementation of purr_kernel_table_t
 * (PurrOS/components/coreos/include/purr_kernel_table.h), the kernel<->CoreOS boundary.
 *
 * Only backs the fields this minimal kernel genuinely supports today: console output, the
 * root filesystem (read-only surface -- list/read/usage/mounted/strerror; write/mkdir/
 * remove/rename/format are not wired up yet, nothing needs them from here), heap and uptime
 * queries. Login, app management, reboot and the rest of the table are real CoreOS content
 * that hasn't moved into this kernel's domain yet -- left zero-initialized (NULL/0) rather
 * than faked, since nothing calls them yet either. Fill them in as the real content that
 * needs them actually gets ported, not ahead of it.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "purr_console.h"
#include "purr_fs.h"
#include "purr_kernel_table.h"

static purr_fs_t *s_fs;

void kernel_table_set_fs(purr_fs_t *fs)
{
    s_fs = fs;
}

static void kprint(const char *s)
{
    while (*s) {
        purr_console_put(NULL, *s++);
    }
}

static void kernel_table_puts(purr_cli_t *cli, const char *s)
{
    (void)cli;
    kprint(s);
    purr_console_flush();
}

static void kernel_table_printf(purr_cli_t *cli, const char *fmt, ...)
{
    (void)cli;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    kprint(buf);
    purr_console_flush();
}

static uint32_t kernel_table_heap_free_internal(void)
{
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}
static uint32_t kernel_table_heap_largest_free_internal(void)
{
    return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
}
static uint32_t kernel_table_heap_free_psram(void)
{
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}
static uint32_t kernel_table_heap_total_psram(void)
{
    return (uint32_t)heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
}
static void *kernel_table_heap_alloc(size_t n) { return malloc(n); }
static void kernel_table_heap_free(void *p) { free(p); }
static uint64_t kernel_table_uptime_us(void) { return (uint64_t)esp_timer_get_time(); }

static int kernel_table_fs_list(const char *path, purr_fs_list_fn cb, void *ctx)
{
    return purr_fs_list(s_fs, path, cb, ctx);
}
static int kernel_table_fs_read(const char *path, purr_fs_read_fn cb, void *ctx)
{
    return purr_fs_read(s_fs, path, cb, ctx);
}
static int kernel_table_fs_write(const char *path, const void *data, uint32_t len)
{
    return purr_fs_write(s_fs, path, data, len);
}
static int kernel_table_fs_mkdir(const char *path) { return purr_fs_mkdir(s_fs, path); }
static int kernel_table_fs_remove(const char *path) { return purr_fs_remove(s_fs, path); }
static int kernel_table_fs_rename(const char *from, const char *to)
{
    return purr_fs_rename(s_fs, from, to);
}
static int kernel_table_fs_usage(uint32_t *used, uint32_t *total)
{
    return purr_fs_usage(s_fs, used, total);
}
static int kernel_table_fs_mounted(void) { return purr_fs_mounted(s_fs); }
static const char *kernel_table_fs_strerror(int err) { return purr_fs_strerror(err); }
static void kernel_table_console_flush(void) { purr_console_flush(); }
static void kernel_table_console_clear(void) { purr_console_clear(); }

static const purr_kernel_table_t s_kernel_table = {
    .puts = kernel_table_puts,
    .printf = kernel_table_printf,
    .heap_alloc = kernel_table_heap_alloc,
    .heap_free = kernel_table_heap_free,
    .heap_free_internal = kernel_table_heap_free_internal,
    .heap_largest_free_internal = kernel_table_heap_largest_free_internal,
    .heap_free_psram = kernel_table_heap_free_psram,
    .heap_total_psram = kernel_table_heap_total_psram,
    .uptime_us = kernel_table_uptime_us,
    .fs_list = kernel_table_fs_list,
    .fs_read = kernel_table_fs_read,
    .fs_write = kernel_table_fs_write,
    .fs_mkdir = kernel_table_fs_mkdir,
    .fs_remove = kernel_table_fs_remove,
    .fs_rename = kernel_table_fs_rename,
    .fs_usage = kernel_table_fs_usage,
    .fs_mounted = kernel_table_fs_mounted,
    .fs_strerror = kernel_table_fs_strerror,
    .console_flush = kernel_table_console_flush,
    .console_clear = kernel_table_console_clear,
    /* fs_format, login_*, app_*, print_*, reboot_system, read_key: not yet real at this
     * layer -- see the file header comment. */
};

const purr_kernel_table_t *purr_kernel_table(void)
{
    return &s_kernel_table;
}
