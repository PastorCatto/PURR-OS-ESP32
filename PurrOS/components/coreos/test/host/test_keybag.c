#include <string.h>

#include "purr_keybag.h"
#include "purr_cfg.h"
#include "testkit.h"

static purr_key_t key(uint8_t id, uint8_t role, uint8_t fill)
{
    purr_key_t k;
    memset(&k, 0, sizeof(k));
    k.key_id = id;
    k.role = role;
    memset(k.pub, fill, sizeof(k.pub));
    return k;
}

static void test_build_and_find(void)
{
    purr_keybag_t bag;
    purr_key_t defaults[3] = {key(0, PURR_ROLE_BOOT, 0xA0), key(1, PURR_ROLE_SYSTEM, 0xA1),
                              key(2, PURR_ROLE_OWNER, 0xA2)};

    CHECK_EQ(purr_keybag_build(&bag, defaults, 3, NULL), 3);
    CHECK(purr_keybag_find(&bag, 1) != NULL);
    CHECK_EQ(purr_keybag_find(&bag, 1)->role, PURR_ROLE_SYSTEM);
    CHECK_EQ(purr_keybag_find(&bag, 2)->pub[0], 0xA2);
    CHECK(purr_keybag_find(&bag, 9) == NULL);
    CHECK_EQ(bag.revoked, 0);

    /* An empty default set is a valid, empty bag. */
    CHECK_EQ(purr_keybag_build(&bag, NULL, 0, NULL), 0);
    CHECK(purr_keybag_find(&bag, 0) == NULL);
}

static void test_overrides(void)
{
    purr_keybag_t bag;
    purr_key_t defaults[2] = {key(0, PURR_ROLE_BOOT, 0xA0), key(1, PURR_ROLE_SYSTEM, 0xA1)};
    purr_cfg_t cfg;

    purr_cfg_defaults(&cfg);
    /* Replace key 1, add key 5, and one slot that is not valid. */
    cfg.keys[0] = (purr_key_slot_t){1, PURR_ROLE_SYSTEM, 1, 0, {0}};
    memset(cfg.keys[0].pub, 0xB1, PURR_PUBKEY_LEN);
    cfg.keys[1] = (purr_key_slot_t){5, PURR_ROLE_VENDOR, 1, 0, {0}};
    memset(cfg.keys[1].pub, 0xB5, PURR_PUBKEY_LEN);
    cfg.keys[2] = (purr_key_slot_t){6, PURR_ROLE_DEVELOPER, 0, 0, {0}};   /* valid = 0 */
    cfg.revoked_keys = 1u << 0;

    CHECK_EQ(purr_keybag_build(&bag, defaults, 2, &cfg), 3);
    CHECK_EQ(purr_keybag_find(&bag, 1)->pub[0], 0xB1);           /* replaced */
    CHECK_EQ(purr_keybag_find(&bag, 0)->pub[0], 0xA0);           /* untouched */
    CHECK_EQ(purr_keybag_find(&bag, 5)->role, PURR_ROLE_VENDOR); /* added */
    CHECK(purr_keybag_find(&bag, 6) == NULL);                    /* invalid slot ignored */
    CHECK(purr_keybag_is_revoked(&bag, 0));
    CHECK(!purr_keybag_is_revoked(&bag, 1));
}

static void test_limits(void)
{
    purr_keybag_t bag;
    purr_key_t many[PURR_KEYBAG_MAX + 1];
    purr_cfg_t cfg;

    for (int i = 0; i < PURR_KEYBAG_MAX + 1; i++) {
        many[i] = key((uint8_t)i, PURR_ROLE_DEVELOPER, 1);
    }
    /* Too many defaults do not fit. */
    CHECK_EQ(purr_keybag_build(&bag, many, PURR_KEYBAG_MAX + 1, NULL), -1);
    /* A key id beyond the revocation mask is refused. */
    purr_key_t bad = key(40, PURR_ROLE_BOOT, 1);
    CHECK_EQ(purr_keybag_build(&bag, &bad, 1, NULL), -1);

    /* A full bag drops an extra override instead of failing. */
    CHECK_EQ(purr_keybag_build(&bag, many, PURR_KEYBAG_MAX, NULL), PURR_KEYBAG_MAX);
    purr_cfg_defaults(&cfg);
    cfg.keys[0] = (purr_key_slot_t){31, PURR_ROLE_VENDOR, 1, 0, {0}};
    CHECK_EQ(purr_keybag_build(&bag, many, PURR_KEYBAG_MAX, &cfg), PURR_KEYBAG_MAX);
    CHECK(purr_keybag_find(&bag, 31) == NULL);

    /* Ids that cannot be tracked in the mask always count as revoked. */
    CHECK(purr_keybag_is_revoked(&bag, 32));
}

