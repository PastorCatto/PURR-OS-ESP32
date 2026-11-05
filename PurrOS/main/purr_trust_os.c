#include "purr_trust_os.h"

#include <string.h>

#include "purr_cfgstore.h"
#include "purr_crypto_mbedtls.h"
#include "purr_kernel.h"
#include "purr_keybag.h"

extern const purr_key_t purr_default_keys[];
extern const size_t purr_default_keys_count;

void os_trust(const purr_cfg_t *raw, purr_trust_t *t)
{
    purr_trust_eval(raw, purr_default_keys, purr_default_keys_count, &purr_crypto_mbedtls, t);
}

void os_cfg_view(const purr_cfg_t *raw, purr_cfg_t *view)
{
    purr_trust_t t;
    os_trust(raw, &t);
    purr_trust_view(raw, &t, view);
}

const char *purr_secure_level_name(purr_secure_level_t l)
{
    switch (l) {
    case PURR_SECURE_LEVEL_GENUINE:     return "GENUINE";
    case PURR_SECURE_LEVEL_DEVELOPMENT: return "DEVELOPMENT";
    default:                            return "UNVERIFIED";
    }
}

void purr_secure_status(purr_secure_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->key_id = 0xFF;
    out->mode_at_boot = PURR_SECURE_WARN;

    purr_flash_t fl;
    purr_cfg_t cfg;
    purr_trust_t now;
    if (purr_cfgstore_open(&fl) != 0 || purr_cfg_load(&fl, &cfg, NULL) < 0) {
        purr_cfg_defaults(&cfg);
    }
    os_trust(&cfg, &now);
    out->mode_now = now.mode;
    out->dev_now = now.dev;
    out->signed_now = now.signature_ok;

    purr_handoff_t h;
    if (purr_kernel_handoff(&h) == 0) {
        out->have_handoff = 1;
        out->mode_at_boot = PURR_SECST_MODE(h.secure_state) < 3 ? PURR_SECST_MODE(h.secure_state)
                                                                : PURR_SECURE_WARN;
        out->dev_at_boot = (h.secure_state & PURR_SECST_DEV) != 0;
        out->signed_at_boot = (h.secure_state & PURR_SECST_SIGNED) != 0;
        out->key_id = h.key_id_used;
        out->boot_state = h.boot_state;
        out->slot_verified = h.key_id_used != 0xFF && h.boot_state != PURR_BOOT_FAILED_WARNED;
    }

    if (out->dev_now || out->dev_at_boot) {
        out->level = PURR_SECURE_LEVEL_DEVELOPMENT;
    } else if (out->have_handoff && out->mode_at_boot == PURR_SECURE_ENFORCE && out->slot_verified) {
        out->level = PURR_SECURE_LEVEL_GENUINE;
    } else {
        out->level = PURR_SECURE_LEVEL_UNVERIFIED;
    }
}

const char *purr_role_label(uint8_t role)
{
    switch (role) {
    case PURR_ROLE_BOOT:      return "PURR OS boot key";
    case PURR_ROLE_SYSTEM:    return "System key";
    case PURR_ROLE_OWNER:     return "Owner key";
    case PURR_ROLE_DEVELOPER: return "Developer key";
    case PURR_ROLE_VENDOR:    return "Vendor key";
    default:                  return "Unknown key";
    }
}

void purr_secure_signer(uint8_t key_id, purr_signer_t *out)
{
    memset(out, 0, sizeof(*out));
    out->key_id = key_id;
    if (key_id == 0xFF) {
        return;
    }
    purr_flash_t fl;
    purr_cfg_t cfg, view;
    if (purr_cfgstore_open(&fl) != 0 || purr_cfg_load(&fl, &cfg, NULL) < 0) {
        purr_cfg_defaults(&cfg);
    }
    os_cfg_view(&cfg, &view);                                     /* signed overrides count, unsigned ones do not */
    static purr_keybag_t bag;                                     /* ~600 bytes: not on the caller's stack */
    purr_keybag_build(&bag, purr_default_keys, purr_default_keys_count, &view);
    const purr_key_t *k = purr_keybag_find(&bag, key_id);
    if (k != NULL) {
        out->known = 1;
        out->role = k->role;
        out->revoked = purr_keybag_is_revoked(&bag, key_id);
    }
}

