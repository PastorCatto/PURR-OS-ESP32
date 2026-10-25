#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "purr_cfg.h"
#include "purr_trust.h"
#include "purr_util.h"
#include "testcrypto.h"
#include "testkit.h"

/* Default keys, as the bootloader compiles them in: 1 boot, 2 system, 3 developer. */
enum { K_BOOT = 1, K_SYSTEM = 2, K_DEV = 3 };
static purr_key_t g_defaults[3];
static uint8_t g_priv[4][32];
static uint8_t g_pub[4][64];

static void setup(void)
{
    tc_seed(0xC0FFEE03u);
    for (int i = 1; i <= 3; i++) {
        tc_make_key(g_pub[i], g_priv[i]);
    }
    g_defaults[0] = (purr_key_t){.key_id = K_BOOT, .role = PURR_ROLE_BOOT};
    g_defaults[1] = (purr_key_t){.key_id = K_SYSTEM, .role = PURR_ROLE_SYSTEM};
    g_defaults[2] = (purr_key_t){.key_id = K_DEV, .role = PURR_ROLE_DEVELOPER};
    for (int i = 0; i < 3; i++) {
        memcpy(g_defaults[i].pub, g_pub[i + 1], 64);
    }
}

static void sign_with(purr_cfg_t *c, const uint8_t priv[32])
{
    uint8_t bytes[PURR_TRUST_SIGNED_MAX], digest[32];
    size_t n = purr_trust_signed_bytes(c, bytes);
    purr_sha256(bytes, n, digest);
    tc_sign(priv, digest, c->trust_sig);
}

static void sign(purr_cfg_t *c)
{
    sign_with(c, g_priv[K_BOOT]);
}

static purr_trust_t eval(const purr_cfg_t *c)
{
    purr_trust_t t;
    purr_trust_eval(c, g_defaults, 3, &purr_crypto_uecc, &t);
    return t;
}

static purr_cfg_t fresh(void)
{
    purr_cfg_t c;
    purr_cfg_defaults(&c);
    return c;
}

static void test_fresh_and_unsigned(void)
{
    purr_cfg_t c = fresh();
    purr_trust_t t = eval(&c);
    CHECK_EQ(t.mode, PURR_SECURE_WARN);
    CHECK_EQ(t.dev, 0);
    CHECK_EQ(t.signature_ok, 0);

    purr_trust_eval(NULL, g_defaults, 3, &purr_crypto_uecc, &t);      /* no cfg at all */
    CHECK_EQ(t.mode, PURR_SECURE_WARN);

    /* An unsigned secure_mode is ignored: the device behaves as warn either way. */
    c.secure_mode = PURR_SECURE_ENFORCE;
    CHECK_EQ(eval(&c).mode, PURR_SECURE_WARN);
    c.secure_mode = PURR_SECURE_OFF;
    CHECK_EQ(eval(&c).mode, PURR_SECURE_WARN);

    /* Unsigned writers can only tighten. */
    c = fresh();
    c.tighten = PURR_SECURE_ENFORCE;
    CHECK_EQ(eval(&c).mode, PURR_SECURE_ENFORCE);
    c.tighten = PURR_SECURE_WARN;
    CHECK_EQ(eval(&c).mode, PURR_SECURE_WARN);
    c.tighten = PURR_SECURE_OFF;
    CHECK_EQ(eval(&c).mode, PURR_SECURE_WARN);
    c.tighten = 200;                                                  /* garbage never loosens */
    CHECK_EQ(eval(&c).mode, PURR_SECURE_WARN);

    /* A blank or zeroed signature field is "no signature", and costs no crypto. */
    memset(c.trust_sig, 0xFF, 64);
    CHECK_EQ(purr_trust_has_signature(&c), 0);
    memset(c.trust_sig, 0x00, 64);
    CHECK_EQ(purr_trust_has_signature(&c), 0);
}

