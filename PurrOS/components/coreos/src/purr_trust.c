#include "purr_trust.h"

#include <string.h>

#include "purr_util.h"

size_t purr_trust_signed_bytes(const purr_cfg_t *cfg, uint8_t *out)
{
    size_t n = 0;

    memcpy(out + n, "PURRTRUST", 9);
    n += 9;
    out[n++] = 0;
    out[n++] = cfg->secure_mode;
    out[n++] = cfg->trust_flags;
    for (int i = 0; i < 4; i++) {
        out[n++] = (uint8_t)(cfg->revoked_keys >> (8 * i));
    }
    memcpy(out + n, cfg->keys, sizeof(cfg->keys));
    n += sizeof(cfg->keys);
    return n;
}

int purr_trust_has_signature(const purr_cfg_t *cfg)
{
    uint8_t all_zero = 0, all_ff = 0xFF;
    for (size_t i = 0; i < sizeof(cfg->trust_sig); i++) {
        all_zero |= cfg->trust_sig[i];
        all_ff &= cfg->trust_sig[i];
    }
    return all_zero != 0 && all_ff != 0xFF;
}

static uint8_t clamp_mode(uint8_t m)
{
    return m > PURR_SECURE_ENFORCE ? PURR_SECURE_WARN : m;     /* unknown is never "off" */
}

void purr_trust_eval(const purr_cfg_t *cfg, const purr_key_t *defaults, size_t ndefaults,
                     const purr_crypto_t *crypto, purr_trust_t *out)
{
    memset(out, 0, sizeof(*out));
    out->mode = PURR_SECURE_WARN;
    if (cfg == NULL) {
        return;
    }

    int sig_ok = 0;
    if (purr_trust_has_signature(cfg)) {
        uint8_t bytes[PURR_TRUST_SIGNED_MAX], digest[PURR_SHA256_LEN];
        size_t n = purr_trust_signed_bytes(cfg, bytes);
        purr_sha256(bytes, n, digest);
        for (size_t i = 0; i < ndefaults && !sig_ok; i++) {
            if (defaults[i].role == PURR_ROLE_BOOT &&
                crypto->verify_p256(defaults[i].pub, digest, cfg->trust_sig)) {
                sig_ok = 1;
            }
        }
    }
    out->signature_ok = sig_ok;

    uint8_t mode = sig_ok ? clamp_mode(cfg->secure_mode) : PURR_SECURE_WARN;
    uint8_t tighten = clamp_mode(cfg->tighten);
    if (tighten > mode) {
        mode = tighten;
    }
    int locked = sig_ok && (cfg->trust_flags & PURR_TRUSTF_DEV_LOCKED);
    out->dev = (cfg->flags & PURR_CFGF_DEV_MODE) && !locked;
    if (out->dev && mode > PURR_SECURE_WARN) {
        mode = PURR_SECURE_WARN;
    }
    out->mode = mode;
}

void purr_trust_view(const purr_cfg_t *cfg, const purr_trust_t *t, purr_cfg_t *view)
{
    *view = *cfg;
    view->magic = PURR_CFG_VIEW_MAGIC;
    view->secure_mode = t->mode;
    if (!t->signature_ok) {
        memset(view->keys, 0, sizeof(view->keys));      /* overrides do not count unsigned */
        view->revoked_keys = 0;
    }
}

uint8_t purr_trust_state(const purr_trust_t *t)
{
    return (uint8_t)((t->mode & 3u) | (t->dev ? PURR_SECST_DEV : 0u) |
                     (t->signature_ok ? PURR_SECST_SIGNED : 0u));
}
