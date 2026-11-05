/* The shell's commands, shared by the KittenOS (recovery) and full profiles. */
#ifndef PURR_COMMANDS_H
#define PURR_COMMANDS_H

#include "purr_appmgr.h"
#include "purr_cli.h"
#include "purr_fs.h"
#include "purr_repo.h"
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

/* Install (from a file on the device or an http(s) address) and remove, for the signed-in user, without
 * a prompt: what the Settings app calls. 0 on success; msg says what happened. Worker task only, one at
 * a time (they share the app scratch buffer). */
int purr_apps_install(const char *file_or_url, char *msg, size_t cap);
int purr_apps_remove(const char *name);

/* Security settings without a prompt, for the Settings app and the `settings` command. Each needs an
 * administrator, returns 0 on success, and says what happened (or why not) in msg. The first two apply
 * from the next boot, like the `secure` command; the confirmation is the caller's job. */
int purr_security_set_enforce(int on, char *msg, size_t cap);   /* refuses if a boot slot would not verify */
int purr_security_set_dev(int on, char *msg, size_t cap);       /* refused when a signed trust block locks it out */
int purr_security_set_unsigned_apps(int on, char *msg, size_t cap);

/* The developer folder, /home/<name>/devapps: what the Developer Portal installs. Kept apart from the apps
 * folder Settings manages. Everything here needs developer mode active this boot. */
void purr_devapps_scan(purr_app_registry_t *out_reg);
int purr_devapps_remove(const char *name);
/* Downloads `url` (a package on an SDK server on the local network; no redirect is followed), checks it
 * against the repository's index entry, and installs it. Worker task only. 0 on success; msg says what
 * happened. */
int purr_devapps_install_url(const char *url, const purr_repo_app_t *app, char *msg, size_t cap);

/* Developer mode was on for THIS boot (the bootloader's handoff says so; a file cannot fake it). */
int purr_security_dev_active(void);

/* Loads one driver file (verified like any driver: unsigned only in developer mode) and registers it. 0 or -1. */
int purr_driver_load_file(const char *path);
/* Unsigned apps may be installed and run: developer mode is active this boot AND the owner switched
 * "allow unsigned apps" on. A production-signed trust block that locks development mode out makes the
 * first part impossible, so this is then always 0. */
int purr_security_unsigned_apps_allowed(void);

/* The app runtime (purr_catrt.h, plan6.md): starts the manager, reads /etc/apps.conf, opens its
 * exec-once apps. Call after purr_apps_setup. */
void purr_catrt_setup(purr_cli_t *cli);

/* Called after any successful sign-in (main.c): fixes home-folder ownership and starts the apps
 * that were waiting for a user. */
void purr_session_started(void);

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
/* For the recovery loader (no filesystem): load the baked-in display, keyboard and touch driver modules from memory. */
int purr_loader_load_drivers(void);

/* Start the Wi-Fi service, sharing the mounted root filesystem for saved networks. */
void purr_net_setup(purr_cli_t *cli);

/* Stage 2 of recovery (PURR_CFGF_CONTINUE_INSTALL, purr_abi.h): reconnects using the same
 * recovery network record the loader just used, then installs `kernel` and the module index
 * over it. Call once, right after a fresh network install's first boot. Best-effort, never
 * fatal to booting. */
void purr_continue_install(purr_cli_t *cli);
/* Stage 2 in steps, for the setup wizard (oobe_firstboot.c). */
int purr_kernel_installed(void);                    /* the kernel slot holds an image and is not marked bad */
int purr_stage2_pending(void);                      /* the same, without clearing it */
int purr_stage2_take_pending(void);                 /* 1 if a recovery install is waiting to continue (clears the request) */
int purr_stage2_step(purr_cli_t *cli, int step);    /* 0 = the kernel, 1 = the modules, 2 = the bootloader, 3 = the shell's picture pack; returns 0 when it installed (or had nothing to install) */

/* Resets purrcfg's boot_fail_count to 0 once a boot has reached a stable, working state (right
 * before the login loop starts) -- the bootloader's failure-count ladder
 * (bootloader/SPEC.md section 6) only climbs when this is never reached. Silent no-op if the
 * count is already 0. */
void purr_mark_boot_healthy(purr_cli_t *cli);

/* Bootloader updates over the air are turned off (2026-11-05). The swap erases flash 0x0, which ESP-IDF aborts in every build
 * (CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS), so a staged bootloader sent KittenOS into a boot loop with the screen never coming up. A
 * bootloader reaches a device by a flash (tools/hw/make_hw_image.sh) until the swap is proven on a bench device. Set to 1 to bring it back. */
#define PURR_BOOTLOADER_OTA 0

/* KittenOS's side of a pending system-file update (PurrOS/SPEC.md section 6.1): swaps a staged
 * file into place, retries, or rolls back, as purr_swap_decide() says. Reboots and never
 * returns if it did anything; returns normally if there was nothing pending. Only meaningful
 * to call from the recovery profile -- "KittenOS performs the swap." */
void purr_swap_setup(purr_cli_t *cli);

/* The current user's installed-apps registry, freshly rescanned -- what
 * Modules/appmanager/apps_module.c's `apps` command reaches through the core table
 * (core_table_apps_scan(), commands.c), exposed here directly for code that's compiled
 * straight into this binary rather than loaded as a module (purr_ui_config.c's app_grid
 * widget, chunk F of memory/[[project_appruntime_fullstack_plan]]). Empty if no filesystem
 * is mounted. */
void purr_apps_scan_current_user(purr_app_registry_t *out_reg);

/* The key bag this device checks signatures against (the built-in keys plus the trust block's). */
const purr_keybag_t *purr_os_keybag(void);

/* Installs an app a trusted repository vouches for (Repositories/SPEC.md section 5): the package is already downloaded and checked against the repository's
 * signed index. It goes into the signed-in user's apps folder with a receipt (repo, index copy, package hash) that the loader checks. Worker task only.
 * 0 on success; msg says what happened. */
int purr_apps_install_vouched(const uint8_t *data, size_t len, const char *name, const char *repo_id, const char *idx8, const char *sha_hex, char *msg, size_t cap);

/* The kernel<->CoreOS call table (purr_kernel_table.h), for main.c's own boot orchestration
 * (run_login()/run_shell()) to call through instead of touching purr_kernel_key()/
 * purr_login_*() directly -- written this way now so the same code needs no rewriting once it
 * actually moves into a separately loaded CoreOS (PurrOS/SPEC.md section 6). Kernelmods get
 * this same table as an argument at load time; main.c fetches it through this accessor since
 * it isn't loaded, it's what's already running. */
const purr_kernel_table_t *purr_kernel_table(void);

#endif