static void test_signed_block(void)
{
    purr_cfg_t c = fresh();
    c.secure_mode = PURR_SECURE_ENFORCE;
    sign(&c);
    purr_trust_t t = eval(&c);
    CHECK_EQ(t.signature_ok, 1);
    CHECK_EQ(t.mode, PURR_SECURE_ENFORCE);

    /* A signed block may loosen, all the way to off. */
    c = fresh();
    c.secure_mode = PURR_SECURE_OFF;
    sign(&c);
    CHECK_EQ(eval(&c).mode, PURR_SECURE_OFF);
    c.tighten = PURR_SECURE_ENFORCE;                                  /* but tighten still raises it */
    CHECK_EQ(eval(&c).mode, PURR_SECURE_ENFORCE);

    /* A signed warn plus an unsigned tighten. */
    c = fresh();
    c.secure_mode = PURR_SECURE_WARN;
    sign(&c);
    c.tighten = PURR_SECURE_ENFORCE;
    t = eval(&c);
    CHECK_EQ(t.mode, PURR_SECURE_ENFORCE);
    CHECK_EQ(t.signature_ok, 1);                                      /* tighten is outside the signature */

    /* An unknown signed mode is never "off". */
    c = fresh();
    c.secure_mode = 7;
    sign(&c);
    CHECK_EQ(eval(&c).mode, PURR_SECURE_WARN);
}

static void test_tampering_and_wrong_signers(void)
{
    purr_cfg_t good = fresh();
    good.secure_mode = PURR_SECURE_ENFORCE;
    good.revoked_keys = 1u << 5;
    good.keys[0] = (purr_key_slot_t){.key_id = 9, .role = PURR_ROLE_DEVELOPER, .valid = 1};
    memset(good.keys[0].pub, 0x42, 64);
    sign(&good);
    CHECK_EQ(eval(&good).signature_ok, 1);

    /* Every signed field, changed after signing, breaks it -> back to warn, nothing trusted. */
    purr_cfg_t c = good;
    c.secure_mode = PURR_SECURE_OFF;
    CHECK_EQ(eval(&c).signature_ok, 0);
    CHECK_EQ(eval(&c).mode, PURR_SECURE_WARN);

    c = good; c.trust_flags ^= PURR_TRUSTF_DEV_LOCKED;      CHECK_EQ(eval(&c).signature_ok, 0);
    c = good; c.revoked_keys = 0;                           CHECK_EQ(eval(&c).signature_ok, 0);
    c = good; c.keys[0].pub[17] ^= 1;                       CHECK_EQ(eval(&c).signature_ok, 0);
    c = good; c.keys[3].valid = 1;                          CHECK_EQ(eval(&c).signature_ok, 0);
    c = good; c.trust_sig[5] ^= 1;                          CHECK_EQ(eval(&c).signature_ok, 0);

    /* Fields outside the block may change freely. */
    c = good; c.tighten = PURR_SECURE_ENFORCE; c.flags |= PURR_CFGF_DEV_MODE; c.seq = 99;
    c.boot_fail_count = 5; c.floors[0].component = PURR_COMP_KERNEL; c.floors[0].version = 0x01020304;
    CHECK_EQ(eval(&c).signature_ok, 1);

    /* Signed by a key that is not a boot-role default: a system key, a developer key. */
    c = good; sign_with(&c, g_priv[K_SYSTEM]);   CHECK_EQ(eval(&c).signature_ok, 0);
    c = good; sign_with(&c, g_priv[K_DEV]);      CHECK_EQ(eval(&c).signature_ok, 0);

    /* An override key in the block cannot vouch for itself: a boot-role key that exists only in
     * keys[] signs its own block, and the evaluator only trusts the compiled-in defaults. */
    uint8_t rogue_pub[64], rogue_priv[32];
    tc_make_key(rogue_pub, rogue_priv);
    c = fresh();
    c.secure_mode = PURR_SECURE_OFF;
    c.keys[0] = (purr_key_slot_t){.key_id = 7, .role = PURR_ROLE_BOOT, .valid = 1};
    memcpy(c.keys[0].pub, rogue_pub, 64);
    sign_with(&c, rogue_priv);
    purr_trust_t t = eval(&c);
    CHECK_EQ(t.signature_ok, 0);
    CHECK_EQ(t.mode, PURR_SECURE_WARN);
}

