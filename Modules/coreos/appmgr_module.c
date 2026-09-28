/*
 * appmgr_module.c: `appinfo`, `appinstall`, `appremove`, `appformat` -- the app-management
 * cluster, the fourth real slice ported onto the prototype kernel table (purr_kernel_table.h),
 * after mem/uptime, the filesystem sweep, and the accounts sweep. Like login_*, net_install
 * and fs_format, each command is one opaque, cli-aware call: these touch the signing key bag
 * (installing/removing an app verifies against it) and the raw per-user apps filesystem,
 * neither of which a module is ever handed. This module is just argument parsing and dispatch.
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

static int cmd_appinfo(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) {
        s_k->puts(cli, "usage: appinfo <name>\n");
        return 1;
    }
    return s_k->app_info(cli, argv[1]);
}

static int cmd_appinstall(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) {
        s_k->puts(cli, "usage: appinstall <file on root, or a URL>\n");
        return 1;
    }
    return s_k->app_install(cli, argv[1]);
}

static int cmd_appremove(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) {
        s_k->puts(cli, "usage: appremove <name>\n");
        return 1;
    }
    return s_k->app_remove(cli, argv[1]);
}

static int cmd_appformat(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2 || !streq(argv[1], "--yes")) {
        s_k->puts(cli, "this erases every app installed for you (not other users).\n"
                      "run: appformat --yes\n");
        return 1;
    }
    return s_k->app_format(cli);
}

static const purr_cmd_t s_cmds[] = {
    {"appinfo",    "show one app's details",        cmd_appinfo},
    {"appinstall", "install a .cat package",        cmd_appinstall},
    {"appremove",  "remove an app",                 cmd_appremove},
    {"appformat",  "erase and create the apps fs",  cmd_appformat},
};

static const purr_kernel_module_table_t s_table = {
    .abi_version = PURR_KERNEL_TABLE_ABI_VERSION,
    .cmds = s_cmds,
    .cmd_count = 4,
};

__attribute__((used))
const purr_kernel_module_table_t *appmgr_module_entry(const purr_kernel_table_t *kernel)
{
    s_k = kernel;
    return &s_table;
}
