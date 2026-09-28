#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "purr_appmgr.h"
#include "purr_fs.h"
#include "purr_util.h"
#include "testcrypto.h"
#include "testkit.h"

/* ---------------------------------------------------------------- a RAM disk, like test_fs.c */

#define BLOCKS 128
#define BSIZE  4096
static uint8_t s_ram[BLOCKS * BSIZE];

static int ram_read(void *ctx, uint32_t block, uint32_t off, void *buf, uint32_t size)
{
    (void)ctx;
    if (block >= BLOCKS || off + size > BSIZE) return -1;
    memcpy(buf, s_ram + block * BSIZE + off, size);
    return 0;
}
static int ram_prog(void *ctx, uint32_t block, uint32_t off, const void *buf, uint32_t size)
{
    (void)ctx;
    if (block >= BLOCKS || off + size > BSIZE) return -1;
    uint8_t *dst = s_ram + block * BSIZE + off;
    const uint8_t *src = buf;
    for (uint32_t i = 0; i < size; i++) dst[i] &= src[i];
    return 0;
}
static int ram_erase(void *ctx, uint32_t block)
{
    (void)ctx;
    if (block >= BLOCKS) return -1;
    memset(s_ram + block * BSIZE, 0xFF, BSIZE);
    return 0;
}

static purr_fs_t fs;

static void fresh_fs(void)
{
    memset(s_ram, 0xFF, sizeof(s_ram));
    purr_bd_t bd = {ram_read, ram_prog, ram_erase, NULL, NULL, BSIZE, BLOCKS};
    CHECK_EQ(purr_fs_format(&fs, &bd), 0);
}

/* ---------------------------------------------------------------- signing */

enum { K_SYS = 0, K_OTHER = 1, K_COUNT = 2 };
static uint8_t g_pub[K_COUNT][64], g_priv[K_COUNT][32];
static purr_keybag_t g_bag;

static void setup_keys(void)
{
    purr_key_t defaults[K_COUNT];
    tc_seed(0xA9CAFEu);
    static const uint8_t roles[K_COUNT] = {PURR_ROLE_DEVELOPER, PURR_ROLE_DEVELOPER};
    for (int i = 0; i < K_COUNT; i++) {
        tc_make_key(g_pub[i], g_priv[i]);
        defaults[i].key_id = (uint8_t)i;
        defaults[i].role = roles[i];
        memcpy(defaults[i].pub, g_pub[i], 64);
    }
    purr_keybag_build(&g_bag, defaults, K_COUNT, NULL);
}

static purr_appmgr_env_t env_for(uint16_t chip)
{
    purr_appmgr_env_t e = {chip, &g_bag, &purr_crypto_uecc};
    return e;
}

typedef struct {
    const char *name, *version;
    uint16_t chip;
    uint32_t payload_len;
    int key;                   /* -1 = leave unsigned (all-zero signature, key id 0) */
} appspec_t;

/* Builds a signed (or deliberately unsigned) .cat image into `out`, returning its size. */
static uint32_t build_app(const appspec_t *s, uint8_t *out)
{
    purr_image_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = PURR_IMAGE_MAGIC;
    h.header_version = PURR_IMAGE_HEADER_VERSION;
    h.header_size = sizeof(h);
    h.chip_id = s->chip;
    h.image_type = PURR_IMG_APP;
    h.key_id = s->key >= 0 ? (uint8_t)s->key : 0;
    strncpy(h.name, s->name, sizeof(h.name) - 1);
    strncpy(h.version, s->version, sizeof(h.version) - 1);
    h.payload_offset = sizeof(h);
    h.payload_size = s->payload_len;

    for (uint32_t i = 0; i < s->payload_len; i++) {
        out[sizeof(h) + i] = (uint8_t)(i * 17u + 3u);
    }
    purr_sha256(out + sizeof(h), s->payload_len, h.payload_sha256);
    if (s->key >= 0) {
        uint8_t digest[32];
        purr_sha256(&h, PURR_IMAGE_SIGNED_LEN, digest);
        tc_sign(g_priv[s->key], digest, h.signature);
    }
    memcpy(out, &h, sizeof(h));
    return (uint32_t)sizeof(h) + s->payload_len;
}

