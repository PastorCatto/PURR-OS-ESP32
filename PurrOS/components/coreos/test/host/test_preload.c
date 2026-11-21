#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "purr_appmgr.h"
#include "purr_preload.h"
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

/* purr_appmgr no longer takes a raw purr_fs_t* (purr_appmgr.h's purr_appmgr_fs_t); these just
 * close back over the test fixture's `fs` the same way commands.c's kernel_table_fs_* wrappers
 * close over the real one. */
static int test_fs_list(const char *path, purr_fs_list_fn cb, void *ctx) { return purr_fs_list(&fs, path, cb, ctx); }
static int test_fs_read(const char *path, purr_fs_read_fn cb, void *ctx) { return purr_fs_read(&fs, path, cb, ctx); }
static int test_fs_write(const char *path, const void *data, uint32_t len) { return purr_fs_write(&fs, path, data, len); }
static int test_fs_mkdir(const char *path) { return purr_fs_mkdir(&fs, path); }
static int test_fs_remove(const char *path) { return purr_fs_remove(&fs, path); }
static int test_fs_rename(const char *from, const char *to) { return purr_fs_rename(&fs, from, to); }
static int test_fs_stat(const char *path, int *is_dir, uint32_t *size) { return purr_fs_stat(&fs, path, is_dir, size); }

static const purr_appmgr_fs_t s_test_fs = {
    .list = test_fs_list,
    .read = test_fs_read,
    .write = test_fs_write,
    .mkdir = test_fs_mkdir,
    .remove = test_fs_remove,
    .rename = test_fs_rename,
    .stat = test_fs_stat,
};

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

static int exists(const char *path)
{
    int is_dir = 0;
    return purr_fs_stat(&fs, path, &is_dir, NULL) == 0;
}

static void put_cat(const char *file, const char *name, const char *version, int key, uint16_t chip)
{
    appspec_t sp = {name, version, chip, 80, key};
    uint32_t len = build_app(&sp, s_img);
    char path[64];
    snprintf(path, sizeof(path), "/boot/preload/%s", file);
    CHECK_EQ(purr_fs_write(&fs, path, s_img, len), 0);
}

static void test_the_list_is_the_apps_in_the_folder(void)
{
    fresh_fs();
    purr_fs_mkdir(&fs, "/boot");
    purr_fs_mkdir(&fs, "/boot/preload");
    put_cat("notes.cat", "notes", "1.2.0", K_SYS, PURR_CHIP_ESP32S3);
    put_cat("clock.cat", "clock", "0.9.1", K_SYS, PURR_CHIP_ANY);
    put_cat("other-chip.cat", "esp32only", "1.0.0", K_SYS, PURR_CHIP_ESP32);        /* not for this chip: left out */
    purr_fs_write(&fs, "/boot/preload/readme.txt", "hello", 5);                    /* not a .cat */
    purr_fs_write(&fs, "/boot/preload/junk.cat", "not a package at all, just some bytes that are long enough to be read as a header", 83);
    purr_fs_mkdir(&fs, "/boot/preload/sub.cat");                                  /* a folder with a .cat name */
    purr_preload_app_t apps[PURR_PRELOAD_MAX];
    int n = purr_preload_list(&s_test_fs, PURR_PRELOAD_DIR, PURR_CHIP_ESP32S3, s_scratch, sizeof(s_scratch), apps, PURR_PRELOAD_MAX);
    CHECK_EQ(n, 2);
    CHECK(strcmp(apps[0].file, "clock.cat") == 0);                                /* sorted by file name */
    CHECK(strcmp(apps[0].name, "clock") == 0);
    CHECK(strcmp(apps[0].version, "0.9.1") == 0);
    CHECK(strcmp(apps[1].name, "notes") == 0);
    CHECK(apps[1].size > sizeof(purr_image_header_t));
    CHECK_EQ(purr_preload_list(&s_test_fs, PURR_PRELOAD_DIR, PURR_CHIP_ESP32S3, s_scratch, sizeof(s_scratch), apps, 1), 1);       /* the cap is kept */
    CHECK_EQ(purr_preload_list(&s_test_fs, "/nowhere", PURR_CHIP_ESP32S3, s_scratch, sizeof(s_scratch), apps, PURR_PRELOAD_MAX), 0);   /* no folder: no apps, no error */
    uint8_t tiny[100];
    CHECK_EQ(purr_preload_list(&s_test_fs, PURR_PRELOAD_DIR, PURR_CHIP_ESP32S3, tiny, sizeof(tiny), apps, PURR_PRELOAD_MAX), 0);       /* too big for the buffer: skipped */
}

