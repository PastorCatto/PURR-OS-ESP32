/*
 * purr_trust_os.h - the OS's view of the device's security state (purr_trust.h, plan3.md).
 *
 * purrcfg holds trust state the OS must not take at face value: secure_mode and the override
 * keys count only if the trust block's signature verifies, and development mode and `tighten`
 * change the effective mode. Anything that decides something from purrcfg -- whether an unsigned
 * install is acceptable, which keys may sign, whether to enforce a version floor -- reads the
 * VIEW from os_cfg_view(), not the raw config. The view cannot be stored back: purr_cfg_store()
 * refuses it. Keep the raw config for storing.
 */
#ifndef PURR_TRUST_OS_H
#define PURR_TRUST_OS_H

#include "purr_cfg.h"
#include "purr_trust.h"

/* Evaluate raw against the compiled-in default keys. */
void os_trust(const purr_cfg_t *raw, purr_trust_t *t);

/* A decisions-only copy with the effective values in place. */
void os_cfg_view(const purr_cfg_t *raw, purr_cfg_t *view);

/*
 * The device's security state as an app, a service or the shell should see it (plan3.md "Secure
 * status"). Honest about what it is: SOFTWARE attestation. Without the hardware root of trust
 * (eFuse Secure Boot) someone who can write flash can still make a device report GENUINE.
 *
 *   GENUINE      the bootloader enforced `enforce` this boot, the slot it started verified, and
 *                development mode is neither active now nor was it at boot
 *   DEVELOPMENT  development mode is active (now, or it was at boot)
 *   UNVERIFIED   anything else: not enforcing, the slot did not verify, or no handoff at all (a
 *                stock bootloader or a cold boot that lost it)
 *
 * mode_now / dev_now / signed_now come from purrcfg (what the NEXT boot will use); the *_at_boot
 * values are what the bootloader actually enforced for THIS boot (the handoff).
 */
typedef enum {
    PURR_SECURE_LEVEL_UNVERIFIED = 0,
    PURR_SECURE_LEVEL_DEVELOPMENT = 1,
    PURR_SECURE_LEVEL_GENUINE = 2,
} purr_secure_level_t;

typedef struct {
    purr_secure_level_t level;
    int     have_handoff;
    uint8_t mode_now, mode_at_boot;        /* PURR_SECURE_*; mode_at_boot is meaningless without a handoff */
    int     dev_now, dev_at_boot;
    int     signed_now, signed_at_boot;
    int     slot_verified;                 /* the running slot's signature verified at boot */
    uint8_t key_id;                        /* the key that verified it, or 0xFF */
    uint8_t boot_state;                    /* PURR_BOOT_* */
} purr_secure_status_t;

void purr_secure_status(purr_secure_status_t *out);
const char *purr_secure_level_name(purr_secure_level_t l);

/* 1 if this device's SIGNED trust block locks development mode out (a production device). */
int purr_secure_dev_locked(void);

/* Development mode on or off for the next boot. This is the one way the bootloader's enforce is weakened without a signed
 * trust block, and only the recovery app (recovery_app.c) calls it with on = 1: it makes the user choose about their data
 * first. Returns 0, -1 (purrcfg unreadable or unwritable) or -2 (the signed trust block forbids it). Turning it off always works. */
int purr_secure_set_dev(int on);

/* Fast startup (bootloader/SPEC.md section 9): on a healthy start the bootloader loads no boot package and draws nothing. The checks are not skipped.
 * get: 1 if it is on. set: 0 or -1 (the boot configuration could not be written). */
int purr_boot_fast_get(void);
int purr_boot_fast_set(int on);
/* The next start shows the boot menu once (the bootloader clears the request when it reads it). 0 or -1. */
int purr_boot_request_menu(void);

/* Asks the bootloader for the recovery loader on the next start, to reinstall on its own with the saved network (the data is kept).
 * force = 1 also marks the kernel and KittenOS slots BAD, so the loader rewrites them even if they still verify; 0 leaves them, and the
 * loader writes only what the server has newer. 0 or -1. */
int purr_secure_request_reinstall(int force);

#endif