static uint8_t s_img[8192];
static uint8_t s_scratch[8192];

/* ---------------------------------------------------------------- add */

static void test_add_then_scan(void)
{
    fresh_fs();
    purr_appmgr_env_t env = env_for(PURR_CHIP_ESP32S3);
    purr_cfg_t cfg;
    purr_cfg_defaults(&cfg);

    appspec_t s = {"paint", "1.0.0", PURR_CHIP_ESP32S3, 200, K_SYS};
    uint32_t len = build_app(&s, s_img);

    purr_app_registry_t reg;
    memset(&reg, 0, sizeof(reg));
    purr_app_result_t r = purr_appmgr_add(&fs, &env, &cfg, &reg, s_img, len);
    CHECK_EQ(r, PURR_APP_OK);

    purr_appmgr_scan(&fs, &env, s_scratch, sizeof(s_scratch), &reg);
    CHECK_EQ(reg.count, 1);
    CHECK_EQ(reg.dropped, 0);
    const purr_app_entry_t *e = purr_appmgr_find(&reg, "paint");
    CHECK(e != NULL);
    CHECK(strcmp(e->version, "1.0.0") == 0);
    CHECK_EQ(e->size, len);
    CHECK_EQ(e->chip_ok, 1);
    CHECK_EQ(e->verified, 1);
    CHECK_EQ(e->signer_role, PURR_ROLE_DEVELOPER);
}

static void test_add_rejects_bad_magic(void)
{
    fresh_fs();
    purr_appmgr_env_t env = env_for(PURR_CHIP_ESP32S3);
    purr_cfg_t cfg;
    purr_cfg_defaults(&cfg);
    purr_app_registry_t reg = {0};

    uint8_t junk[64];
    memset(junk, 0x42, sizeof(junk));
    CHECK_EQ(purr_appmgr_add(&fs, &env, &cfg, &reg, junk, sizeof(junk)), PURR_APP_BAD_MAGIC);
}

static void test_add_rejects_wrong_type(void)
{
    fresh_fs();
    purr_appmgr_env_t env = env_for(PURR_CHIP_ESP32S3);
    purr_cfg_t cfg;
    purr_cfg_defaults(&cfg);
    purr_app_registry_t reg = {0};

    appspec_t s = {"notanapp", "1.0.0", PURR_CHIP_ESP32S3, 50, K_SYS};
    uint32_t len = build_app(&s, s_img);
    purr_image_header_t *h = (purr_image_header_t *)s_img;
    h->image_type = PURR_IMG_OS;                  /* claim to be a system image instead */
    uint8_t digest[32];
    purr_sha256(h, PURR_IMAGE_SIGNED_LEN, digest);
    tc_sign(g_priv[K_SYS], digest, h->signature);

    CHECK_EQ(purr_appmgr_add(&fs, &env, &cfg, &reg, s_img, len), PURR_APP_BAD_TYPE);
}

static void test_add_rejects_wrong_chip(void)
{
    fresh_fs();
    purr_appmgr_env_t env = env_for(PURR_CHIP_ESP32S3);
    purr_cfg_t cfg;
    purr_cfg_defaults(&cfg);
    purr_app_registry_t reg = {0};

    appspec_t s = {"other_chip", "1.0.0", PURR_CHIP_ESP32, 50, K_SYS};   /* built for esp32, not s3 */
    uint32_t len = build_app(&s, s_img);
    CHECK_EQ(purr_appmgr_add(&fs, &env, &cfg, &reg, s_img, len), PURR_APP_BAD_CHIP);
}