int purr_secure_dev_locked(void)
{
    purr_flash_t fl;
    purr_cfg_t cfg;
    purr_trust_t t;
    if (purr_cfgstore_open(&fl) != 0 || purr_cfg_load(&fl, &cfg, NULL) < 0) {
        return 0;
    }
    os_trust(&cfg, &t);
    return t.signature_ok && (cfg.trust_flags & PURR_TRUSTF_DEV_LOCKED) != 0;
}

int purr_secure_set_dev(int on)
{
    purr_flash_t fl;
    purr_cfg_t cfg;
    purr_trust_t t;
    if (purr_cfgstore_open(&fl) != 0 || purr_cfg_load(&fl, &cfg, NULL) < 0) {
        return -1;
    }
    os_trust(&cfg, &t);
    if (on && t.signature_ok && (cfg.trust_flags & PURR_TRUSTF_DEV_LOCKED)) {
        return -2;
    }
    if (on) {
        cfg.flags |= PURR_CFGF_DEV_MODE | PURR_CFGF_DEV_RECOVERY;      /* recovery may use a dev server only while developer mode is on */
    } else {
        cfg.flags &= ~(PURR_CFGF_DEV_MODE | PURR_CFGF_DEV_RECOVERY);
    }
    return purr_cfg_store(&fl, &cfg) == 0 ? 0 : -1;
}

int purr_secure_request_reinstall(int force)
{
    purr_flash_t fl;
    purr_cfg_t cfg;
    if (purr_cfgstore_open(&fl) != 0 || purr_cfg_load(&fl, &cfg, NULL) < 0) {
        return -1;
    }
    cfg.flags |= PURR_CFGF_FORCE_LOADER | PURR_CFGF_AUTO_REINSTALL;
    if (force) {
        cfg.flags |= PURR_CFGF_BAD_KERNEL | PURR_CFGF_BAD_KITTENOS;
    }
    return purr_cfg_store(&fl, &cfg) == 0 ? 0 : -1;
}

static int cfg_flag_get(uint32_t flag)
{
    purr_flash_t fl;
    purr_cfg_t cfg;
    if (purr_cfgstore_open(&fl) != 0 || purr_cfg_load(&fl, &cfg, NULL) < 0) {
        return 0;
    }
    return (cfg.flags & flag) != 0;
}

static int cfg_flag_put(uint32_t flag, int on)
{
    purr_flash_t fl;
    purr_cfg_t cfg;
    if (purr_cfgstore_open(&fl) != 0 || purr_cfg_load(&fl, &cfg, NULL) < 0) {
        return -1;
    }
    if (on) {
        cfg.flags |= flag;
    } else {
        cfg.flags &= ~flag;
    }
    return purr_cfg_store(&fl, &cfg) == 0 ? 0 : -1;
}

int purr_boot_fast_get(void) { return cfg_flag_get(PURR_CFGF_FAST_START); }

/* Fast startup is not allowed for now (owner, 2026-11-04): it hid the boot screens and went with start-up trouble. Turning it off always works; turning it on is refused. */
int purr_boot_fast_set(int on) { return on ? -1 : cfg_flag_put(PURR_CFGF_FAST_START, 0); }

/* Clears the flag if a device still has it set (it was turned on before this was disallowed, and a restore keeps it). Called early on every start, so the next
 * start shows the boot screens again. Writes only when the flag is set. */
void purr_boot_fast_clear(void)
{
    if (purr_boot_fast_get() == 1) {
        purr_boot_fast_set(0);
    }
}
int purr_boot_request_menu(void) { return cfg_flag_put(PURR_CFGF_SHOW_MENU, 1); }
