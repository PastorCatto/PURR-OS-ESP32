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

#include <stddef.h>
#include <stdint.h>

#include "purr_appmgr.h"
#include "purr_cli.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped from 4: net_forget's signature changed (documentation/FINDINGS.md F-10 -- it needs
 * `cli` and a result now, to refuse a standard user and say why). This one is NOT offset-
 * compatible like most additions here: an old module calling through the old void(ssid)
 * shape against this table would read garbage as its two arguments, so the version bump is
 * load-bearing this time, not just defensive -- rebuild and replant the wifi module. */
#define PURR_MODULE_ABI_VERSION 5u

/* purr_net.h's own purr_net_ap_t/purr_net_status_t aren't used here on purpose: that header
 * pulls in esp_err_t, whose own header chain ends at sdkconfig.h -- a real ESP-IDF project's
 * generated config, which a module's standalone compile doesn't have. Same shapes, so the
 * core's wrappers just assign field by field; no conversion logic needed. */
#define PURR_MODULE_NET_SSID_LEN 33

typedef struct {
    char ssid[PURR_MODULE_NET_SSID_LEN];
    int rssi;
    uint8_t open;                  /* no password needed */
} purr_module_net_ap_t;

typedef struct {
    int connected;
    char ssid[PURR_MODULE_NET_SSID_LEN];
    int rssi;
    char ip[16];
} purr_module_net_status_t;

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

    /* Wi-Fi, all domain-level: a module never sees esp_err_t, a wifi_err_reason_t, or
     * anything else ESP-IDF-specific -- net_scan/net_connect already turn a failure into
     * words a person reads (into err/err_cap), the same text the old inline shell commands
     * built, just moved behind the table instead of calling purr_net_* directly. */
    int (*net_scan)(purr_module_net_ap_t *out, int max, int *out_n, char *err, size_t err_cap);
    int (*net_connect)(const char *ssid, const char *pass, char *err, size_t err_cap);

    /* F-10: unlike the rest of this cluster, forgetting a saved network is destructive
     * enough (losing the only saved credentials can mean losing the only way back onto the
     * network) to need the same admin check as format/netinstall/reboot recovery|loader,
     * not just a plain Wi-Fi operation. Same convention as net_install: 0 on success,
     * nonzero on failure, the core has already told the user why on `cli`. */
    int (*net_forget)(purr_cli_t *cli, const char *ssid);
    int (*net_saved)(purr_module_net_ap_t *out, int max);
    void (*net_status)(purr_module_net_status_t *out);

    /* Deliberately NOT decomposed into primitives, unlike apps_scan/net_*: the operation this
     * wraps (fetch the recovery manifest, download and verify a signed image against the key
     * bag, erase and write ota_0, set the boot target) touches exactly what a module is never
     * handed -- purr_fs_t, the signing key bag, esp_err_t/esp_partition_t. Breaking it into
     * smaller calls would mean exposing that machinery piece by piece through the ABI for no
     * real benefit, since no module has a reason to build a different install flow. Progress
     * is printed straight to `cli` from inside the core (it has real purr_cli_* linkage, no
     * need to route through puts/printf above), same messages the old inline cmd_net_install
     * printed before this moved into a module. Returns 0 on success, nonzero on failure --
     * the core has already told the user why on `cli`. */
    int (*net_install)(purr_cli_t *cli, const char *component);
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