static void test_chip_any_is_accepted(void)
{
    fresh_fs();
    purr_appmgr_env_t env = env_for(PURR_CHIP_ESP32S3);
    purr_cfg_t cfg;
    purr_cfg_defaults(&cfg);
    purr_app_registry_t reg = {0};

    appspec_t s = {"universal", "1.0.0", PURR_CHIP_ANY, 50, K_SYS};
    uint32_t len = build_app(&s, s_img);
    CHECK_EQ(purr_appmgr_add(&fs, &env, &cfg, &reg, s_img, len), PURR_APP_OK);
}

static void test_unsigned_rules(void)
{
    fresh_fs();
    purr_appmgr_env_t env = env_for(PURR_CHIP_ESP32S3);
    appspec_t s = {"sketchy", "1.0.0", PURR_CHIP_ESP32S3, 50, -1};      /* unsigned */
    uint32_t len = build_app(&s, s_img);

    purr_cfg_t cfg;
    purr_cfg_defaults(&cfg);
    cfg.secure_mode = PURR_SECURE_WARN;
    purr_app_registry_t reg = {0};
    CHECK_EQ(purr_appmgr_add(&fs, &env, &cfg, &reg, s_img, len), PURR_APP_BAD_VERIFY);

    cfg.secure_mode = PURR_SECURE_OFF;
    CHECK_EQ(purr_appmgr_add(&fs, &env, &cfg, &reg, s_img, len), PURR_APP_OK);
}

static void test_update_rules(void)
{
    fresh_fs();
    purr_appmgr_env_t env = env_for(PURR_CHIP_ESP32S3);
    purr_cfg_t cfg;
    purr_cfg_defaults(&cfg);

    appspec_t v1 = {"editor", "1.0.0", PURR_CHIP_ESP32S3, 100, K_SYS};
    uint32_t len1 = build_app(&v1, s_img);
    purr_app_registry_t reg = {0};
    CHECK_EQ(purr_appmgr_add(&fs, &env, &cfg, &reg, s_img, len1), PURR_APP_OK);
    purr_appmgr_scan(&fs, &env, s_scratch, sizeof(s_scratch), &reg);

    /* The same version again: rejected. */
    appspec_t v1_again = {"editor", "1.0.0", PURR_CHIP_ESP32S3, 90, K_SYS};
    uint32_t lenA = build_app(&v1_again, s_img);
    CHECK_EQ(purr_appmgr_add(&fs, &env, &cfg, &reg, s_img, lenA), PURR_APP_EXISTS_NEWER);

    /* An older version: rejected. */
    appspec_t v0 = {"editor", "0.9.0", PURR_CHIP_ESP32S3, 90, K_SYS};
    uint32_t len0 = build_app(&v0, s_img);
    CHECK_EQ(purr_appmgr_add(&fs, &env, &cfg, &reg, s_img, len0), PURR_APP_EXISTS_NEWER);

    /* A newer version: replaces it. */
    appspec_t v2 = {"editor", "2.0.0", PURR_CHIP_ESP32S3, 300, K_SYS};
    uint32_t len2 = build_app(&v2, s_img);
    CHECK_EQ(purr_appmgr_add(&fs, &env, &cfg, &reg, s_img, len2), PURR_APP_OK);

    purr_appmgr_scan(&fs, &env, s_scratch, sizeof(s_scratch), &reg);
    CHECK_EQ(reg.count, 1);                       /* still one app, not two */
    const purr_app_entry_t *e = purr_appmgr_find(&reg, "editor");
    CHECK(e != NULL);
    CHECK(strcmp(e->version, "2.0.0") == 0);
    CHECK_EQ(e->size, len2);
}

