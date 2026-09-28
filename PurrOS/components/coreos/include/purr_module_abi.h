/*
 * purr_module_abi.h - the call table between the core and a real module (Modules/SPEC.md
 * section 6, first real instance).
 *
 * Included by BOTH sides: the core (which has the rest of coreos/kernel available) and a
 * module's own standalone compile (purrstrap `modules build`, which only ever gets this
 * header and purr_cli.h -- no ESP-IDF, no filesystem, nothing else). Kept dependency-free
 * for exactly that reason, same as purr_cli.h itself.
 *
 * A module can't call any of the core's real functions directly (it's a separately
 * compiled, separately loaded blob with no linkage to them) -- every core service it needs
 * comes through purr_core_table_t, a pointer to which is the only argument its entry point
 * receives. Its own exported command table is just an ordinary purr_cmd_t array living in
 * its own relocated memory: the `fn` pointers in it are relocated exactly like any other
 * address in the module (Modules/SPEC.md section 4), so the core can drop them straight
 * into its combined dispatch table with no translation needed.
 */
#ifndef PURR_MODULE_ABI_H
#define PURR_MODULE_ABI_H

#include <stdint.h>

#include "purr_cli.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_MODULE_ABI_VERSION 1u

typedef struct {
    /* Writes to the shell the module was invoked from. A module cannot call purr_cli_puts
     * itself (no linkage to it), so this is the only way it can produce output. */
    void (*puts)(purr_cli_t *cli, const char *s);
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
