/*
 * purr_module_abi.h - the call table between the core and a real module (Modules/SPEC.md
 * section 6, first real instance).
 *
 * Included by BOTH sides: the core (which has the rest of coreos/kernel available) and a
 * module's own standalone compile (purrstrap `modules build`, which gets this header plus
 * whatever coreos/kernel headers it needs for TYPES ONLY -- purr_appmgr.h's registry
 * structs, say -- never for linkage. No module calls a real function by name; every core
 * service comes through purr_core_table_t below, a pointer to which is the only argument
 * its entry point receives.
 *
 * Its own exported command table is just an ordinary purr_cmd_t array living in its own
 * relocated memory: the `fn` pointers in it are relocated exactly like any other address in
 * the module (Modules/SPEC.md section 4), so the core can drop them straight into its
 * combined dispatch table with no translation needed. A command function only ever receives
 * (purr_cli_t *, argc, argv) -- purr_cmd_fn's fixed shape -- so a module that needs the core
 * table inside its own commands caches the pointer it was handed at entry() time in one of
 * its own statics, the same way any module-level state works.
 */
#ifndef PURR_MODULE_ABI_H
#define PURR_MODULE_ABI_H

#include <stdint.h>

#include "purr_appmgr.h"
#include "purr_cli.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped from 1: the table grew (printf, apps_scan). Appending fields is offset-compatible
 * with a module built against a smaller table (it only ever reads the fields it knows the
 * name of), but the version is bumped anyway so a stale module is refused rather than
 * silently running against a table shape its author never saw -- rebuild and replant it. */
#define PURR_MODULE_ABI_VERSION 2u

typedef struct {
    /* Writes to the shell the module was invoked from. A module cannot call purr_cli_puts
     * itself (no linkage to it), so this is the only way it can produce output. */
    void (*puts)(purr_cli_t *cli, const char *s);

    /* Formatted output, since AppManager's commands (and most real commands) build padded
     * columns, sizes, etc. A module cannot call snprintf/vsnprintf itself either (no libc
     * linkage) -- calling a variadic function through a pointer works exactly like calling
     * one directly, same ABI either way, so this is just purr_cli_printf reached indirectly. */
    void (*printf)(purr_cli_t *cli, const char *fmt, ...);

    /* Fills *out_reg by (re)scanning the installed-apps filesystem. Deliberately a
     * domain-level call, not raw purr_fs/purr_appmgr access: installing and removing apps is
     * security-sensitive and stays entirely inside the core (a module never gets a purr_fs_t
     * or a signing key bag), so a module can only ever see the read-only scan result. */
    void (*apps_scan)(purr_app_registry_t *out_reg);
} purr_core_table_t;

typedef struct {
    uint32_t abi_version;      /* must equal PURR_MODULE_ABI_VERSION; the core refuses a mismatch */
    const purr_cmd_t *cmds;    /* the module's own table, already relocated */
    uint32_t cmd_count;
} purr_module_table_t;

/* A module's payload entry point (Modules/SPEC.md section 5's entry_offset points at a
 * function with this signature). Called once at load time; the returned table is kept by
 * the core and merged into the shell's dispatch. */
typedef const purr_module_table_t *(*purr_module_entry_fn)(const purr_core_table_t *core);

#ifdef __cplusplus
}
#endif

#endif /* PURR_MODULE_ABI_H */
