#include "purr_keybag.h"

#include <string.h>

static int add_or_replace(purr_keybag_t *bag, uint8_t key_id, uint8_t role,
                          const uint8_t *pub)
{
    for (uint8_t i = 0; i < bag->count; i++) {
        if (bag->keys[i].key_id == key_id) {
            bag->keys[i].role = role;
            memcpy(bag->keys[i].pub, pub, PURR_PUBKEY_LEN);
            return 0;
        }
    }
    if (bag->count >= PURR_KEYBAG_MAX) {
        return -1;
    }
    bag->keys[bag->count].key_id = key_id;
    bag->keys[bag->count].role = role;
    memcpy(bag->keys[bag->count].pub, pub, PURR_PUBKEY_LEN);
    bag->count++;
    return 0;
}

int purr_keybag_build(purr_keybag_t *bag, const purr_key_t *defaults,
                      size_t ndefaults, const purr_cfg_t *cfg)
{
    memset(bag, 0, sizeof(*bag));

    for (size_t i = 0; i < ndefaults; i++) {
        if (defaults[i].key_id > PURR_KEY_MAX_ID) {
            return -1;
        }
        if (add_or_replace(bag, defaults[i].key_id, defaults[i].role,
                           defaults[i].pub) != 0) {
            return -1;
        }
    }
    if (cfg != NULL) {
        for (int i = 0; i < PURR_KEY_SLOTS; i++) {
            const purr_key_slot_t *s = &cfg->keys[i];
            if (s->valid != 1 || s->key_id > PURR_KEY_MAX_ID) {
                continue;
            }
            /* An override that does not fit is skipped, never fatal. */
            (void)add_or_replace(bag, s->key_id, s->role, s->pub);
        }
        bag->revoked = cfg->revoked_keys;
    }
    return bag->count;
}

const purr_key_t *purr_keybag_find(const purr_keybag_t *bag, uint8_t key_id)
{
    for (uint8_t i = 0; i < bag->count; i++) {
        if (bag->keys[i].key_id == key_id) {
            return &bag->keys[i];
        }
    }
    return NULL;
}

int purr_keybag_is_revoked(const purr_keybag_t *bag, uint8_t key_id)
{
    if (key_id > PURR_KEY_MAX_ID) {
        return 1;   /* cannot be tracked, so never trusted */
    }
    return (bag->revoked >> key_id) & 1u;
}

int purr_role_may_sign(uint8_t role, uint8_t image_type, uint8_t subtype)
{
    switch (image_type) {
    case PURR_IMG_OS:
    case PURR_IMG_RECOVERY:
        return role == PURR_ROLE_BOOT;

    case PURR_IMG_MODULE:
        switch (subtype) {
        case PURR_MOD_KERNEL:
        case PURR_MOD_COREOS:
        case PURR_MOD_BOOTPKG:
        case PURR_MOD_LOADER:
            return role == PURR_ROLE_BOOT;
        case PURR_MOD_APPMANAGER:
        case PURR_MOD_RUNTIME:
        case PURR_MOD_DEVBUNDLE:
            return role == PURR_ROLE_SYSTEM;
        case PURR_MOD_DRIVER:
            return role == PURR_ROLE_SYSTEM || role == PURR_ROLE_VENDOR ||
                   role == PURR_ROLE_DEVELOPER || role == PURR_ROLE_OWNER;
        default:
            return 0;
        }

    case PURR_IMG_APP:
        return role == PURR_ROLE_SYSTEM || role == PURR_ROLE_OWNER ||
               role == PURR_ROLE_DEVELOPER || role == PURR_ROLE_VENDOR;

    default:
        return 0;
    }
}
