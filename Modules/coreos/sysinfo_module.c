/*
 * sysinfo_module.c: `mem` and `uptime` -- the first real CoreOS commands ported onto the
 * prototype kernel table (PurrOS/components/coreos/include/purr_kernel_table.h), one layer
 * below purr_module_abi.h's CoreOS-to-module table every other module so far has used. Same
 * freestanding discipline (-nostdlib -ffreestanding, no libc/FreeRTOS linkage of its own,
 * everything through the table); the difference is which boundary it proves. These replace
 * the old inline cmd_mem/cmd_uptime in commands.c -- a real conversion, not a duplicate.
 */
#include "purr_kernel_table.h"

static const purr_kernel_table_t *s_k;

static int cmd_mem(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    s_k->printf(cli, "internal: %u free, %u largest block\n",
               (unsigned)s_k->heap_free_internal(), (unsigned)s_k->heap_largest_free_internal());
    uint32_t psram = s_k->heap_total_psram();
    if (psram) {
        s_k->printf(cli, "psram:    %u free of %u\n", (unsigned)s_k->heap_free_psram(), (unsigned)psram);
    }
    return 0;
}

static int cmd_uptime(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    unsigned s = (unsigned)(s_k->uptime_us() / 1000000);
    s_k->printf(cli, "up %u:%02u:%02u\n", s / 3600, (s / 60) % 60, s % 60);
    return 0;
}

static const purr_cmd_t s_cmds[] = {
    {"mem",    "memory usage",           cmd_mem},
    {"uptime", "time since power on",    cmd_uptime},
};

static const purr_kernel_module_table_t s_table = {
    .abi_version = PURR_KERNEL_TABLE_ABI_VERSION,
    .cmds = s_cmds,
    .cmd_count = 2,
};

__attribute__((used))
const purr_kernel_module_table_t *sysinfo_module_entry(const purr_kernel_table_t *kernel)
{
    s_k = kernel;
    return &s_table;
}
