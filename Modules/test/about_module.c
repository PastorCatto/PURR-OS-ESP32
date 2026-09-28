/* The first real module: a genuine shell command (not a spike diagnostic), loaded from
 * LittleFS at boot and merged into the real shell's dispatch table. See commands.c's
 * "Modules (real)" section for the loader, and purr_module_abi.h for the call table. */
#include "purr_module_abi.h"

static const purr_core_table_t *s_core;

static int cmd_about(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    s_core->puts(cli, "This command is running from a real module, loaded from LittleFS at "
                      "boot -- not linked into the firmware image.\n");
    return 0;
}

static const purr_cmd_t s_cmds[] = {
    {"about", "prints a message from a loaded module", cmd_about},
};

static const purr_module_table_t s_table = {
    .abi_version = PURR_MODULE_ABI_VERSION,
    .cmds = s_cmds,
    .cmd_count = 1,
};

__attribute__((used))
const purr_module_table_t *about_module_entry(const purr_core_table_t *core)
{
    s_core = core;
    return &s_table;
}