static void test_development_mode(void)
{
    /* off by default; a fresh device is not in it */
    purr_cfg_t c = fresh();
    CHECK_EQ(eval(&c).dev, 0);

    /* unsigned: dev relaxes a tightened enforce to warn, no lower */
    c.tighten = PURR_SECURE_ENFORCE;
    CHECK_EQ(eval(&c).mode, PURR_SECURE_ENFORCE);
    c.flags |= PURR_CFGF_DEV_MODE;
    purr_trust_t t = eval(&c);
    CHECK_EQ(t.dev, 1);
    CHECK_EQ(t.mode, PURR_SECURE_WARN);

    /* signed enforce, dev on: warn */
    c = fresh();
    c.secure_mode = PURR_SECURE_ENFORCE;
    sign(&c);
    c.flags |= PURR_CFGF_DEV_MODE;
    t = eval(&c);
    CHECK_EQ(t.dev, 1);
    CHECK_EQ(t.mode, PURR_SECURE_WARN);
    CHECK_EQ(t.signature_ok, 1);

    /* a signed production block locks development mode out entirely */
    c = fresh();
    c.secure_mode = PURR_SECURE_ENFORCE;
    c.trust_flags = PURR_TRUSTF_DEV_LOCKED;
    sign(&c);
    c.flags |= PURR_CFGF_DEV_MODE;
    t = eval(&c);
    CHECK_EQ(t.dev, 0);
    CHECK_EQ(t.mode, PURR_SECURE_ENFORCE);

    /* ...but DEV_LOCKED means nothing unless it is signed */
    c = fresh();
    c.trust_flags = PURR_TRUSTF_DEV_LOCKED;
    c.tighten = PURR_SECURE_ENFORCE;
    c.flags |= PURR_CFGF_DEV_MODE;
    t = eval(&c);
    CHECK_EQ(t.dev, 1);
    CHECK_EQ(t.mode, PURR_SECURE_WARN);

    /* dev mode never lowers a signed off, and never raises anything */
    c = fresh();
    c.secure_mode = PURR_SECURE_OFF;
    sign(&c);
    c.flags |= PURR_CFGF_DEV_MODE;
    CHECK_EQ(eval(&c).mode, PURR_SECURE_OFF);
}

static int g_flash_touched;
static int ff_read(void *c, uint32_t a, void *b, uint32_t n) { (void)c; (void)a; memset(b, 0xFF, n); return 0; }
static int ff_erase(void *c, uint32_t a) { (void)c; (void)a; g_flash_touched++; return 0; }
static int ff_write(void *c, uint32_t a, const void *b, uint32_t n) { (void)c; (void)a; (void)b; (void)n; g_flash_touched++; return 0; }

static void test_view(void)
{
    purr_cfg_t c = fresh();
    c.secure_mode = PURR_SECURE_ENFORCE;               /* unsigned: ignored */
    c.revoked_keys = 0xFF;
    c.keys[2] = (purr_key_slot_t){.key_id = 4, .role = PURR_ROLE_BOOT, .valid = 1};
    c.tighten = PURR_SECURE_ENFORCE;
    c.flags |= PURR_CFGF_DEV_MODE;
    purr_trust_t t = eval(&c);
    purr_cfg_t v;
    purr_trust_view(&c, &t, &v);
    CHECK_EQ(v.magic, PURR_CFG_VIEW_MAGIC);
    CHECK_EQ(v.secure_mode, PURR_SECURE_WARN);         /* tighten enforce, relaxed by dev */
    CHECK_EQ(v.revoked_keys, 0);
    CHECK_EQ(v.keys[2].valid, 0);                      /* the unsigned override key is gone */
    CHECK_EQ(c.keys[2].valid, 1);                      /* the original is untouched */
    CHECK_EQ(c.secure_mode, PURR_SECURE_ENFORCE);

    /* a view is never stored back */
    purr_flash_t fl = {ff_read, ff_erase, ff_write, NULL, 0};
    g_flash_touched = 0;
    CHECK_EQ(purr_cfg_store(&fl, &v), -1);
    CHECK_EQ(g_flash_touched, 0);
    CHECK_EQ(purr_cfg_valid(&v), 0);

    /* a signed block's keys and revocations survive into the view */
    c = fresh();
    c.secure_mode = PURR_SECURE_ENFORCE;
    c.revoked_keys = 0x20;
    c.keys[1] = (purr_key_slot_t){.key_id = 8, .role = PURR_ROLE_VENDOR, .valid = 1};
    sign(&c);
    t = eval(&c);
    purr_trust_view(&c, &t, &v);
    CHECK_EQ(v.revoked_keys, 0x20);
    CHECK_EQ(v.keys[1].valid, 1);
    CHECK_EQ(v.secure_mode, PURR_SECURE_ENFORCE);
}