static void test_registry_full(void)
{
    fresh_fs();
    purr_appmgr_env_t env = env_for(PURR_CHIP_ESP32S3);
    purr_cfg_t cfg;
    purr_cfg_defaults(&cfg);
    purr_app_registry_t reg = {0};

    for (int i = 0; i < PURR_APP_MAX; i++) {
        char name[16];
        snprintf(name, sizeof(name), "app%d", i);
        appspec_t s = {name, "1.0.0", PURR_CHIP_ESP32S3, 20, K_SYS};
        uint32_t len = build_app(&s, s_img);
        CHECK_EQ(purr_appmgr_add(&fs, &env, &cfg, &reg, s_img, len), PURR_APP_OK);
        reg.count++;                              /* fill() has no scan; fake the count as
                                                    * the real caller would after a real scan */
        strncpy(reg.apps[reg.count - 1].name, name, sizeof(reg.apps[0].name) - 1);
        strncpy(reg.apps[reg.count - 1].version, "1.0.0", sizeof(reg.apps[0].version) - 1);
    }
    appspec_t one_more = {"one_more", "1.0.0", PURR_CHIP_ESP32S3, 20, K_SYS};
    uint32_t len = build_app(&one_more, s_img);
    CHECK_EQ(purr_appmgr_add(&fs, &env, &cfg, &reg, s_img, len), PURR_APP_REGISTRY_FULL);

    purr_app_registry_t rescanned;
    purr_appmgr_scan(&fs, &env, s_scratch, sizeof(s_scratch), &rescanned);
    CHECK_EQ(rescanned.count, PURR_APP_MAX);
}

static void test_name_too_long(void)
{
    /* purr_image_header_t.name is a fixed char[32] with no guaranteed terminator. A name
     * that fits (<=31 characters plus the implicit null) is never "too long" by
     * construction; the real case this check exists for is a header with no terminator
     * in the field at all, hand-crafted here since build_app() cannot produce one. */
    fresh_fs();
    purr_appmgr_env_t env = env_for(PURR_CHIP_ESP32S3);
    purr_cfg_t cfg;
    purr_cfg_defaults(&cfg);
    purr_app_registry_t reg = {0};

    appspec_t s = {"placeholder", "1.0.0", PURR_CHIP_ESP32S3, 20, K_SYS};
    uint32_t len = build_app(&s, s_img);
    purr_image_header_t *h = (purr_image_header_t *)s_img;
    memset(h->name, 'a', sizeof(h->name));         /* every byte non-zero: no terminator */
    uint8_t digest[32];
    purr_sha256(h, PURR_IMAGE_SIGNED_LEN, digest);
    tc_sign(g_priv[K_SYS], digest, h->signature);

    CHECK_EQ(purr_appmgr_add(&fs, &env, &cfg, &reg, s_img, len), PURR_APP_NAME_TOO_LONG);
}

static void test_empty_name_rejected(void)
{
    fresh_fs();
    purr_appmgr_env_t env = env_for(PURR_CHIP_ESP32S3);
    purr_cfg_t cfg;
    purr_cfg_defaults(&cfg);
    purr_app_registry_t reg = {0};

    appspec_t s = {"", "1.0.0", PURR_CHIP_ESP32S3, 20, K_SYS};
    uint32_t len = build_app(&s, s_img);
    CHECK_EQ(purr_appmgr_add(&fs, &env, &cfg, &reg, s_img, len), PURR_APP_NAME_TOO_LONG);
}

/* ---------------------------------------------------------------- remove */

static void test_remove(void)
{
    fresh_fs();
    purr_appmgr_env_t env = env_for(PURR_CHIP_ESP32S3);
    purr_cfg_t cfg;
    purr_cfg_defaults(&cfg);
    purr_app_registry_t reg = {0};

    appspec_t s = {"gone_soon", "1.0.0", PURR_CHIP_ESP32S3, 40, K_SYS};
    uint32_t len = build_app(&s, s_img);
    purr_appmgr_add(&fs, &env, &cfg, &reg, s_img, len);
    purr_appmgr_scan(&fs, &env, s_scratch, sizeof(s_scratch), &reg);
    CHECK_EQ(reg.count, 1);

    CHECK_EQ(purr_appmgr_remove(&fs, "gone_soon"), 0);
    purr_appmgr_scan(&fs, &env, s_scratch, sizeof(s_scratch), &reg);
    CHECK_EQ(reg.count, 0);
    CHECK_EQ(purr_appmgr_remove(&fs, "gone_soon"), -1);   /* already gone */
    CHECK_EQ(purr_appmgr_remove(&fs, "never_existed"), -1);
}

