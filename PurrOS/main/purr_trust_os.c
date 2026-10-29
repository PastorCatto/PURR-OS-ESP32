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
