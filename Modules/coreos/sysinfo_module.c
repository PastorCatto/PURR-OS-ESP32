/*
 * sysinfo_module.c: `mem`, `uptime`, `version`, `info`, `parts`, `echo`, `clear`, `reboot`,
 * and `purrcfg` -- the fifth and last sweep of the built-in command table onto the prototype
 * kernel table (PurrOS/components/coreos/include/purr_kernel_table.h). Same freestanding
 * discipline (-nostdlib -ffreestanding, no libc/FreeRTOS linkage of its own, everything
 * through the table) as every module before it. These replace the old inline commands in
 * commands.c -- a real conversion, not a duplicate.
 */
#include "purr_kernel_table.h"

static const purr_kernel_table_t *s_k;

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

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

static int cmd_version(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    s_k->print_version(cli);
    return 0;
}

static int cmd_info(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    s_k->print_info(cli);
    return 0;
}

static int cmd_parts(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    s_k->print_parts(cli);
    return 0;
}

static int cmd_echo(purr_cli_t *cli, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        s_k->printf(cli, "%s%s", i > 1 ? " " : "", argv[i]);
    }
    s_k->puts(cli, "\n");
    return 0;
}

static int cmd_clear(purr_cli_t *cli, int argc, char **argv)
{
    (void)cli; (void)argc; (void)argv;
    s_k->console_clear();
    return 0;
}

static int cmd_reboot(purr_cli_t *cli, int argc, char **argv)
{
    const char *target = NULL;
    if (argc == 2 && streq(argv[1], "recovery")) {
        target = "recovery";
    } else if (argc == 2 && streq(argv[1], "loader")) {
        target = "loader";
    } else if (argc > 1) {
        s_k->puts(cli, "usage: reboot [recovery|loader]\n");
        return 1;
    }
    s_k->reboot_system(cli, target);
    return 0;   /* not reached on success -- reboot_system() never returns then */
}

static int cmd_purrcfg(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    s_k->print_purrcfg(cli);
    return 0;
}

static const purr_cmd_t s_cmds[] = {
    {"mem",     "memory usage",                  cmd_mem},
    {"uptime",  "time since power on",           cmd_uptime},
    {"version", "show the system and profile",   cmd_version},
    {"info",    "board, chip and display",       cmd_info},
    {"parts",   "the partition table",           cmd_parts},
    {"echo",    "print the arguments",           cmd_echo},
    {"clear",   "clear the screen",              cmd_clear},
    {"reboot",  "restart (or recovery|loader)",  cmd_reboot},
    {"purrcfg", "show the boot config",          cmd_purrcfg},
};

static const purr_kernel_module_table_t s_table = {
    .abi_version = PURR_KERNEL_TABLE_ABI_VERSION,
    .cmds = s_cmds,
    .cmd_count = 9,
};

__attribute__((used))
const purr_kernel_module_table_t *sysinfo_module_entry(const purr_kernel_table_t *kernel)
{
    s_k = kernel;
    return &s_table;
}