/* ---------------------------------------------------------------- recovery of a cut install */

static void test_recover_cleans_tmp_folders(void)
{
    fresh_fs();
    purr_appmgr_env_t env = env_for(PURR_CHIP_ESP32S3);
    purr_cfg_t cfg;
    purr_cfg_defaults(&cfg);
    purr_app_registry_t reg = {0};

    /* Simulate a cut install: a ".tmp" folder with a package inside, never renamed. */
    purr_fs_mkdir(&fs, "/half_installed.tmp");
    purr_fs_write(&fs, "/half_installed.tmp/package.cat", "x", 1);

    purr_appmgr_scan(&fs, &env, s_scratch, sizeof(s_scratch), &reg);
    CHECK_EQ(reg.count, 0);                       /* a .tmp folder is never a real app */

    purr_appmgr_recover(&fs);
    int is_dir;
    CHECK(purr_fs_stat(&fs, "/half_installed.tmp", &is_dir, NULL) < 0);   /* gone */

    /* A real app survives recovery untouched. */
    appspec_t s = {"survivor", "1.0.0", PURR_CHIP_ESP32S3, 30, K_SYS};
    uint32_t len = build_app(&s, s_img);
    purr_appmgr_add(&fs, &env, &cfg, &reg, s_img, len);
    purr_appmgr_recover(&fs);
    purr_appmgr_scan(&fs, &env, s_scratch, sizeof(s_scratch), &reg);
    CHECK_EQ(reg.count, 1);
}

static void test_scan_skips_too_large_for_scratch(void)
{
    fresh_fs();
    purr_appmgr_env_t env = env_for(PURR_CHIP_ESP32S3);
    purr_cfg_t cfg;
    purr_cfg_defaults(&cfg);
    purr_app_registry_t reg = {0};

    appspec_t s = {"big", "1.0.0", PURR_CHIP_ESP32S3, 4000, K_SYS};
    uint32_t len = build_app(&s, s_img);
    CHECK_EQ(purr_appmgr_add(&fs, &env, &cfg, &reg, s_img, len), PURR_APP_OK);

    uint8_t tiny_scratch[64];
    purr_appmgr_scan(&fs, &env, tiny_scratch, sizeof(tiny_scratch), &reg);
    CHECK_EQ(reg.count, 0);
    CHECK_EQ(reg.dropped, 1);
}

static void test_scan_ignores_stray_files_and_incoming(void)
{
    fresh_fs();
    purr_appmgr_env_t env = env_for(PURR_CHIP_ESP32S3);
    purr_app_registry_t reg = {0};

    purr_fs_write(&fs, "/readme.txt", "hi", 2);    /* a stray file, not a folder */
    purr_fs_mkdir(&fs, "/incoming");                /* the transport drop folder */
    purr_fs_write(&fs, "/incoming/something.cat", "x", 1);

    purr_appmgr_scan(&fs, &env, s_scratch, sizeof(s_scratch), &reg);
    CHECK_EQ(reg.count, 0);
    CHECK_EQ(reg.dropped, 0);                       /* neither counts as a bad app: skipped outright */
}

int main(void)
{
    setup_keys();
    test_add_then_scan();
    test_add_rejects_bad_magic();
    test_add_rejects_wrong_type();
    test_add_rejects_wrong_chip();
    test_chip_any_is_accepted();
    test_unsigned_rules();
    test_update_rules();
    test_registry_full();
    test_name_too_long();
    test_empty_name_rejected();
    test_remove();
    test_recover_cleans_tmp_folders();
    test_scan_skips_too_large_for_scratch();
    test_scan_ignores_stray_files_and_incoming();
    TK_DONE("test_appmgr");
}
