/*
 * apps_module.c: `apps` (list installed apps) -- the first real command ported out of the
 * monolithic shell and into an actual loaded module. See Modules/SPEC.md for the mechanism,
 * and AppManager's storage core (PurrOS/components/coreos/include/purr_appmgr.h) for what
 * this wraps. appinfo/appinstall/appremove/appformat stay in the monolith for now; this is
 * the first slice, not the whole port.
 */
#include "purr_module_abi.h"

static const purr_core_table_t *s_core;

static int cmd_apps(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    purr_app_registry_t reg;
    s_core->apps_scan(&reg);

    if (reg.count == 0) {
        s_core->puts(cli, "no apps installed\n");
    }
    for (int i = 0; i < reg.count; i++) {
        const purr_app_entry_t *e = &reg.apps[i];
        s_core->printf(cli, "  %-16s %-10s %6uK  %s%s\n", e->name, e->version,
                       (unsigned)(e->size / 1024),
                       e->verified ? "verified" : "unverified",
                       e->chip_ok ? "" : "  (wrong chip)");
    }
    if (reg.dropped) {
        s_core->printf(cli, "  (%d app folder(s) skipped: unreadable or not a valid package)\n",
                       reg.dropped);
    }
    return 0;
}

static const purr_cmd_t s_cmds[] = {
    {"apps", "list installed apps", cmd_apps},
};

static const purr_module_table_t s_table = {
    .abi_version = PURR_MODULE_ABI_VERSION,
    .cmds = s_cmds,
    .cmd_count = 1,
};

__attribute__((used))
const purr_module_table_t *apps_module_entry(const purr_core_table_t *core)
{
    s_core = core;
    return &s_table;
}
