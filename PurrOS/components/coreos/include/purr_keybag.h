/*
 * purr_keybag.h - the virtual key bag (Keys/SPEC.md, bootloader/SPEC.md section 4).
 *
 * Compiled-in default keys, with overrides from purrcfg applied by key id, and
 * a revocation mask. Read-only: changing a key goes through purrcfg as a
 * signed request.
 */
#ifndef PURR_KEYBAG_H
#define PURR_KEYBAG_H

#include <stddef.h>
#include <stdint.h>

#include "purr_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_KEYBAG_MAX 16

typedef struct {
    uint8_t key_id;
    uint8_t role;                    /* PURR_ROLE_* */
    uint8_t pub[PURR_PUBKEY_LEN];
} purr_key_t;

typedef struct {
    purr_key_t keys[PURR_KEYBAG_MAX];
    uint8_t    count;
    uint32_t   revoked;              /* bit per key id */
} purr_keybag_t;

/*
 * Build the effective bag: the defaults, then the valid override slots of cfg
 * (cfg may be NULL). An override with the same key id replaces the default,
 * otherwise it is added if there is room. Returns the number of keys, or -1
 * if the defaults alone do not fit.
 */
int purr_keybag_build(purr_keybag_t *bag, const purr_key_t *defaults,
                      size_t ndefaults, const purr_cfg_t *cfg);

/* The key with this id, or NULL. Revoked keys are still returned. */
const purr_key_t *purr_keybag_find(const purr_keybag_t *bag, uint8_t key_id);

int purr_keybag_is_revoked(const purr_keybag_t *bag, uint8_t key_id);

/*
 * May a key with this role sign this kind of file? For system modules the
 * subtype is PURR_FLAGS_SUBTYPE(header flags), otherwise 0.
 */
int purr_role_may_sign(uint8_t role, uint8_t image_type, uint8_t subtype);

#ifdef __cplusplus
}
#endif

#endif /* PURR_KEYBAG_H */
