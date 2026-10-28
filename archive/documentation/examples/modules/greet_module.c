/* greet_module.c - a minimal /system module: adds a `greet` command. */
#include "purr_module_abi.h"

static const purr_core_table_t *s_core;

static int cmd_greet(purr_cli_t *cli, int argc, char **argv)
{
    s_core->printf(cli, "hello, %s!\n", argc > 1 ? argv[1] : "world");
    return 0;
}

static const purr_cmd_t s_cmds[] = {
    {"greet", "say hello", cmd_greet},
};

static const purr_module_table_t s_table = {
    .abi_version = PURR_MODULE_ABI_VERSION,
    .cmds = s_cmds,
    .cmd_count = 1,
};

__attribute__((used))
const purr_module_table_t *greet_module_entry(const purr_core_table_t *core)
{
    s_core = core;
    return &s_table;
}
