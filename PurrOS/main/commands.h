/* The shell's commands, shared by the KittenOS (recovery) and full profiles. */
#ifndef PURR_COMMANDS_H
#define PURR_COMMANDS_H

#include "purr_cli.h"
#include "purr_fs.h"
#include "purr_kernel_table.h"

/* The shared, already-mounted root filesystem, for login.c's account files. */
purr_fs_t *purr_login_fs(void);

/* The table of commands for this build's profile. */
const purr_cmd_t *purr_commands(int *count);

/* What this build calls itself, e.g. "KittenOS" or "PURR OS". */
const char *purr_system_name(void);

/* Mount the root filesystem and say how it went. */
void purr_fs_setup(purr_cli_t *cli);

/* Mount the apps filesystem and say how it went. */
void purr_apps_setup(purr_cli_t *cli);

/* F-10: assigns every /home/<name> that has no owner of its own yet to that account, by
 * name -- the one-time migration for a device upgrading to file ownership (Users/SPEC.md
 * 2.1). Call once the account list is actually loaded (main.c's run_login(), right after
 * login_run()/login_skip() -- NOT purr_fs_setup(), which runs before accounts are read from
 * disk and would see an empty list). Silent and idempotent; safe to call every login. */
void purr_fs_permissions_migrate(void);

/* Sweep /system, verify every file there, and load the ones that pass (Modules/SPEC.md
 * section 7.1). Anything that fails verification or carries an incompatible ABI is quarantined
 * (moved to /system/.rejected, never retried) and reported on `cli`. Must run before
 * purr_commands() is first called -- it used to load modules lazily on that first call;
 * now it expects the sweep already done. */
void purr_modules_setup(purr_cli_t *cli);

/* Start the Wi-Fi service, sharing the mounted root filesystem for saved networks. */
void purr_net_setup(purr_cli_t *cli);

/* Stage 2 of recovery (PURR_CFGF_CONTINUE_INSTALL, purr_abi.h): reconnects using the same
 * recovery network record the loader just used, then installs `kernel` and the module index
 * over it. Call once, right after a fresh network install's first boot. Best-effort, never
 * fatal to booting. */
void purr_continue_install(purr_cli_t *cli);

/* Resets purrcfg's boot_fail_count to 0 once a boot has reached a stable, working state (right
 * before the login loop starts) -- the bootloader's failure-count ladder
 * (bootloader/SPEC.md section 6) only climbs when this is never reached. Silent no-op if the
 * count is already 0. */
void purr_mark_boot_healthy(purr_cli_t *cli);

/* KittenOS's side of a pending system-file update (PurrOS/SPEC.md section 6.1): swaps a staged
 * file into place, retries, or rolls back, as purr_swap_decide() says. Reboots and never
 * returns if it did anything; returns normally if there was nothing pending. Only meaningful
 * to call from the recovery profile -- "KittenOS performs the swap." */
void purr_swap_setup(purr_cli_t *cli);

/* The kernel<->CoreOS call table (purr_kernel_table.h), for main.c's own boot orchestration
 * (run_login()/run_shell()) to call through instead of touching purr_kernel_key()/
 * purr_login_*() directly -- written this way now so the same code needs no rewriting once it
 * actually moves into a separately loaded CoreOS (PurrOS/SPEC.md section 6). Kernelmods get
 * this same table as an argument at load time; main.c fetches it through this accessor since
 * it isn't loaded, it's what's already running. */
const purr_kernel_table_t *purr_kernel_table(void);

#endif