static void test_roles(void)
{
    /* Boot images: boot role only. */
    CHECK(purr_role_may_sign(PURR_ROLE_BOOT, PURR_IMG_OS, 0));
    CHECK(purr_role_may_sign(PURR_ROLE_BOOT, PURR_IMG_RECOVERY, 0));
    CHECK(!purr_role_may_sign(PURR_ROLE_SYSTEM, PURR_IMG_OS, 0));
    CHECK(!purr_role_may_sign(PURR_ROLE_OWNER, PURR_IMG_RECOVERY, 0));
    CHECK(!purr_role_may_sign(PURR_ROLE_DEVELOPER, PURR_IMG_OS, 0));

    /* System modules by subtype. */
    CHECK(purr_role_may_sign(PURR_ROLE_BOOT, PURR_IMG_MODULE, PURR_MOD_KERNEL));
    CHECK(purr_role_may_sign(PURR_ROLE_BOOT, PURR_IMG_MODULE, PURR_MOD_COREOS));
    CHECK(purr_role_may_sign(PURR_ROLE_BOOT, PURR_IMG_MODULE, PURR_MOD_BOOTPKG));
    CHECK(purr_role_may_sign(PURR_ROLE_BOOT, PURR_IMG_MODULE, PURR_MOD_LOADER));
    CHECK(!purr_role_may_sign(PURR_ROLE_SYSTEM, PURR_IMG_MODULE, PURR_MOD_KERNEL));
    CHECK(purr_role_may_sign(PURR_ROLE_SYSTEM, PURR_IMG_MODULE, PURR_MOD_APPMANAGER));
    CHECK(purr_role_may_sign(PURR_ROLE_SYSTEM, PURR_IMG_MODULE, PURR_MOD_RUNTIME));
    CHECK(purr_role_may_sign(PURR_ROLE_SYSTEM, PURR_IMG_MODULE, PURR_MOD_DEVBUNDLE));
    CHECK(!purr_role_may_sign(PURR_ROLE_BOOT, PURR_IMG_MODULE, PURR_MOD_APPMANAGER));
    CHECK(!purr_role_may_sign(PURR_ROLE_VENDOR, PURR_IMG_MODULE, PURR_MOD_RUNTIME));
    CHECK(!purr_role_may_sign(PURR_ROLE_DEVELOPER, PURR_IMG_MODULE, PURR_MOD_DEVBUNDLE));

    /* Drivers: system, vendor, developer and owner. Never boot. */
    CHECK(purr_role_may_sign(PURR_ROLE_SYSTEM, PURR_IMG_MODULE, PURR_MOD_DRIVER));
    CHECK(purr_role_may_sign(PURR_ROLE_VENDOR, PURR_IMG_MODULE, PURR_MOD_DRIVER));
    CHECK(purr_role_may_sign(PURR_ROLE_DEVELOPER, PURR_IMG_MODULE, PURR_MOD_DRIVER));
    CHECK(purr_role_may_sign(PURR_ROLE_OWNER, PURR_IMG_MODULE, PURR_MOD_DRIVER));
    CHECK(!purr_role_may_sign(PURR_ROLE_BOOT, PURR_IMG_MODULE, PURR_MOD_DRIVER));

    /* Apps: everyone except boot. */
    CHECK(purr_role_may_sign(PURR_ROLE_SYSTEM, PURR_IMG_APP, 0));
    CHECK(purr_role_may_sign(PURR_ROLE_OWNER, PURR_IMG_APP, 0));
    CHECK(purr_role_may_sign(PURR_ROLE_DEVELOPER, PURR_IMG_APP, 0));
    CHECK(purr_role_may_sign(PURR_ROLE_VENDOR, PURR_IMG_APP, 0));
    CHECK(!purr_role_may_sign(PURR_ROLE_BOOT, PURR_IMG_APP, 0));

    /* Nothing signs unknown types, subtypes or the empty role. */
    CHECK(!purr_role_may_sign(PURR_ROLE_BOOT, 9, 0));
    CHECK(!purr_role_may_sign(PURR_ROLE_BOOT, PURR_IMG_MODULE, 99));
    for (int t = 0; t <= 5; t++) {
        CHECK(!purr_role_may_sign(PURR_ROLE_NONE, (uint8_t)t, 0));
    }
}

int main(void)
{
    test_build_and_find();
    test_overrides();
    test_limits();
    test_roles();
    TK_DONE("test_keybag");
}