static void test_layout_and_state(void)
{
    /* The purr_cfg_t offsets purrstrap/lib/purrcfg.py builds against. If one moves, that file
     * (and the signed bytes) must move with it. */
    CHECK_EQ(sizeof(purr_cfg_t), 752u);
    CHECK_EQ(offsetof(purr_cfg_t, secure_mode), 12u);
    CHECK_EQ(offsetof(purr_cfg_t, boot_target), 13u);
    CHECK_EQ(offsetof(purr_cfg_t, tighten), 14u);
    CHECK_EQ(offsetof(purr_cfg_t, trust_flags), 15u);
    CHECK_EQ(offsetof(purr_cfg_t, flags), 16u);
    CHECK_EQ(offsetof(purr_cfg_t, revoked_keys), 20u);
    CHECK_EQ(offsetof(purr_cfg_t, keys), 108u);
    CHECK_EQ(sizeof(purr_key_slot_t), 68u);
    CHECK_EQ(offsetof(purr_cfg_t, trust_sig), 684u);
    CHECK_EQ(offsetof(purr_cfg_t, crc32), 748u);
    CHECK_EQ(PURR_CFGF_DEV_MODE, 1u << 8);
    CHECK_EQ(PURR_TRUSTF_DEV_LOCKED, 1u);

    /* The exact bytes purrstrap signs (purrstrap/scripts/keys.py builds the same). */
    purr_cfg_t c = fresh();
    c.secure_mode = PURR_SECURE_ENFORCE;
    c.trust_flags = PURR_TRUSTF_DEV_LOCKED;
    c.revoked_keys = 0x04030201u;
    c.keys[0] = (purr_key_slot_t){.key_id = 5, .role = PURR_ROLE_DEVELOPER, .valid = 1};
    uint8_t b[PURR_TRUST_SIGNED_MAX];
    size_t n = purr_trust_signed_bytes(&c, b);
    CHECK_EQ(n, 9u + 1u + 1u + 1u + 4u + 8u * 68u);
    CHECK_EQ(n, (size_t)PURR_TRUST_SIGNED_MAX - 0u);
    CHECK(memcmp(b, "PURRTRUST", 9) == 0);
    CHECK_EQ(b[9], 0);
    CHECK_EQ(b[10], PURR_SECURE_ENFORCE);
    CHECK_EQ(b[11], PURR_TRUSTF_DEV_LOCKED);
    CHECK_EQ(b[12], 0x01); CHECK_EQ(b[13], 0x02); CHECK_EQ(b[14], 0x03); CHECK_EQ(b[15], 0x04);
    CHECK_EQ(b[16], 5);                                 /* keys[0].key_id */
    CHECK_EQ(b[17], PURR_ROLE_DEVELOPER);
    CHECK_EQ(b[18], 1);                                 /* keys[0].valid */

    purr_trust_t t = {PURR_SECURE_ENFORCE, 1, 1};
    CHECK_EQ(purr_trust_state(&t), PURR_SECURE_ENFORCE | PURR_SECST_DEV | PURR_SECST_SIGNED);
    t = (purr_trust_t){PURR_SECURE_WARN, 0, 0};
    CHECK_EQ(purr_trust_state(&t), PURR_SECURE_WARN);
    CHECK_EQ(PURR_SECST_MODE(purr_trust_state(&t)), PURR_SECURE_WARN);
}

int main(void)
{
    setup();
    test_fresh_and_unsigned();
    test_signed_block();
    test_tampering_and_wrong_signers();
    test_development_mode();
    test_view();
    test_layout_and_state();
    TK_DONE("test_trust");
}
