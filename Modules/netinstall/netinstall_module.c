/*
 * netinstall_module.c: `netinstall [component]` -- fetch, verify, and flash a signed PURR OS
 * image from the network into ota_0. See Modules/SPEC.md for the mechanism, and
 * PurrOS/components/coreos/include/purr_module_abi.h's net_install comment for why this one
 * stays a single opaque core call instead of decomposed primitives like apps_scan/net_*: the
 * real operation touches the signing key bag and raw partition/OTA APIs, exactly what a
 * module is never handed. All of it -- fetch, verify, write, progress messages -- runs inside
 * the core; this module is a one-line dispatcher onto core->net_install.
 */
#include "purr_module_abi.h"

static const purr_core_table_t *s_core;

static int cmd_netinstall(purr_cli_t *cli, int argc, char **argv)
{
    const char *component = argc > 1 ? argv[1] : "purros";
    return s_core->net_install(cli, component);
}

static const purr_cmd_t s_cmds[] = {
    {"netinstall", "install [component] over the network", cmd_netinstall},
};

static const purr_module_table_t s_table = {
    .abi_version = PURR_MODULE_ABI_VERSION,
    .cmds = s_cmds,
    .cmd_count = 1,
};

__attribute__((used))
const purr_module_table_t *netinstall_module_entry(const purr_core_table_t *core)
{
    s_core = core;
    return &s_table;
}