static void test_planting_installs_under_the_account(void)
{
    fresh_fs();
    purr_fs_mkdir(&fs, "/boot");
    purr_fs_mkdir(&fs, "/boot/preload");
    put_cat("notes.cat", "notes", "1.2.0", K_SYS, PURR_CHIP_ESP32S3);
    put_cat("unsigned.cat", "unsigned", "1.0.0", -1, PURR_CHIP_ESP32S3);
    purr_fs_mkdir(&fs, "/home");
    purr_fs_mkdir(&fs, "/home/ada");
    purr_fs_mkdir(&fs, "/home/ada/apps");
    purr_appmgr_env_t env = env_for(PURR_CHIP_ESP32S3);
    purr_cfg_t cfg;
    purr_cfg_defaults(&cfg);
    purr_app_registry_t reg;
    purr_appmgr_scan(&s_test_fs, "/home/ada/apps", &env, s_scratch, sizeof(s_scratch), &reg);

    CHECK_EQ(purr_preload_plant(&s_test_fs, PURR_PRELOAD_DIR, "notes.cat", "/home/ada/apps", &env, &cfg, &reg, s_scratch, sizeof(s_scratch)), PURR_APP_OK);
    CHECK(exists("/home/ada/apps/notes/package.cat"));
    CHECK(exists("/boot/preload/notes.cat"));                                     /* the file stays, for an app that is added later */
    purr_appmgr_scan(&s_test_fs, "/home/ada/apps", &env, s_scratch, sizeof(s_scratch), &reg);
    CHECK_EQ(reg.count, 1);
    CHECK(reg.apps[0].verified);

    /* Again, the same version: "already installed", which the wizard counts as done. */
    CHECK_EQ(purr_preload_plant(&s_test_fs, PURR_PRELOAD_DIR, "notes.cat", "/home/ada/apps", &env, &cfg, &reg, s_scratch, sizeof(s_scratch)), PURR_APP_EXISTS_NEWER);

    /* A package that does not verify is refused, and says so; the folder is not left half made. */
    CHECK_EQ(purr_preload_plant(&s_test_fs, PURR_PRELOAD_DIR, "unsigned.cat", "/home/ada/apps", &env, &cfg, &reg, s_scratch, sizeof(s_scratch)), PURR_APP_BAD_VERIFY);
    CHECK(!exists("/home/ada/apps/unsigned"));
    CHECK(!exists("/home/ada/apps/unsigned.tmp"));

    /* A name that is not a bare file name, and one that is not there. */
    CHECK_EQ(purr_preload_plant(&s_test_fs, PURR_PRELOAD_DIR, "../etc/x.cat", "/home/ada/apps", &env, &cfg, &reg, s_scratch, sizeof(s_scratch)), PURR_APP_BAD_NAME);
    CHECK_EQ(purr_preload_plant(&s_test_fs, PURR_PRELOAD_DIR, "missing.cat", "/home/ada/apps", &env, &cfg, &reg, s_scratch, sizeof(s_scratch)), PURR_APP_IO_ERROR);
}

int main(void)
{
    setup_keys();
    test_the_list_is_the_apps_in_the_folder();
    test_planting_installs_under_the_account();
    TK_DONE("test_preload");
}
