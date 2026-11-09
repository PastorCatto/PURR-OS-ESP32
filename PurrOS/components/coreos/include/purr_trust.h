/*
 * purr_trust.h - the effective security state of a device, from its purrcfg
 * (bootloader/SPEC.md section 5, plan3.md "Design: trust block, development mode, secure status").
 *
 * Plain C with no ESP-IDF dependency; shared by the bootloader and the OS and tested on the PC.
 *
 * purrcfg carries two kinds of trust state:
 *
 *   SIGNED (a 64-byte ECDSA P-256 signature by a boot-role key in trust_sig): secure_mode,
 *   trust_flags, revoked_keys and the override keys[]. They count only if the signature
 *   verifies against the COMPILED-IN default keys -- never against the overridden bag, so an
 *   override cannot vouch for itself. Anything else is treated as "defaults": the compiled-in
 *   keys, no revocations, mode `warn`.
 *
 *   UNSIGNED, written by the OS: `tighten` (a floor on the mode) and the DEV_MODE flag. These
 *   can only ever make the device stricter, except DEV_MODE, which relaxes `enforce` to `warn`
 *   and nothing further.
 *
 *     base = signature_ok ? secure_mode : warn            (anything above enforce = warn)
 *     mode = max(base, tighten)
 *     dev  = DEV_MODE set and not (signature_ok and DEV_LOCKED)
 *     if dev and mode > warn: mode = warn
 *
 * An unsigned write can therefore raise the mode, and can lower it only through development
 * mode, which is visible (status, handoff, boot menu) and which a production signature locks out.
 */
#ifndef PURR_TRUST_H
#define PURR_TRUST_H

#include <stddef.h>
#include <stdint.h>

#include "purr_abi.h"
#include "purr_keybag.h"
#include "purr_verify.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t mode;          /* effective PURR_SECURE_* */
    int     dev;           /* development mode is active */
    int     signature_ok;  /* the trust block's signature verified */
} purr_trust_t;

/*
 * The bytes the trust signature covers, written to out (at least PURR_TRUST_SIGNED_MAX long):
 *   "PURRTRUST" 0x00 | secure_mode | trust_flags | revoked_keys (u32 LE) | keys[8] (as stored)
 * Returns the length. purrstrap (purrstrap/scripts/keys.py) builds the same bytes to sign.
 */
#define PURR_TRUST_SIGNED_MAX  (10 + 2 + 4 + PURR_KEY_SLOTS * sizeof(purr_key_slot_t))
size_t purr_trust_signed_bytes(const purr_cfg_t *cfg, uint8_t *out);

/* Whether trust_sig is a signature at all (not blank flash, not zeroed). Cheap; no crypto. */
int purr_trust_has_signature(const purr_cfg_t *cfg);

/*
 * Evaluate cfg. `defaults` are the compiled-in keys (purr_default_keys); only those with the
 * boot role can sign a trust block. crypto is the P-256 backend. A cfg with no signature costs
 * no crypto. Always fills *out; with cfg == NULL it yields the fresh-device state.
 */
void purr_trust_eval(const purr_cfg_t *cfg, const purr_key_t *defaults, size_t ndefaults,
                     const purr_crypto_t *crypto, purr_trust_t *out);

/*
 * A copy of cfg with the effective values in place, for DECISIONS ONLY: secure_mode is the
 * effective mode, and if the signature did not verify the override keys and revocations are
 * dropped. The copy's magic is PURR_CFG_VIEW_MAGIC, so purr_cfg_store() refuses it. Use it
 * wherever code reads secure_mode or builds a key bag; keep the original for storing.
 */
void purr_trust_view(const purr_cfg_t *cfg, const purr_trust_t *t, purr_cfg_t *view);

/* The packed secure_state byte for the handoff (PURR_SECST_*). */
uint8_t purr_trust_state(const purr_trust_t *t);

#ifdef __cplusplus
}
#endif

#endif /* PURR_TRUST_H */
