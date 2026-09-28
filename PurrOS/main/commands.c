#include "commands.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mmu_map.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "login.h"
#include "purr_appmgr.h"
#include "purr_module.h"
#include "purr_module_abi.h"
#include "purr_relocate.h"
#include "purr_cfgstore.h"
#include "purr_console.h"
#include "purr_crypto_mbedtls.h"
#include "purr_fetch.h"
#include "purr_fs.h"
#include "purr_kernel.h"
#include "purr_manifest.h"
#include "purr_net.h"
#include "purr_util.h"
#include "purr_verify.h"
#include "purr_wifi.h"

extern const purr_key_t purr_default_keys[];
extern const size_t purr_default_keys_count;

#if CONFIG_IDF_TARGET_ESP32S3
#define NET_INSTALL_CHIP PURR_CHIP_ESP32S3
#else
#define NET_INSTALL_CHIP PURR_CHIP_ESP32
#endif

#define VERSION "0.1.0"

/* Where real modules (Modules/SPEC.md) live on the root filesystem. */
#define PURR_MODULES_DIR "/system"

#if CONFIG_PURR_PROFILE_RECOVERY
#define SYSTEM_NAME  "KittenOS"
#define PROFILE_NAME "recovery"
#elif CONFIG_PURR_PROFILE_MINIMAL
#define SYSTEM_NAME  "PURR Loader"
#define PROFILE_NAME "minimal"
#else
#define SYSTEM_NAME  "PURR OS"
#define PROFILE_NAME "full"
#endif

const char *purr_system_name(void)
{
    return SYSTEM_NAME;
}

/* ---------------------------------------------------------------- filesystem */

static purr_fs_t s_fs;

static void fail(purr_cli_t *cli, const char *what, int err)
{
    purr_cli_printf(cli, "%s: %s\n", what, purr_fs_strerror(err));
}

/* Paths are absolute. A missing leading slash is added, so "etc" means "/etc". */
static const char *abs_path(const char *in, char *out, size_t n)
{
    snprintf(out, n, "%s%s", in[0] == '/' ? "" : "/", in);
    return out;
}

static int need_fs(purr_cli_t *cli)
{
    if (purr_fs_mounted(&s_fs)) {
        return 1;
    }
    purr_cli_puts(cli, "no filesystem mounted (run: format --yes)\n");
    return 0;
}

purr_fs_t *purr_login_fs(void)
{
    return &s_fs;
}

void purr_fs_setup(purr_cli_t *cli)
{
    purr_bd_t bd;
    if (purr_fs_flash_bd("root", &bd) != 0) {
        purr_cli_puts(cli, "root: no such partition\n");
        return;
    }
    int e = purr_fs_mount(&s_fs, &bd);
    if (e == 0) {
        uint32_t used = 0, total = 0;
        purr_fs_usage(&s_fs, &used, &total);
        purr_cli_printf(cli, "root: mounted, %uK of %uK used\n",
                        (unsigned)(used * 4), (unsigned)(total * 4));
    } else {
        purr_cli_printf(cli, "root: not mounted (%s)\nrun: format --yes\n", purr_fs_strerror(e));
    }
}

static void print_entry(void *ctx, const char *name, int is_dir, uint32_t size)
{
    purr_cli_t *cli = ctx;
    if (is_dir) {
        purr_cli_printf(cli, "  %s/\n", name);
    } else {
        purr_cli_printf(cli, "  %-20s %u\n", name, (unsigned)size);
    }
}

static int cmd_ls(purr_cli_t *cli, int argc, char **argv)
{
    if (!need_fs(cli)) return 1;
    char path[80];
    int e = purr_fs_list(&s_fs, abs_path(argc > 1 ? argv[1] : "/", path, sizeof(path)), print_entry, cli);
    if (e < 0) { fail(cli, argc > 1 ? argv[1] : "/", e); return 1; }
    return 0;
}

static int show(void *ctx, const void *data, uint32_t len)
{
    purr_cli_t *cli = ctx;
    const uint8_t *p = data;
    for (uint32_t i = 0; i < len; i++) {
        if (p[i] == '\n' || (p[i] >= 0x20 && p[i] < 0x7F)) {
            cli->put(cli->put_ctx, (char)p[i]);
        } else if (p[i] != '\r') {
            cli->put(cli->put_ctx, '.');          /* binary: show a dot */
        }
    }
    return 0;
}

static int cmd_cat(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) { purr_cli_puts(cli, "usage: cat <file>\n"); return 1; }
    if (!need_fs(cli)) return 1;
    char path[80];
    int e = purr_fs_read(&s_fs, abs_path(argv[1], path, sizeof(path)), show, cli);
    if (e < 0) { fail(cli, argv[1], e); return 1; }
    purr_cli_puts(cli, "\n");
    return 0;
}

static int cmd_mkdir(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) { purr_cli_puts(cli, "usage: mkdir <dir>\n"); return 1; }
    if (!need_fs(cli)) return 1;
    char path[80];
    int e = purr_fs_mkdir(&s_fs, abs_path(argv[1], path, sizeof(path)));
    if (e < 0) { fail(cli, argv[1], e); return 1; }
    return 0;
}

static int cmd_rm(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) { purr_cli_puts(cli, "usage: rm <file or empty dir>\n"); return 1; }
    if (!need_fs(cli)) return 1;
    char path[80];
    int e = purr_fs_remove(&s_fs, abs_path(argv[1], path, sizeof(path)));
    if (e < 0) { fail(cli, argv[1], e); return 1; }
    return 0;
}

static int cmd_mv(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 3) { purr_cli_puts(cli, "usage: mv <from> <to>\n"); return 1; }
    if (!need_fs(cli)) return 1;
    char a[80], b[80];
    int e = purr_fs_rename(&s_fs, abs_path(argv[1], a, sizeof(a)), abs_path(argv[2], b, sizeof(b)));
    if (e < 0) { fail(cli, argv[1], e); return 1; }
    return 0;
}

static int cmd_write(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 3) { purr_cli_puts(cli, "usage: write <file> <text...>\n"); return 1; }
    if (!need_fs(cli)) return 1;
    char path[80], text[PURR_CLI_LINE];
    text[0] = '\0';
    for (int i = 2; i < argc; i++) {
        size_t used = strlen(text);
        snprintf(text + used, sizeof(text) - used, "%s%s", i > 2 ? " " : "", argv[i]);
    }
    strncat(text, "\n", sizeof(text) - strlen(text) - 1);
    int e = purr_fs_write(&s_fs, abs_path(argv[1], path, sizeof(path)), text, (uint32_t)strlen(text));
    if (e < 0) { fail(cli, argv[1], e); return 1; }
    return 0;
}

static int cmd_df(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!need_fs(cli)) return 1;
    uint32_t used, total;
    int e = purr_fs_usage(&s_fs, &used, &total);
    if (e < 0) { fail(cli, "df", e); return 1; }
    purr_cli_printf(cli, "root: %uK used of %uK (%u blocks free)\n", (unsigned)(used * 4),
                    (unsigned)(total * 4), (unsigned)(total - used));
    return 0;
}

static int cmd_format(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "--yes") != 0) {
        purr_cli_puts(cli, "this erases everything on the root filesystem.\nrun: format --yes\n");
        return 1;
    }
    purr_bd_t bd;
    if (purr_fs_flash_bd("root", &bd) != 0) {
        purr_cli_puts(cli, "root: no such partition\n");
        return 1;
    }
    purr_cli_puts(cli, "formatting...\n");
    purr_console_flush();
    purr_fs_unmount(&s_fs);
    int e = purr_fs_format(&s_fs, &bd);
    if (e < 0) { fail(cli, "format", e); return 1; }
    purr_cli_puts(cli, "done.\n");
    return 0;
}

/* ---------------------------------------------------------------- apps */
/*
 * Apps live per-user now (Users/SPEC.md), at "/home/<name>/apps" on the shared root
 * filesystem -- not a dedicated partition (that partition existed for one session and is
 * gone; see the partition CSVs). Every command below scans/adds/removes under the CURRENT
 * logged-in user's own home, via purr_appmgr's root parameter.
 */
#define APP_SCRATCH_CAP (512 * 1024)
static uint8_t *s_app_scratch;                /* allocated on first use; apps is rarely touched */

static uint8_t *app_scratch(void)
{
    if (s_app_scratch == NULL) {
        s_app_scratch = heap_caps_malloc(APP_SCRATCH_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_app_scratch == NULL) {
            s_app_scratch = malloc(APP_SCRATCH_CAP);
        }
    }
    return s_app_scratch;
}

/* "/home/<name>/apps", built fresh each time from whoever is logged in right now -- never
 * cached, since the logged-in user can change between calls (su, logout/login). */
static void user_apps_root(char *out, size_t outcap, const char *username)
{
    snprintf(out, outcap, "/home/%s/apps", username);
}

/* Creates /home, /home/<name> and /home/<name>/apps if they don't already exist. purr_fs_mkdir
 * on an existing directory is just an error this ignores; LittleFS needs each level to exist
 * before the next can be created, so these go one at a time, outermost first. */
static void ensure_user_home(const char *username)
{
    purr_fs_mkdir(&s_fs, "/home");
    char home[48];
    snprintf(home, sizeof(home), "/home/%s", username);
    purr_fs_mkdir(&s_fs, home);
    char apps[64];
    user_apps_root(apps, sizeof(apps), username);
    purr_fs_mkdir(&s_fs, apps);
}

/* Replaces the old dedicated apps-partition mount: ensures every already-registered user has
 * a home+apps folder, and cleans up any cut install left over from last time, for each of
 * them. Called once at boot, after purr_fs_setup (this needs the root filesystem, not a
 * separate one). */
void purr_apps_setup(purr_cli_t *cli)
{
    if (!purr_fs_mounted(&s_fs)) {
        purr_cli_puts(cli, "apps: no root filesystem mounted\n");
        return;
    }
    purr_user_list_t *users = purr_login_users();
    int n = 0;
    for (int i = 0; users != NULL && i < users->count; i++) {
        ensure_user_home(users->users[i].name);
        char root[64];
        user_apps_root(root, sizeof(root), users->users[i].name);
        purr_appmgr_recover(&s_fs, root);
        n++;
    }
    purr_cli_printf(cli, "apps: ready (%d user home%s)\n", n, n == 1 ? "" : "s");
}

static purr_appmgr_env_t apps_env(purr_cfg_t *cfg_out)
{
    purr_flash_t fl;
    if (purr_cfgstore_open(&fl) != 0 || purr_cfg_load(&fl, cfg_out, NULL) < 0) {
        purr_cfg_defaults(cfg_out);
    }
    static purr_keybag_t bag;
    purr_keybag_build(&bag, purr_default_keys, purr_default_keys_count, cfg_out);
    purr_appmgr_env_t env = {NET_INSTALL_CHIP, &bag, &purr_crypto_mbedtls};
    return env;
}

/* cmd_apps used to live here. It's now Modules/appmanager/apps_module.c, the first real
 * command ported out of the monolith and into an actual loaded module -- see
 * Modules/SPEC.md. core_table_apps_scan() (near the module loader, further down this file)
 * is what it calls through the core table instead of touching purr_appmgr directly. */

static int cmd_appinfo(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) {
        purr_cli_puts(cli, "usage: appinfo <name>\n");
        return 1;
    }
    if (!need_fs(cli)) return 1;
    char root[64];
    user_apps_root(root, sizeof(root), purr_login_current()->name);
    purr_cfg_t cfg;
    purr_appmgr_env_t env = apps_env(&cfg);
    purr_app_registry_t reg;
    purr_appmgr_scan(&s_fs, root, &env, app_scratch(), APP_SCRATCH_CAP, &reg);
    const purr_app_entry_t *e = purr_appmgr_find(&reg, argv[1]);
    if (e == NULL) {
        purr_cli_printf(cli, "appinfo: no such app '%s'\n", argv[1]);
        return 1;
    }
    purr_cli_printf(cli, "name:     %s\n", e->name);
    purr_cli_printf(cli, "version:  %s\n", e->version);
    purr_cli_printf(cli, "size:     %uK\n", (unsigned)(e->size / 1024));
    purr_cli_printf(cli, "chip:     %s\n", e->chip_ok ? "matches this device" : "WRONG for this device");
    purr_cli_printf(cli, "verified: %s (signer role %u)\n", e->verified ? "yes" : "no", (unsigned)e->signer_role);
    return 0;
}

typedef struct {
    uint8_t *buf;
    uint32_t cap, len;
} appinstall_read_ctx_t;

static int appinstall_accumulate(void *vctx, const void *data, uint32_t n)
{
    appinstall_read_ctx_t *c = vctx;
    if (c->len + n > c->cap) {
        return -1;
    }
    memcpy(c->buf + c->len, data, n);
    c->len += n;
    return 0;
}

static int is_url(const char *s)
{
    return strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0;
}

static int cmd_appinstall(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) {
        purr_cli_puts(cli, "usage: appinstall <file on root, or a URL>\n");
        return 1;
    }
    if (!need_fs(cli)) return 1;
    char root[64];
    user_apps_root(root, sizeof(root), purr_login_current()->name);
    ensure_user_home(purr_login_current()->name); /* in case this user was created after boot */

    uint8_t *scratch = app_scratch();
    if (scratch == NULL) {
        purr_cli_puts(cli, "appinstall: out of memory\n");
        return 1;
    }

    /* Scan first, while `scratch` is free to use for reading each existing app; the
     * result is what add() needs to apply the update-must-be-newer rule. */
    purr_cfg_t cfg;
    purr_appmgr_env_t env = apps_env(&cfg);
    purr_app_registry_t reg;
    purr_appmgr_scan(&s_fs, root, &env, scratch, APP_SCRATCH_CAP, &reg);

    /* Now read the package to install into the same buffer, either from a URL (there is
     * no other way onto the device yet, with no MTP transport built) or from root. */
    uint32_t len = 0;
    if (is_url(argv[1])) {
        purr_cli_printf(cli, "fetching %s...\n", argv[1]);
        purr_console_flush();
        size_t got = 0;
        esp_err_t err = purr_fetch_into(argv[1], scratch, APP_SCRATCH_CAP, &got);
        if (err != ESP_OK) {
            purr_cli_printf(cli, "appinstall: fetch failed (%s)\n", esp_err_to_name(err));
            return 1;
        }
        len = (uint32_t)got;
    } else {
        char path[80];
        appinstall_read_ctx_t rc = {scratch, APP_SCRATCH_CAP, 0};
        int e = purr_fs_read(&s_fs, abs_path(argv[1], path, sizeof(path)), appinstall_accumulate, &rc);
        if (e < 0) {
            fail(cli, argv[1], e);
            return 1;
        }
        len = rc.len;
    }

    purr_app_result_t r = purr_appmgr_add(&s_fs, root, &env, &cfg, &reg, scratch, len);
    if (r != PURR_APP_OK) {
        purr_cli_printf(cli, "appinstall: %s\n", purr_app_result_name(r));
        return 1;
    }
    purr_cli_puts(cli, "installed\n");
    return 0;
}

static int cmd_appremove(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) {
        purr_cli_puts(cli, "usage: appremove <name>\n");
        return 1;
    }
    if (!need_fs(cli)) return 1;
    char root[64];
    user_apps_root(root, sizeof(root), purr_login_current()->name);
    if (purr_appmgr_remove(&s_fs, root, argv[1]) != 0) {
        purr_cli_printf(cli, "appremove: no such app '%s'\n", argv[1]);
        return 1;
    }
    purr_cli_printf(cli, "%s removed\n", argv[1]);
    return 0;
}

static int cmd_appformat(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "--yes") != 0) {
        purr_cli_puts(cli, "this erases every app installed for you (not other users).\n"
                          "run: appformat --yes\n");
        return 1;
    }
    if (!need_fs(cli)) return 1;
    char root[64];
    user_apps_root(root, sizeof(root), purr_login_current()->name);

    /* Apps now live on the SHARED root filesystem, alongside every other user, logins, and
     * /system -- a raw low-level format here would erase everything on the device, not just
     * apps. This removes only what purr_appmgr itself put under this user's own apps root. */
    purr_cfg_t cfg;
    purr_appmgr_env_t env = apps_env(&cfg);
    purr_app_registry_t reg;
    purr_appmgr_scan(&s_fs, root, &env, app_scratch(), APP_SCRATCH_CAP, &reg);
    int removed = 0;
    for (int i = 0; i < reg.count; i++) {
        if (purr_appmgr_remove(&s_fs, root, reg.apps[i].name) == 0) {
            removed++;
        }
    }
    purr_cli_printf(cli, "removed %d app(s).\n", removed);
    return 0;
}

/* ---------------------------------------------------------------- Wi-Fi */

void purr_net_setup(purr_cli_t *cli)
{
    esp_err_t e = purr_net_init(&s_fs);
    if (e != ESP_OK) {
        purr_cli_printf(cli, "wifi: did not start (%s)\n", esp_err_to_name(e));
    }
}

/* ---------------------------------------------------------------- net install */

typedef struct {
    const uint8_t *base;
    size_t size;
} install_mem_ctx_t;

static int install_mem_read(void *ctx, uint32_t offset, void *buf, uint32_t len)
{
    install_mem_ctx_t *m = ctx;
    if ((uint64_t)offset + len > m->size) {
        return -1;
    }
    memcpy(buf, m->base + offset, len);
    return 0;
}

/* Resolves `name` against the directory `base_url` sits in, same as the recovery loader. */
static void resolve_manifest_url(const char *base_url, const char *name, char *out, size_t cap)
{
    const char *slash = strrchr(base_url, '/');
    size_t dir_len = slash ? (size_t)(slash - base_url) + 1 : 0;
    if (dir_len >= cap) {
        dir_len = 0;
    }
    memcpy(out, base_url, dir_len);
    snprintf(out + dir_len, cap - dir_len, "%s", name);
}

/* Installs a component from the recovery manifest into ota_0 (Install/SPEC.md's network
 * install, run from an already-working system instead of the recovery loader). Defaults to
 * "purros"; a component name can be given to fetch something else the manifest lists. */
static int cmd_net_install(purr_cli_t *cli, int argc, char **argv)
{
    const char *component = argc > 1 ? argv[1] : "purros";
    purr_cli_printf(cli, "netinstall: %s\n", component);
    purr_console_flush();

    purr_net_status_t st;
    purr_net_status(&st);
    if (!st.connected) {
        purr_cli_puts(cli, "not connected. run: wifi connect <ssid> [password]\n");
        return 1;
    }

    purr_cli_puts(cli, "fetching the recovery manifest...\n");
    purr_console_flush();
    uint8_t *manifest_buf = NULL;
    size_t manifest_len = 0;
    if (purr_fetch_alloc(CONFIG_PURR_RECOVERY_MANIFEST_URL, &manifest_buf, &manifest_len,
                         32 * 1024) != ESP_OK) {
        purr_cli_puts(cli, "could not fetch the manifest\n");
        return 1;
    }
    purr_manifest_t man;
    purr_manifest_parse(&man, (const char *)manifest_buf, manifest_len);
    free(manifest_buf);

    const purr_manifest_entry_t *entry = purr_manifest_find(&man, component, CONFIG_IDF_TARGET,
                                                             purr_board()->name);
    if (entry == NULL) {
        purr_cli_printf(cli, "no %s entry for this board in the manifest\n", component);
        return 1;
    }
    purr_cli_printf(cli, "found %s %s (%u bytes)\n", component, entry->version, (unsigned)entry->size);

    char image_url[256];
    resolve_manifest_url(CONFIG_PURR_RECOVERY_MANIFEST_URL, entry->file, image_url, sizeof(image_url));
    purr_cli_printf(cli, "downloading %s...\n", entry->file);
    purr_console_flush();

    uint8_t *image = NULL;
    size_t image_len = 0;
    if (purr_fetch_alloc(image_url, &image, &image_len, 2 * 1024 * 1024) != ESP_OK) {
        purr_cli_puts(cli, "download failed\n");
        return 1;
    }
    uint8_t got_hash[PURR_SHA256_LEN];
    purr_sha256(image, image_len, got_hash);
    if (image_len != entry->size || memcmp(got_hash, entry->sha256, PURR_SHA256_LEN) != 0) {
        free(image);
        purr_cli_puts(cli, "download does not match the manifest (size or hash)\n");
        return 1;
    }

    purr_cli_puts(cli, "verifying...\n");
    purr_console_flush();
    purr_flash_t fl;
    purr_cfg_t cfg;
    if (purr_cfgstore_open(&fl) != 0 || purr_cfg_load(&fl, &cfg, NULL) < 0) {
        purr_cfg_defaults(&cfg);
    }
    purr_keybag_t bag;
    purr_keybag_build(&bag, purr_default_keys, purr_default_keys_count, &cfg);
    purr_verify_env_t env = {
        .chip_id = NET_INSTALL_CHIP, .bag = &bag, .crypto = &purr_crypto_mbedtls,
        .bootloader_version = 0, .version_floor = 0, .enforce_floor = 0,
    };
    install_mem_ctx_t rdctx = {image, image_len};
    purr_image_header_t hdr;
    purr_verify_result_t vr = purr_image_verify(&env, install_mem_read, &rdctx, (uint32_t)image_len, &hdr);

    if (hdr.magic != PURR_IMAGE_MAGIC || hdr.image_type != PURR_IMG_OS) {
        free(image);
        purr_cli_puts(cli, "not a PURR OS image\n");
        return 1;
    }
    if (vr != PURR_V_OK && cfg.secure_mode != PURR_SECURE_OFF) {
        free(image);
        purr_cli_printf(cli, "verification failed: %s\n", purr_verify_name(vr));
        return 1;
    }
    if (vr != PURR_V_OK) {
        purr_cli_printf(cli, "unverified (%s), accepted because secure mode is off\n",
                        purr_verify_name(vr));
    }

    const esp_partition_t *slot = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                            ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (slot == NULL) {
        free(image);
        purr_cli_puts(cli, "no ota_0 partition on this board\n");
        return 1;
    }
    if (hdr.payload_size > slot->size) {
        free(image);
        purr_cli_puts(cli, "the image is bigger than the ota_0 partition\n");
        return 1;
    }

    purr_cli_puts(cli, "writing to ota_0...\n");
    purr_console_flush();
    const uint8_t *payload = image + hdr.payload_offset;
    if (esp_partition_erase_range(slot, 0, slot->size) != ESP_OK ||
        esp_partition_write(slot, 0, payload, hdr.payload_size) != ESP_OK) {
        free(image);
        purr_cli_puts(cli, "write failed\n");
        return 1;
    }

    /* Read back and compare, rather than trust the write. */
    uint8_t chunk[512];
    purr_sha256_t sha;
    uint8_t digest[PURR_SHA256_LEN];
    purr_sha256_init(&sha);
    for (uint32_t off = 0; off < hdr.payload_size; off += sizeof(chunk)) {
        uint32_t n = hdr.payload_size - off;
        if (n > sizeof(chunk)) {
            n = sizeof(chunk);
        }
        if (esp_partition_read(slot, off, chunk, n) != ESP_OK) {
            free(image);
            purr_cli_puts(cli, "read-back failed\n");
            return 1;
        }
        purr_sha256_update(&sha, chunk, n);
    }
    purr_sha256_final(&sha, digest);
    free(image);
    if (memcmp(digest, hdr.payload_sha256, sizeof(digest)) != 0) {
        purr_cli_puts(cli, "read-back hash mismatch: the write did not take\n");
        return 1;
    }

    if (esp_ota_set_boot_partition(slot) != ESP_OK) {
        purr_cli_puts(cli, "installed, but could not set it as the boot target\n");
        return 1;
    }
    purr_cli_printf(cli, "%s %s installed. Run: reboot\n", component, hdr.version);
    return 0;
}


static int cmd_version(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    purr_cli_printf(cli, "%s %s (%s profile)\n", SYSTEM_NAME, VERSION, PROFILE_NAME);
    return 0;
}

static int cmd_info(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    purr_display_info_t di = {0};
    const purr_display_v2_t *d = purr_kernel_display();
    if (d) {
        d->get_info(&di);
    }
    purr_cli_printf(cli, "system:  %s %s\n", SYSTEM_NAME, VERSION);
    purr_cli_printf(cli, "board:   %s\n", purr_board()->name);
    purr_cli_printf(cli, "chip:    %s rev %d.%d, %d cores\n", CONFIG_IDF_TARGET,
                    chip.revision / 100, chip.revision % 100, chip.cores);
    purr_cli_printf(cli, "display: %s %dx%d\n", d ? di.name : "none", di.width, di.height);
    purr_cli_printf(cli, "idf:     %s\n", esp_get_idf_version());
    return 0;
}

static int cmd_mem(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    purr_cli_printf(cli, "internal: %u free, %u largest block\n",
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram) {
        purr_cli_printf(cli, "psram:    %u free of %u\n",
                        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)psram);
    }
    return 0;
}

static int cmd_uptime(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    unsigned s = (unsigned)(esp_timer_get_time() / 1000000);
    purr_cli_printf(cli, "up %u:%02u:%02u\n", s / 3600, (s / 60) % 60, s % 60);
    return 0;
}

static int cmd_parts(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    purr_cli_puts(cli, "name      type sub offset   size\n");
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        purr_cli_printf(cli, "%-9s %-4s %02x  %06x  %uK\n", p->label,
                        p->type == ESP_PARTITION_TYPE_APP ? "app" : "data",
                        (unsigned)p->subtype, (unsigned)p->address, (unsigned)(p->size / 1024));
    }
    esp_partition_iterator_release(it);
    return 0;
}

static int cmd_echo(purr_cli_t *cli, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        purr_cli_printf(cli, "%s%s", i > 1 ? " " : "", argv[i]);
    }
    purr_cli_puts(cli, "\n");
    return 0;
}

static int cmd_clear(purr_cli_t *cli, int argc, char **argv)
{
    (void)cli; (void)argc; (void)argv;
    purr_console_clear();
    return 0;
}

/* Load purrcfg, or say why not. */
static int load_cfg(purr_cli_t *cli, purr_flash_t *fl, purr_cfg_t *cfg)
{
    if (purr_cfgstore_open(fl) != 0) {
        purr_cli_puts(cli, "purrcfg: no such partition\n");
        return -1;
    }
    int r = purr_cfg_load(fl, cfg, NULL);
    if (r < 0) {
        purr_cli_puts(cli, "purrcfg: read error\n");
        return -1;
    }
    return r;                                     /* 0 = read, 1 = blank, defaults used */
}

static int cmd_reboot(purr_cli_t *cli, int argc, char **argv)
{
    uint32_t flag = 0;
    const char *what = NULL;
    if (argc == 2 && strcmp(argv[1], "recovery") == 0) {
        flag = PURR_CFGF_FORCE_RECOVERY;
        what = "recovery";
    } else if (argc == 2 && strcmp(argv[1], "loader") == 0) {
        flag = PURR_CFGF_FORCE_LOADER;
        what = "the recovery loader";
    } else if (argc > 1) {
        purr_cli_puts(cli, "usage: reboot [recovery|loader]\n");
        return 1;
    }
    if (flag != 0) {
        /* The one-shot request: the bootloader clears it and starts the target once. */
        purr_flash_t fl;
        purr_cfg_t cfg;
        if (load_cfg(cli, &fl, &cfg) < 0) {
            return 1;
        }
        purr_cfg_set_flag(&cfg, flag);
        if (purr_cfg_store(&fl, &cfg) != 0) {
            purr_cli_puts(cli, "purrcfg: could not write the request\n");
            return 1;
        }
        purr_cli_printf(cli, "restarting into %s...\n", what);
    } else {
        purr_cli_puts(cli, "restarting...\n");
    }
    purr_console_flush();
    esp_restart();
    return 0;
}

static int cmd_purrcfg(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    purr_flash_t fl;
    purr_cfg_t cfg;
    int r = load_cfg(cli, &fl, &cfg);
    if (r < 0) {
        return 1;
    }
    static const char *const modes[] = {"off", "warn", "enforce"};
    purr_cli_printf(cli, "copy:        %s\n", r == 0 ? "stored" : "none yet (defaults)");
    purr_cli_printf(cli, "seq:         %u\n", (unsigned)cfg.seq);
    purr_cli_printf(cli, "secure mode: %s\n", cfg.secure_mode < 3 ? modes[cfg.secure_mode] : "?");
    purr_cli_printf(cli, "flags:       0x%x%s\n", (unsigned)cfg.flags,
                    (cfg.flags & PURR_CFGF_FORCE_RECOVERY) ? " (recovery requested)" : "");
    purr_cli_printf(cli, "boot count:  %u, fails %u\n", (unsigned)cfg.boot_seq, (unsigned)cfg.boot_fail_count);
    return 0;
}

/* ---------------------------------------------------------------- accounts */

static void console_puts_raw(const char *s)
{
    while (*s) {
        purr_console_put(NULL, *s++);
    }
    purr_console_flush();
}

/* A masked line read directly from the keyboard, bypassing the shell's own line editor
 * (which would echo it in the clear). Used for passwords typed mid-command. */
static void read_masked_line(char *buf, size_t cap)
{
    size_t len = 0;
    for (;;) {
        char c = purr_kernel_key();
        if (c == 0) {
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }
        if (c == '\r' || c == '\n') {
            break;
        }
        if ((c == '\b' || c == 0x7F) && len > 0) {
            len--;
            console_puts_raw("\b \b");
            continue;
        }
        if (c >= 0x20 && c <= 0x7E && len < cap - 1) {
            buf[len++] = c;
            console_puts_raw("*");
        }
    }
    buf[len] = '\0';
    console_puts_raw("\n");
}

static int cmd_whoami(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    purr_cli_printf(cli, "%s%s\n", purr_login_current()->name, purr_login_is_root() ? " (root)" : "");
    return 0;
}

static int cmd_id(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    const purr_user_t *u = purr_login_current();
    purr_cli_printf(cli, "uid=%u(%s) role=%s%s\n", u->uid, u->name,
                    u->role == PURR_ROLE_USER_ADMIN ? "admin" : "standard",
                    purr_login_is_root() ? " root" : "");
    return 0;
}

static int cmd_logout(purr_cli_t *cli, int argc, char **argv)
{
    (void)cli; (void)argc; (void)argv;
    purr_login_set_root(0);
    purr_login_request_logout();
    return 0;
}

static int cmd_su(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    if (purr_login_is_root()) {
        purr_cli_puts(cli, "already root\n");
        return 0;
    }
    if (purr_login_current()->role != PURR_ROLE_USER_ADMIN) {
        purr_cli_puts(cli, "su: only an admin can do that\n");
        return 1;
    }
    purr_cli_puts(cli, "password: ");
    purr_console_flush();
    char pass[64];
    read_masked_line(pass, sizeof(pass));
    int ok = purr_shadow_check(purr_login_shadow(), purr_login_current()->name, pass);
    memset(pass, 0, sizeof(pass));
    if (!ok) {
        purr_cli_puts(cli, "su: incorrect password\n");
        return 1;
    }
    purr_login_set_root(1);
    return 0;
}

static int cmd_passwd(purr_cli_t *cli, int argc, char **argv)
{
    const char *target = argc > 1 ? argv[1] : purr_login_current()->name;
    int is_self = strcmp(target, purr_login_current()->name) == 0;
    if (!is_self && !purr_login_is_root() && purr_login_current()->role != PURR_ROLE_USER_ADMIN) {
        purr_cli_puts(cli, "passwd: only an admin can change another account's password\n");
        return 1;
    }
    if (purr_user_list_find(purr_login_users(), target) == NULL) {
        purr_cli_printf(cli, "passwd: no such user '%s'\n", target);
        return 1;
    }
    if (is_self) {
        purr_cli_puts(cli, "current password: ");
        purr_console_flush();
        char current[64];
        read_masked_line(current, sizeof(current));
        int ok = purr_shadow_check(purr_login_shadow(), target, current);
        memset(current, 0, sizeof(current));
        if (!ok) {
            purr_cli_puts(cli, "passwd: incorrect password\n");
            return 1;
        }
    }
    purr_cli_puts(cli, "new password: ");
    purr_console_flush();
    char pass1[64], pass2[64];
    read_masked_line(pass1, sizeof(pass1));
    purr_cli_puts(cli, "confirm: ");
    purr_console_flush();
    read_masked_line(pass2, sizeof(pass2));
    if (strcmp(pass1, pass2) != 0 || pass1[0] == '\0') {
        memset(pass1, 0, sizeof(pass1));
        memset(pass2, 0, sizeof(pass2));
        purr_cli_puts(cli, "passwd: those did not match, or were empty\n");
        return 1;
    }
    uint8_t salt[PURR_SALT_LEN];
    purr_login_random_salt(salt);
    purr_shadow_set(purr_login_shadow(), target, pass1, salt, PURR_PBKDF2_ITERATIONS);
    memset(pass1, 0, sizeof(pass1));
    memset(pass2, 0, sizeof(pass2));
    if (purr_login_persist() != 0) {
        purr_cli_puts(cli, "passwd: could not save\n");
        return 1;
    }
    purr_cli_puts(cli, "password changed\n");
    return 0;
}

static int require_admin(purr_cli_t *cli)
{
    if (purr_login_is_root() || purr_login_current()->role == PURR_ROLE_USER_ADMIN) {
        return 1;
    }
    purr_cli_puts(cli, "only an admin can do that\n");
    return 0;
}

static int cmd_useradd(purr_cli_t *cli, int argc, char **argv)
{
    if (!require_admin(cli)) {
        return 1;
    }
    if (argc < 2) {
        purr_cli_puts(cli, "usage: useradd <name> [admin|standard]\n");
        return 1;
    }
    purr_user_role_t role = PURR_ROLE_USER_STANDARD;
    if (argc > 2) {
        if (strcmp(argv[2], "admin") == 0) {
            role = PURR_ROLE_USER_ADMIN;
        } else if (strcmp(argv[2], "standard") != 0) {
            purr_cli_puts(cli, "usage: useradd <name> [admin|standard]\n");
            return 1;
        }
    }
    uint8_t uid = purr_user_next_uid(purr_login_users());
    int r = purr_user_list_add(purr_login_users(), argv[1], uid, role);
    if (r != PURR_USER_ADDED) {
        purr_cli_printf(cli, "useradd: %s\n", r == PURR_USER_EXISTS ? "already exists" :
                        r == PURR_USER_FULL ? "too many accounts" : "invalid name");
        return 1;
    }
    purr_cli_puts(cli, "set their password:\n");
    purr_console_flush();
    char pass1[64], pass2[64];
    purr_cli_puts(cli, "new password: ");
    purr_console_flush();
    read_masked_line(pass1, sizeof(pass1));
    purr_cli_puts(cli, "confirm: ");
    purr_console_flush();
    read_masked_line(pass2, sizeof(pass2));
    if (strcmp(pass1, pass2) != 0 || pass1[0] == '\0') {
        memset(pass1, 0, sizeof(pass1));
        memset(pass2, 0, sizeof(pass2));
        purr_user_list_remove(purr_login_users(), argv[1]);
        purr_cli_puts(cli, "useradd: those did not match, or were empty; not created\n");
        return 1;
    }
    uint8_t salt[PURR_SALT_LEN];
    purr_login_random_salt(salt);
    purr_shadow_set(purr_login_shadow(), argv[1], pass1, salt, PURR_PBKDF2_ITERATIONS);
    memset(pass1, 0, sizeof(pass1));
    memset(pass2, 0, sizeof(pass2));
    if (purr_login_persist() != 0) {
        purr_cli_puts(cli, "useradd: could not save\n");
        return 1;
    }
    purr_cli_printf(cli, "%s created (uid %u, %s)\n", argv[1], uid, role == PURR_ROLE_USER_ADMIN ? "admin" : "standard");
    return 0;
}

static int cmd_userdel(purr_cli_t *cli, int argc, char **argv)
{
    if (!require_admin(cli)) {
        return 1;
    }
    if (argc < 2) {
        purr_cli_puts(cli, "usage: userdel <name>\n");
        return 1;
    }
    if (strcmp(argv[1], purr_login_current()->name) == 0) {
        purr_cli_puts(cli, "userdel: cannot delete the account you are logged in as\n");
        return 1;
    }
    if (purr_user_is_last_admin(purr_login_users(), argv[1])) {
        purr_cli_puts(cli, "userdel: refusing to remove the last admin\n");
        return 1;
    }
    if (!purr_user_list_remove(purr_login_users(), argv[1])) {
        purr_cli_printf(cli, "userdel: no such user '%s'\n", argv[1]);
        return 1;
    }
    purr_shadow_list_remove(purr_login_shadow(), argv[1]);
    if (purr_login_persist() != 0) {
        purr_cli_puts(cli, "userdel: could not save\n");
        return 1;
    }
    purr_cli_printf(cli, "%s removed\n", argv[1]);
    return 0;
}

static int cmd_usermod(purr_cli_t *cli, int argc, char **argv)
{
    if (!require_admin(cli)) {
        return 1;
    }
    if (argc < 3 || (strcmp(argv[2], "admin") != 0 && strcmp(argv[2], "standard") != 0)) {
        purr_cli_puts(cli, "usage: usermod <name> admin|standard\n");
        return 1;
    }
    purr_user_t *u = purr_user_list_find(purr_login_users(), argv[1]);
    if (u == NULL) {
        purr_cli_printf(cli, "usermod: no such user '%s'\n", argv[1]);
        return 1;
    }
    purr_user_role_t role = strcmp(argv[2], "admin") == 0 ? PURR_ROLE_USER_ADMIN : PURR_ROLE_USER_STANDARD;
    if (role == PURR_ROLE_USER_STANDARD && purr_user_is_last_admin(purr_login_users(), argv[1])) {
        purr_cli_puts(cli, "usermod: refusing to demote the last admin\n");
        return 1;
    }
    u->role = role;
    if (purr_login_persist() != 0) {
        purr_cli_puts(cli, "usermod: could not save\n");
        return 1;
    }
    purr_cli_printf(cli, "%s is now %s\n", argv[1], role == PURR_ROLE_USER_ADMIN ? "admin" : "standard");
    return 0;
}

/*
 * cmd_plantmodules: temporary, stands in for a real transport (a modinstall-style URL fetch,
 * or MTP once it exists -- neither exists yet, and WiFi isn't configured on this device
 * right now anyway). Plants the real, purrstrap-built and signed "about" and "apps" modules
 * to /modules/ so the real loader picks them up on the next boot. Remove once modules have a
 * real way onto the device.
 */
static const uint8_t s_about_module[] = {
    0x52, 0x52, 0x55, 0x50, 0x01, 0xad, 0x00, 0x09, 0x00, 0x03, 0x02, 0x05,
    0x00, 0x61, 0x62, 0x6f, 0x75, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x2e, 0x31,
    0x2e, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xad, 0x00, 0x00,
    0x00, 0x18, 0x01, 0x00, 0x00, 0x03, 0x3e, 0x5d, 0x4d, 0xf7, 0x9d, 0x38,
    0x81, 0x22, 0x8d, 0x00, 0x44, 0x4b, 0x02, 0xa0, 0x5d, 0x21, 0x69, 0xd4,
    0xe4, 0x6a, 0x8d, 0xfe, 0x25, 0x32, 0xb4, 0xe2, 0x07, 0xea, 0x55, 0xc0,
    0xbe, 0x28, 0xb5, 0xf8, 0xbb, 0x4b, 0xa6, 0x7c, 0x35, 0x92, 0x69, 0xdf,
    0x4c, 0x15, 0x97, 0xff, 0x7b, 0x0f, 0xbc, 0xf6, 0xe7, 0xe8, 0x0f, 0xcb,
    0x48, 0x79, 0xb1, 0xac, 0x5e, 0xff, 0xf6, 0xf5, 0x23, 0x37, 0xbf, 0x72,
    0x1f, 0x5d, 0x65, 0xd1, 0x41, 0xfb, 0x3b, 0x8e, 0xa5, 0x83, 0x31, 0x75,
    0xfc, 0x80, 0x9c, 0x27, 0xc3, 0x16, 0xa7, 0xbd, 0x6b, 0xc9, 0x4f, 0x3c,
    0x74, 0x2e, 0x09, 0xff, 0x4b, 0x24, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00,
    0x00, 0xdc, 0x00, 0x00, 0x00, 0xe4, 0x00, 0x00, 0x00, 0xe8, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0xec, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00,
    0x00, 0xf0, 0x00, 0x00, 0x00, 0x36, 0x41, 0x00, 0x81, 0xfe, 0xff, 0xb1,
    0xfc, 0xff, 0x88, 0x08, 0xad, 0x02, 0x88, 0x08, 0x0c, 0x02, 0xe0, 0x08,
    0x00, 0x1d, 0xf0, 0x00, 0x00, 0xd8, 0x00, 0x00, 0x00, 0x36, 0x41, 0x00,
    0x81, 0xf7, 0xff, 0x29, 0x08, 0x21, 0xfd, 0xff, 0x1d, 0xf0, 0x00, 0x00,
    0x00, 0x54, 0x68, 0x69, 0x73, 0x20, 0x63, 0x6f, 0x6d, 0x6d, 0x61, 0x6e,
    0x64, 0x20, 0x69, 0x73, 0x20, 0x72, 0x75, 0x6e, 0x6e, 0x69, 0x6e, 0x67,
    0x20, 0x66, 0x72, 0x6f, 0x6d, 0x20, 0x61, 0x20, 0x72, 0x65, 0x61, 0x6c,
    0x20, 0x6d, 0x6f, 0x64, 0x75, 0x6c, 0x65, 0x2c, 0x20, 0x6c, 0x6f, 0x61,
    0x64, 0x65, 0x64, 0x20, 0x66, 0x72, 0x6f, 0x6d, 0x20, 0x4c, 0x69, 0x74,
    0x74, 0x6c, 0x65, 0x46, 0x53, 0x20, 0x61, 0x74, 0x20, 0x62, 0x6f, 0x6f,
    0x74, 0x20, 0x2d, 0x2d, 0x20, 0x6e, 0x6f, 0x74, 0x20, 0x6c, 0x69, 0x6e,
    0x6b, 0x65, 0x64, 0x20, 0x69, 0x6e, 0x74, 0x6f, 0x20, 0x74, 0x68, 0x65,
    0x20, 0x66, 0x69, 0x72, 0x6d, 0x77, 0x61, 0x72, 0x65, 0x20, 0x69, 0x6d,
    0x61, 0x67, 0x65, 0x2e, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x61, 0x62, 0x6f,
    0x75, 0x74, 0x00, 0x00, 0x00, 0x70, 0x72, 0x69, 0x6e, 0x74, 0x73, 0x20,
    0x61, 0x20, 0x6d, 0x65, 0x73, 0x73, 0x61, 0x67, 0x65, 0x20, 0x66, 0x72,
    0x6f, 0x6d, 0x20, 0x61, 0x20, 0x6c, 0x6f, 0x61, 0x64, 0x65, 0x64, 0x20,
    0x6d, 0x6f, 0x64, 0x75, 0x6c, 0x65, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00,
    0x00, 0xe4, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xa8, 0x00, 0x00,
    0x00, 0xb0, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
};

static const uint8_t s_apps_module[] = {
    0x52, 0x52, 0x55, 0x50, 0x01, 0xad, 0x00, 0x09, 0x00, 0x03, 0x02, 0x05,
    0x00, 0x61, 0x70, 0x70, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x2e, 0x31,
    0x2e, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xad, 0x00, 0x00,
    0x00, 0xe8, 0x01, 0x00, 0x00, 0x95, 0x3a, 0x0b, 0x45, 0xa0, 0xf8, 0xca,
    0x43, 0x0a, 0xa8, 0x3a, 0x39, 0x71, 0xbd, 0x74, 0xbe, 0x66, 0x91, 0x8d,
    0x61, 0xc4, 0x71, 0x1c, 0x51, 0x9c, 0xb2, 0xd6, 0x49, 0xcc, 0x2e, 0x87,
    0x3e, 0x51, 0xc8, 0x2f, 0x02, 0x3e, 0x37, 0x82, 0x2d, 0xd3, 0x22, 0x8a,
    0x6a, 0x56, 0xb3, 0x41, 0x14, 0x50, 0x3b, 0x1e, 0x9a, 0x43, 0xd9, 0x98,
    0xc6, 0xc3, 0x9b, 0x6c, 0x96, 0x10, 0x68, 0x92, 0x55, 0x55, 0x7e, 0x5d,
    0xb1, 0xad, 0x1a, 0x62, 0xbd, 0x06, 0x5b, 0xed, 0xe8, 0x2d, 0x25, 0xd7,
    0x50, 0xf8, 0x89, 0x17, 0x9a, 0x7c, 0x5f, 0x23, 0xb5, 0x86, 0xcc, 0x24,
    0xdb, 0xf5, 0x46, 0x98, 0x1e, 0xc8, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00,
    0x00, 0x0c, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00,
    0x00, 0x18, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0xc4, 0x00, 0x00,
    0x00, 0x94, 0x01, 0x00, 0x00, 0x9c, 0x01, 0x00, 0x00, 0xa0, 0x01, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0xa4, 0x01, 0x00, 0x00, 0xd8, 0x00, 0x00,
    0x00, 0xe4, 0x00, 0x00, 0x00, 0x70, 0x01, 0x00, 0x00, 0xf0, 0x00, 0x00,
    0x00, 0xa8, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x14, 0x01, 0x00,
    0x00, 0x30, 0x01, 0x00, 0x00, 0x36, 0xa1, 0x0d, 0x61, 0xfb, 0xff, 0x92,
    0xa6, 0xa0, 0x88, 0x06, 0x1a, 0x99, 0x72, 0xc1, 0x10, 0x88, 0x28, 0x29,
    0x09, 0xad, 0x07, 0xe0, 0x08, 0x00, 0x52, 0xd7, 0x06, 0x92, 0x25, 0x20,
    0xcc, 0xd9, 0x88, 0x06, 0xb1, 0xf4, 0xff, 0x88, 0x08, 0xad, 0x02, 0xe0,
    0x08, 0x00, 0x92, 0x25, 0x20, 0x2d, 0x07, 0x0c, 0x05, 0xa6, 0x19, 0x08,
    0x31, 0xeb, 0xff, 0x41, 0xec, 0xff, 0xc6, 0x02, 0x00, 0x72, 0xd7, 0x06,
    0xc2, 0x27, 0x21, 0x56, 0x4c, 0x04, 0xc6, 0x14, 0x00, 0xf2, 0x02, 0x32,
    0x81, 0xe4, 0xff, 0xb2, 0x02, 0x30, 0xa1, 0xe4, 0xff, 0x98, 0x06, 0xf0,
    0x83, 0x83, 0xb0, 0xa4, 0x83, 0xfd, 0x08, 0x82, 0xa6, 0xa0, 0xe8, 0xb2,
    0x98, 0x19, 0x1a, 0x88, 0xa9, 0x01, 0xb1, 0xe2, 0xff, 0xa8, 0x08, 0xd2,
    0xc2, 0x20, 0xcd, 0x02, 0xe0, 0xea, 0x41, 0xe0, 0x09, 0x00, 0x92, 0xd7,
    0x06, 0x92, 0x29, 0x20, 0x1b, 0x55, 0x22, 0xc2, 0x34, 0x97, 0x25, 0xc0,
    0x46, 0xec, 0xff, 0x88, 0x06, 0x92, 0xa6, 0xa0, 0x1a, 0x99, 0xb1, 0xd9,
    0xff, 0x88, 0x18, 0xa2, 0x29, 0x00, 0xe0, 0x08, 0x00, 0x0c, 0x02, 0x1d,
    0xf0, 0x90, 0x01, 0x00, 0x00, 0x36, 0x41, 0x00, 0x81, 0xd1, 0xff, 0x29,
    0x08, 0x21, 0xfd, 0xff, 0x1d, 0xf0, 0x00, 0x00, 0x00, 0x76, 0x65, 0x72,
    0x69, 0x66, 0x69, 0x65, 0x64, 0x00, 0x00, 0x00, 0x00, 0x75, 0x6e, 0x76,
    0x65, 0x72, 0x69, 0x66, 0x69, 0x65, 0x64, 0x00, 0x00, 0x20, 0x20, 0x28,
    0x77, 0x72, 0x6f, 0x6e, 0x67, 0x20, 0x63, 0x68, 0x69, 0x70, 0x29, 0x00,
    0x00, 0x6e, 0x6f, 0x20, 0x61, 0x70, 0x70, 0x73, 0x20, 0x69, 0x6e, 0x73,
    0x74, 0x61, 0x6c, 0x6c, 0x65, 0x64, 0x0a, 0x00, 0x00, 0x20, 0x20, 0x25,
    0x2d, 0x31, 0x36, 0x73, 0x20, 0x25, 0x2d, 0x31, 0x30, 0x73, 0x20, 0x25,
    0x36, 0x75, 0x4b, 0x20, 0x20, 0x25, 0x73, 0x25, 0x73, 0x0a, 0x00, 0x00,
    0x00, 0x20, 0x20, 0x28, 0x25, 0x64, 0x20, 0x61, 0x70, 0x70, 0x20, 0x66,
    0x6f, 0x6c, 0x64, 0x65, 0x72, 0x28, 0x73, 0x29, 0x20, 0x73, 0x6b, 0x69,
    0x70, 0x70, 0x65, 0x64, 0x3a, 0x20, 0x75, 0x6e, 0x72, 0x65, 0x61, 0x64,
    0x61, 0x62, 0x6c, 0x65, 0x20, 0x6f, 0x72, 0x20, 0x6e, 0x6f, 0x74, 0x20,
    0x61, 0x20, 0x76, 0x61, 0x6c, 0x69, 0x64, 0x20, 0x70, 0x61, 0x63, 0x6b,
    0x61, 0x67, 0x65, 0x29, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x61, 0x70, 0x70,
    0x73, 0x00, 0x00, 0x00, 0x00, 0x6c, 0x69, 0x73, 0x74, 0x20, 0x69, 0x6e,
    0x73, 0x74, 0x61, 0x6c, 0x6c, 0x65, 0x64, 0x20, 0x61, 0x70, 0x70, 0x73,
    0x00, 0x03, 0x00, 0x00, 0x00, 0x9c, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x74, 0x01, 0x00, 0x00, 0x7c, 0x01, 0x00, 0x00, 0x20, 0x00, 0x00,
    0x00,
};

static const uint8_t s_wifi_module[] = {
    0x52, 0x52, 0x55, 0x50, 0x01, 0xad, 0x00, 0x09, 0x00, 0x03, 0x02, 0x05,
    0x00, 0x77, 0x69, 0x66, 0x69, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x2e, 0x31,
    0x2e, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xad, 0x00, 0x00,
    0x00, 0x3c, 0x06, 0x00, 0x00, 0xb7, 0xfe, 0xdc, 0x14, 0xb6, 0x1d, 0x8b,
    0x66, 0xe9, 0xfe, 0x69, 0x3f, 0xe3, 0x25, 0xc4, 0xe3, 0xf3, 0xa2, 0xbc,
    0x3f, 0x1a, 0x12, 0xde, 0x2c, 0x8a, 0x38, 0x30, 0xd6, 0xfe, 0xd6, 0x24,
    0xbb, 0xd4, 0xa5, 0x63, 0x3e, 0x0e, 0x22, 0x21, 0x7d, 0xed, 0x9e, 0xcb,
    0x85, 0x32, 0x91, 0x1c, 0x4b, 0xed, 0xc5, 0xeb, 0xb3, 0xc8, 0x48, 0xc9,
    0x3a, 0x10, 0xe0, 0x96, 0x20, 0x23, 0x9a, 0xf0, 0x6c, 0x8b, 0x1e, 0x74,
    0xb9, 0xdc, 0x06, 0x96, 0x2c, 0x3f, 0x36, 0x5d, 0x4b, 0x1d, 0x73, 0xd9,
    0x6e, 0xa1, 0x14, 0xd2, 0xeb, 0xeb, 0xfc, 0x4b, 0x2d, 0x7a, 0x16, 0xc3,
    0x46, 0x42, 0x27, 0x9f, 0xee, 0x74, 0x03, 0x00, 0x00, 0x20, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00,
    0x00, 0x0c, 0x00, 0x00, 0x00, 0x58, 0x00, 0x00, 0x00, 0x5c, 0x00, 0x00,
    0x00, 0x60, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00,
    0x00, 0x6c, 0x00, 0x00, 0x00, 0x10, 0x01, 0x00, 0x00, 0x14, 0x01, 0x00,
    0x00, 0x18, 0x01, 0x00, 0x00, 0x1c, 0x01, 0x00, 0x00, 0x7c, 0x01, 0x00,
    0x00, 0x80, 0x01, 0x00, 0x00, 0x84, 0x01, 0x00, 0x00, 0x88, 0x01, 0x00,
    0x00, 0x8c, 0x01, 0x00, 0x00, 0x90, 0x01, 0x00, 0x00, 0x94, 0x01, 0x00,
    0x00, 0x98, 0x01, 0x00, 0x00, 0x9c, 0x01, 0x00, 0x00, 0xa0, 0x01, 0x00,
    0x00, 0xa4, 0x01, 0x00, 0x00, 0xa8, 0x01, 0x00, 0x00, 0x70, 0x03, 0x00,
    0x00, 0x88, 0x05, 0x00, 0x00, 0x90, 0x05, 0x00, 0x00, 0x94, 0x05, 0x00,
    0x00, 0x9c, 0x05, 0x00, 0x00, 0xa0, 0x05, 0x00, 0x00, 0x02, 0x00, 0x00,
    0x00, 0x98, 0x05, 0x00, 0x00, 0xa4, 0x05, 0x00, 0x00, 0xa8, 0x05, 0x00,
    0x00, 0x84, 0x03, 0x00, 0x00, 0x9c, 0x03, 0x00, 0x00, 0xac, 0x03, 0x00,
    0x00, 0x36, 0xc1, 0x00, 0x71, 0xfb, 0xff, 0xad, 0x01, 0x88, 0x07, 0x82,
    0x28, 0x07, 0xe0, 0x08, 0x00, 0x98, 0x01, 0x88, 0x07, 0xac, 0x09, 0x88,
    0x18, 0xd8, 0xa1, 0xb1, 0xf6, 0xff, 0x4b, 0xc1, 0x20, 0xa2, 0x20, 0xe0,
    0x08, 0x00, 0x88, 0x07, 0xb1, 0xf4, 0xff, 0x88, 0x18, 0xc2, 0xc1, 0x2c,
    0xad, 0x02, 0xe0, 0x08, 0x00, 0x86, 0x02, 0x00, 0x00, 0x88, 0x08, 0xb1,
    0xf0, 0xff, 0xad, 0x02, 0xe0, 0x08, 0x00, 0x0c, 0x02, 0x1d, 0xf0, 0x00,
    0x00, 0xc4, 0x03, 0x00, 0x00, 0xcc, 0x03, 0x00, 0x00, 0xd4, 0x03, 0x00,
    0x00, 0x38, 0x04, 0x00, 0x00, 0xe4, 0x03, 0x00, 0x00, 0xfc, 0x03, 0x00,
    0x00, 0x36, 0xa1, 0x06, 0x61, 0xe3, 0xff, 0xb1, 0xfa, 0xff, 0x88, 0x06,
    0xad, 0x02, 0x88, 0x08, 0x32, 0xa0, 0x00, 0xe0, 0x08, 0x00, 0x98, 0x06,
    0x82, 0xa2, 0xc0, 0x8a, 0x51, 0x98, 0x39, 0x82, 0xa3, 0x20, 0x32, 0x61,
    0xc8, 0xe2, 0xa0, 0x60, 0xdd, 0x05, 0x8a, 0xc1, 0x1c, 0x0b, 0xad, 0x01,
    0x7d, 0x02, 0xe0, 0x09, 0x00, 0xdc, 0x1a, 0x88, 0x06, 0xb1, 0xef, 0xff,
    0x88, 0x18, 0xad, 0x02, 0xcd, 0x05, 0xe0, 0x08, 0x00, 0x0c, 0x12, 0x46,
    0x14, 0x00, 0xc2, 0x21, 0xc8, 0x1c, 0x05, 0x50, 0x5c, 0x43, 0xa6, 0x1c,
    0x2c, 0x41, 0xe6, 0xff, 0x2d, 0x01, 0xe2, 0x02, 0x28, 0x98, 0x06, 0x81,
    0xe2, 0xff, 0xd8, 0x92, 0xe0, 0x84, 0x83, 0x98, 0x19, 0xb1, 0xe4, 0xff,
    0xcd, 0x02, 0x80, 0xe8, 0x20, 0xad, 0x07, 0x32, 0xc3, 0x01, 0xe0, 0x09,
    0x00, 0x22, 0xc2, 0x2c, 0x57, 0x23, 0xda, 0xc2, 0x21, 0xc8, 0xc7, 0x25,
    0x04, 0x0c, 0x02, 0x46, 0x04, 0x00, 0x88, 0x06, 0xb1, 0xdc, 0xff, 0x88,
    0x18, 0x50, 0xcc, 0xc0, 0xad, 0x07, 0xe0, 0x08, 0x00, 0x06, 0xfa, 0xff,
    0x1d, 0xf0, 0x00, 0x00, 0x00, 0x14, 0x04, 0x00, 0x00, 0x20, 0x05, 0x00,
    0x00, 0x1c, 0x04, 0x00, 0x00, 0x30, 0x04, 0x00, 0x00, 0x36, 0xe1, 0x05,
    0x61, 0xb7, 0xff, 0x1c, 0x0b, 0x88, 0x06, 0xad, 0x01, 0x88, 0x68, 0x22,
    0x61, 0xb0, 0xe0, 0x08, 0x00, 0x2d, 0x0a, 0x8c, 0xea, 0x41, 0xf7, 0xff,
    0x51, 0xf5, 0xff, 0x7d, 0x01, 0x0c, 0x03, 0xe6, 0x1a, 0x13, 0x46, 0x0c,
    0x00, 0x88, 0x06, 0xb1, 0xf3, 0xff, 0x88, 0x08, 0xa2, 0x21, 0xb0, 0xe0,
    0x08, 0x00, 0x46, 0x08, 0x00, 0x00, 0xd2, 0x07, 0x28, 0x98, 0x06, 0x8d,
    0x04, 0xd0, 0x85, 0x93, 0x98, 0x19, 0xb1, 0xed, 0xff, 0xa2, 0x21, 0xb0,
    0xcd, 0x07, 0xdd, 0x08, 0x1b, 0x33, 0xe0, 0x09, 0x00, 0x72, 0xc7, 0x2c,
    0x37, 0x92, 0xde, 0x1d, 0xf0, 0x3c, 0x04, 0x00, 0x00, 0x44, 0x04, 0x00,
    0x00, 0x4c, 0x04, 0x00, 0x00, 0x54, 0x04, 0x00, 0x00, 0x5c, 0x04, 0x00,
    0x00, 0x64, 0x04, 0x00, 0x00, 0x94, 0x04, 0x00, 0x00, 0xbc, 0x04, 0x00,
    0x00, 0xd4, 0x04, 0x00, 0x00, 0xe8, 0x04, 0x00, 0x00, 0x04, 0x05, 0x00,
    0x00, 0x24, 0x05, 0x00, 0x00, 0x36, 0x81, 0x01, 0xe6, 0x23, 0x11, 0x81,
    0x93, 0xff, 0xb1, 0xf6, 0xff, 0x88, 0x08, 0xad, 0x02, 0x88, 0x08, 0xe0,
    0x08, 0x00, 0xc6, 0x69, 0x00, 0xc8, 0x14, 0xb2, 0x0c, 0x00, 0xdc, 0x4b,
    0x86, 0x63, 0x00, 0x82, 0x09, 0x00, 0xcc, 0x48, 0xa2, 0x0d, 0x01, 0x86,
    0x05, 0x00, 0x1b, 0xdd, 0xa2, 0x0d, 0x00, 0x46, 0x02, 0x00, 0xd1, 0xe6,
    0xff, 0x9d, 0x0c, 0x8d, 0x0b, 0xa2, 0xa0, 0x73, 0x1b, 0x99, 0x87, 0x1a,
    0xdd, 0xa7, 0x98, 0x20, 0xad, 0x02, 0xa5, 0xe7, 0xff, 0x2d, 0x0a, 0x06,
    0x5c, 0x00, 0x92, 0x08, 0x00, 0xcc, 0x69, 0x82, 0x0d, 0x01, 0xac, 0x78,
    0x06, 0x06, 0x00, 0x1b, 0xdd, 0xa2, 0x0d, 0x00, 0xc6, 0x02, 0x00, 0x00,
    0x00, 0xd1, 0xdb, 0xff, 0x9d, 0x0b, 0x8d, 0x0c, 0xa2, 0xa0, 0x63, 0x1b,
    0x88, 0x97, 0x1a, 0xd9, 0xa1, 0xd8, 0xff, 0x9d, 0x0b, 0x8d, 0x0c, 0xd2,
    0xa0, 0x66, 0xc6, 0x22, 0x00, 0x81, 0x74, 0xff, 0x0b, 0x93, 0x88, 0x08,
    0x66, 0x23, 0x0e, 0x88, 0x08, 0xb1, 0xd6, 0xff, 0xad, 0x02, 0xe0, 0x08,
    0x00, 0x06, 0x49, 0x00, 0x00, 0x00, 0x31, 0xb2, 0xff, 0x26, 0x29, 0x01,
    0x38, 0x34, 0x88, 0x18, 0xc8, 0x24, 0xb1, 0xd0, 0xff, 0xad, 0x02, 0xe0,
    0x08, 0x00, 0x81, 0x68, 0xff, 0xa8, 0x24, 0x88, 0x08, 0xd2, 0xa0, 0x60,
    0x88, 0x48, 0xcd, 0x01, 0x30, 0xb3, 0x20, 0xe0, 0x08, 0x00, 0x81, 0x63,
    0xff, 0x88, 0x08, 0xdc, 0x0a, 0x88, 0x18, 0xb1, 0x7a, 0xff, 0xcd, 0x01,
    0xad, 0x02, 0xe0, 0x08, 0x00, 0x06, 0x39, 0x00, 0x00, 0x00, 0x00, 0x88,
    0x78, 0x42, 0xc1, 0x60, 0xad, 0x04, 0xe0, 0x08, 0x00, 0x81, 0x5b, 0xff,
    0xb1, 0xc1, 0xff, 0x88, 0x08, 0xad, 0x02, 0x88, 0x18, 0xc2, 0xc4, 0x2c,
    0xe0, 0x08, 0x00, 0x0c, 0x02, 0x86, 0x30, 0x00, 0x00, 0x92, 0x08, 0x00,
    0xd2, 0x0a, 0x01, 0xcc, 0x49, 0x56, 0x8d, 0x05, 0x46, 0x02, 0x00, 0x1b,
    0xaa, 0x1b, 0x88, 0x97, 0x1d, 0xea, 0xc6, 0x12, 0x00, 0x81, 0x4f, 0xff,
    0x88, 0x08, 0x66, 0x23, 0x11, 0x82, 0x28, 0x00, 0xb1, 0xb4, 0xff, 0x20,
    0xa2, 0x20, 0xe0, 0x08, 0x00, 0x06, 0x24, 0x00, 0x00, 0x00, 0x00, 0x88,
    0x58, 0xa8, 0x24, 0xe0, 0x08, 0x00, 0x81, 0x46, 0xff, 0xb1, 0xaf, 0xff,
    0x88, 0x08, 0xad, 0x02, 0x88, 0x08, 0xe0, 0x08, 0x00, 0x0c, 0x02, 0x06,
    0x1d, 0x00, 0x92, 0x08, 0x00, 0xcc, 0x89, 0xd2, 0x0a, 0x01, 0x56, 0xcd,
    0x03, 0x06, 0x07, 0x00, 0x00, 0x1b, 0xaa, 0xd2, 0x0a, 0x00, 0x46, 0x02,
    0x00, 0xa1, 0x9e, 0xff, 0x9d, 0x0b, 0x8d, 0x0c, 0xd2, 0xa0, 0x6c, 0x1b,
    0x88, 0x97, 0x1d, 0xd9, 0x86, 0x07, 0x00, 0x00, 0x00, 0xad, 0x02, 0xa5,
    0xdf, 0xff, 0xc6, 0xf1, 0xff, 0xb2, 0x08, 0x00, 0xcc, 0x6b, 0x82, 0x09,
    0x01, 0xec, 0x58, 0x06, 0x06, 0x00, 0x1b, 0x99, 0xa2, 0x09, 0x00, 0xc6,
    0x01, 0x00, 0x91, 0x92, 0xff, 0x8d, 0x0c, 0xa2, 0xa0, 0x73, 0x1b, 0x88,
    0xb7, 0x1a, 0xdd, 0xc6, 0x02, 0x00, 0x00, 0xcd, 0x0b, 0xad, 0x02, 0xa5,
    0xcb, 0xff, 0xc6, 0xe5, 0xff, 0x00, 0x81, 0x28, 0xff, 0xb1, 0x92, 0xff,
    0x88, 0x08, 0xad, 0x02, 0x88, 0x18, 0xe0, 0x08, 0x00, 0x0c, 0x12, 0x1d,
    0xf0, 0x84, 0x05, 0x00, 0x00, 0x36, 0x41, 0x00, 0x81, 0x22, 0xff, 0x29,
    0x08, 0x21, 0xfd, 0xff, 0x1d, 0xf0, 0x00, 0x00, 0x00, 0x77, 0x69, 0x66,
    0x69, 0x3a, 0x20, 0x20, 0x20, 0x20, 0x25, 0x73, 0x20, 0x28, 0x25, 0x64,
    0x20, 0x64, 0x42, 0x6d, 0x29, 0x0a, 0x00, 0x00, 0x00, 0x69, 0x70, 0x3a,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x25, 0x73, 0x0a, 0x00, 0x00, 0x00,
    0x00, 0x77, 0x69, 0x66, 0x69, 0x3a, 0x20, 0x20, 0x20, 0x20, 0x6e, 0x6f,
    0x74, 0x20, 0x63, 0x6f, 0x6e, 0x6e, 0x65, 0x63, 0x74, 0x65, 0x64, 0x0a,
    0x00, 0x6f, 0x70, 0x65, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x73, 0x65, 0x63,
    0x75, 0x72, 0x65, 0x64, 0x00, 0x73, 0x63, 0x61, 0x6e, 0x6e, 0x69, 0x6e,
    0x67, 0x2e, 0x2e, 0x2e, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x20, 0x20, 0x25,
    0x2d, 0x33, 0x32, 0x73, 0x20, 0x25, 0x34, 0x64, 0x20, 0x64, 0x42, 0x6d,
    0x20, 0x20, 0x25, 0x73, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x20, 0x20, 0x28,
    0x25, 0x64, 0x20, 0x6d, 0x6f, 0x72, 0x65, 0x20, 0x6e, 0x6f, 0x74, 0x20,
    0x73, 0x68, 0x6f, 0x77, 0x6e, 0x29, 0x0a, 0x00, 0x00, 0x28, 0x6f, 0x70,
    0x65, 0x6e, 0x29, 0x00, 0x00, 0x6e, 0x6f, 0x20, 0x73, 0x61, 0x76, 0x65,
    0x64, 0x20, 0x6e, 0x65, 0x74, 0x77, 0x6f, 0x72, 0x6b, 0x73, 0x0a, 0x00,
    0x00, 0x20, 0x20, 0x25, 0x2d, 0x33, 0x32, 0x73, 0x20, 0x25, 0x73, 0x0a,
    0x00, 0x73, 0x63, 0x61, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x63, 0x6f, 0x6e,
    0x6e, 0x65, 0x63, 0x74, 0x00, 0x66, 0x6f, 0x72, 0x67, 0x65, 0x74, 0x00,
    0x00, 0x6c, 0x69, 0x73, 0x74, 0x00, 0x00, 0x00, 0x00, 0x73, 0x74, 0x61,
    0x74, 0x75, 0x73, 0x00, 0x00, 0x75, 0x73, 0x61, 0x67, 0x65, 0x3a, 0x20,
    0x77, 0x69, 0x66, 0x69, 0x20, 0x73, 0x63, 0x61, 0x6e, 0x7c, 0x63, 0x6f,
    0x6e, 0x6e, 0x65, 0x63, 0x74, 0x7c, 0x66, 0x6f, 0x72, 0x67, 0x65, 0x74,
    0x7c, 0x6c, 0x69, 0x73, 0x74, 0x7c, 0x73, 0x74, 0x61, 0x74, 0x75, 0x73,
    0x0a, 0x00, 0x00, 0x00, 0x00, 0x75, 0x73, 0x61, 0x67, 0x65, 0x3a, 0x20,
    0x77, 0x69, 0x66, 0x69, 0x20, 0x63, 0x6f, 0x6e, 0x6e, 0x65, 0x63, 0x74,
    0x20, 0x3c, 0x73, 0x73, 0x69, 0x64, 0x3e, 0x20, 0x5b, 0x70, 0x61, 0x73,
    0x73, 0x77, 0x6f, 0x72, 0x64, 0x5d, 0x0a, 0x00, 0x00, 0x63, 0x6f, 0x6e,
    0x6e, 0x65, 0x63, 0x74, 0x69, 0x6e, 0x67, 0x20, 0x74, 0x6f, 0x20, 0x25,
    0x73, 0x2e, 0x2e, 0x2e, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x63, 0x6f, 0x6e,
    0x6e, 0x65, 0x63, 0x74, 0x65, 0x64, 0x2c, 0x20, 0x69, 0x70, 0x20, 0x25,
    0x73, 0x0a, 0x00, 0x00, 0x00, 0x75, 0x73, 0x61, 0x67, 0x65, 0x3a, 0x20,
    0x77, 0x69, 0x66, 0x69, 0x20, 0x66, 0x6f, 0x72, 0x67, 0x65, 0x74, 0x20,
    0x3c, 0x73, 0x73, 0x69, 0x64, 0x3e, 0x0a, 0x00, 0x00, 0x66, 0x6f, 0x72,
    0x67, 0x6f, 0x74, 0x74, 0x65, 0x6e, 0x20, 0x28, 0x69, 0x66, 0x20, 0x69,
    0x74, 0x20, 0x77, 0x61, 0x73, 0x20, 0x73, 0x61, 0x76, 0x65, 0x64, 0x29,
    0x0a, 0x00, 0x00, 0x00, 0x00, 0x77, 0x69, 0x66, 0x69, 0x3a, 0x20, 0x75,
    0x6e, 0x6b, 0x6e, 0x6f, 0x77, 0x6e, 0x20, 0x73, 0x75, 0x62, 0x63, 0x6f,
    0x6d, 0x6d, 0x61, 0x6e, 0x64, 0x20, 0x27, 0x25, 0x73, 0x27, 0x0a, 0x00,
    0x00, 0x77, 0x69, 0x66, 0x69, 0x00, 0x00, 0x00, 0x00, 0x73, 0x63, 0x61,
    0x6e, 0x7c, 0x63, 0x6f, 0x6e, 0x6e, 0x65, 0x63, 0x74, 0x7c, 0x66, 0x6f,
    0x72, 0x67, 0x65, 0x74, 0x7c, 0x6c, 0x69, 0x73, 0x74, 0x7c, 0x73, 0x74,
    0x61, 0x74, 0x75, 0x73, 0x00, 0x6e, 0x65, 0x74, 0x00, 0x63, 0x6f, 0x6e,
    0x6e, 0x65, 0x63, 0x74, 0x69, 0x6f, 0x6e, 0x20, 0x73, 0x74, 0x61, 0x74,
    0x75, 0x73, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x90, 0x05, 0x00,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x44, 0x05, 0x00, 0x00, 0x4c, 0x05, 0x00,
    0x00, 0xac, 0x01, 0x00, 0x00, 0x6c, 0x05, 0x00, 0x00, 0x70, 0x05, 0x00,
    0x00, 0x10, 0x00, 0x00, 0x00,
};

typedef struct {
    const char *filename;
    const uint8_t *data;
    uint32_t len;
} temp_module_asset_t;

static const temp_module_asset_t s_temp_modules[] = {
    {"about.cat", s_about_module, sizeof(s_about_module)},
    {"apps.cat", s_apps_module, sizeof(s_apps_module)},
    {"wifi.cat", s_wifi_module, sizeof(s_wifi_module)},
};

static int cmd_plantmodules(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!need_fs(cli)) return 1;
    if (purr_fs_mkdir(&s_fs, PURR_MODULES_DIR) < 0) {
        /* already exists is fine; anything else, the write below will fail too and say why */
    }
    for (size_t i = 0; i < sizeof(s_temp_modules) / sizeof(s_temp_modules[0]); i++) {
        char path[64];
        snprintf(path, sizeof(path), "%s/%s", PURR_MODULES_DIR, s_temp_modules[i].filename);
        int e = purr_fs_write(&s_fs, path, s_temp_modules[i].data, s_temp_modules[i].len);
        if (e < 0) {
            fail(cli, path, e);
            return 1;
        }
        purr_cli_printf(cli, "planted %s (%u bytes)\n", path, (unsigned)s_temp_modules[i].len);
    }
    purr_cli_puts(cli, "reboot to load them.\n");
    return 0;
}

static const purr_cmd_t s_cmds[] = {
    {"help",    "list the commands",             purr_cli_cmd_help},
    {"version", "show the system and profile",   cmd_version},
    {"info",    "board, chip and display",       cmd_info},
    {"mem",     "free memory",                   cmd_mem},
    {"uptime",  "time since power on",           cmd_uptime},
    {"parts",   "the partition table",           cmd_parts},
    {"ls",      "list a directory",              cmd_ls},
    {"cat",     "show a file",                   cmd_cat},
    {"mkdir",   "make a directory",              cmd_mkdir},
    {"rm",      "remove a file or empty dir",    cmd_rm},
    {"mv",      "rename or move",                cmd_mv},
    {"write",   "write text to a file",          cmd_write},
    {"df",      "filesystem space",              cmd_df},
    {"format",  "erase and create the fs",       cmd_format},
    {"appinfo",    "show one app's details",        cmd_appinfo},
    {"appinstall", "install a .cat package",        cmd_appinstall},
    {"appremove",  "remove an app",                 cmd_appremove},
    {"appformat",  "erase and create the apps fs",  cmd_appformat},
    {"netinstall", "install [component] over the network", cmd_net_install},
    {"echo",    "print the arguments",           cmd_echo},
    {"clear",   "clear the screen",              cmd_clear},
    {"reboot",  "restart (or recovery|loader)",  cmd_reboot},
    {"purrcfg", "show the boot config",          cmd_purrcfg},
    {"whoami",  "the logged-in user",            cmd_whoami},
    {"id",      "uid, role and root state",      cmd_id},
    {"su",      "become root (own password)",    cmd_su},
    {"passwd",  "change a password",             cmd_passwd},
    {"useradd", "add an account (admin only)",   cmd_useradd},
    {"userdel", "remove an account (admin only)", cmd_userdel},
    {"usermod", "change a role (admin only)",    cmd_usermod},
    {"logout",  "end the session",               cmd_logout},
    {"plantmodules", "temporary: plant the real modules for testing", cmd_plantmodules},
};

/* ---------------------------------------------------------------- Modules (real) */
/*
 * The first real module (Modules/SPEC.md): loads every `.cat` file in /modules on the root
 * filesystem at boot: verify -> parse layout -> relocate (two passes, one per base -- section
 * 4) -> map executable -> call entry, merging each module's exported commands into the
 * shell's dispatch table. This is permanent: it is how a module actually becomes usable.
 */
#define PURR_MODULE_MAX 8
#define PURR_MODULE_READ_CAP (128 * 1024)

typedef struct {
    uint8_t *databuf;                  /* kept forever: modules are never unloaded (yet) */
    const purr_module_table_t *table;
} loaded_module_t;

static loaded_module_t s_loaded_modules[PURR_MODULE_MAX];
static int s_loaded_module_count = 0;

static void core_table_puts(purr_cli_t *cli, const char *s)
{
    purr_cli_puts(cli, s);
}

static void core_table_printf(purr_cli_t *cli, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[160];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    purr_cli_puts(cli, buf);
}

/* What Modules/appmanager/apps_module.c's `apps` command calls through the core table
 * instead of touching purr_fs/purr_appmgr directly -- the exact same scan cmd_apps used to
 * do inline, before it moved into a module. A module never gets a purr_fs_t or a key bag. */
static void core_table_apps_scan(purr_app_registry_t *out_reg)
{
    if (!purr_fs_mounted(&s_fs)) {
        memset(out_reg, 0, sizeof(*out_reg));
        return;
    }
    char root[64];
    user_apps_root(root, sizeof(root), purr_login_current()->name);
    purr_cfg_t cfg;
    purr_appmgr_env_t env = apps_env(&cfg);
    purr_appmgr_scan(&s_fs, root, &env, app_scratch(), APP_SCRATCH_CAP, out_reg);
}

/* Words a person reads, not an ESP-IDF error name -- moved here from the old inline
 * cmd_wifi_connect, since a module can't see a wifi_err_reason_t at all. */
static const char *net_reason_text(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "network not found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "likely a wrong password";
    default:
        return NULL;
    }
}

static int core_table_net_scan(purr_module_net_ap_t *out, int max, int *out_n, char *err, size_t err_cap)
{
    purr_net_ap_t aps[16];
    int cap = max < 16 ? max : 16;
    int n = 0;
    esp_err_t e = purr_net_scan(aps, cap, &n);
    if (e != ESP_OK) {
        snprintf(err, err_cap, "scan failed: %s", esp_err_to_name(e));
        return 0;
    }
    int shown = n < cap ? n : cap;
    for (int i = 0; i < shown; i++) {
        snprintf(out[i].ssid, sizeof(out[i].ssid), "%s", aps[i].ssid);
        out[i].rssi = aps[i].rssi;
        out[i].open = aps[i].open;
    }
    *out_n = n;
    return 1;
}

static int core_table_net_connect(const char *ssid, const char *pass, char *err, size_t err_cap)
{
    esp_err_t e = purr_net_connect(ssid, pass, 15000);
    if (e != ESP_OK) {
        if (e == ESP_ERR_TIMEOUT) {
            const char *why = net_reason_text(purr_net_last_disconnect_reason());
            snprintf(err, err_cap, "connection timed out. Could not connect.%s%s",
                    why ? " " : "", why ? why : "");
        } else {
            snprintf(err, err_cap, "could not connect (%s).", esp_err_to_name(e));
        }
        return 0;
    }
    return 1;
}

static void core_table_net_forget(const char *ssid)
{
    purr_net_forget(ssid);
}

static int core_table_net_saved(purr_module_net_ap_t *out, int max)
{
    purr_net_ap_t saved[PURR_WIFI_MAX_SAVED];
    int cap = max < PURR_WIFI_MAX_SAVED ? max : PURR_WIFI_MAX_SAVED;
    int n = purr_net_saved(saved, cap);
    int shown = n < cap ? n : cap;
    for (int i = 0; i < shown; i++) {
        snprintf(out[i].ssid, sizeof(out[i].ssid), "%s", saved[i].ssid);
        out[i].rssi = saved[i].rssi;
        out[i].open = saved[i].open;
    }
    return shown;
}

static void core_table_net_status(purr_module_net_status_t *out)
{
    purr_net_status_t st;
    purr_net_status(&st);
    out->connected = st.connected;
    snprintf(out->ssid, sizeof(out->ssid), "%s", st.ssid);
    out->rssi = st.rssi;
    snprintf(out->ip, sizeof(out->ip), "%s", st.ip);
}

static const purr_core_table_t s_core_table = {
    .puts = core_table_puts,
    .printf = core_table_printf,
    .apps_scan = core_table_apps_scan,
    .net_scan = core_table_net_scan,
    .net_connect = core_table_net_connect,
    .net_forget = core_table_net_forget,
    .net_saved = core_table_net_saved,
    .net_status = core_table_net_status,
};

typedef struct {
    char names[PURR_MODULE_MAX][48];
    int is_dir[PURR_MODULE_MAX];
    int count;
} module_listing_t;

static void collect_module_row(void *ctx, const char *name, int is_dir, uint32_t size)
{
    (void)size;
    module_listing_t *l = ctx;
    if (l->count < PURR_MODULE_MAX) {
        strncpy(l->names[l->count], name, sizeof(l->names[l->count]) - 1);
        l->names[l->count][sizeof(l->names[l->count]) - 1] = '\0';
        l->is_dir[l->count] = is_dir;
        l->count++;
    }
}

static int load_one_module_file(const char *path)
{
    uint8_t *rb = malloc(PURR_MODULE_READ_CAP);
    if (rb == NULL) {
        return -1;
    }
    appinstall_read_ctx_t rc = {rb, PURR_MODULE_READ_CAP, 0};
    if (purr_fs_read(&s_fs, path, appinstall_accumulate, &rc) < 0) {
        free(rb);
        return -1;
    }

    purr_cfg_t cfg;
    purr_flash_t fl;
    if (purr_cfgstore_open(&fl) != 0 || purr_cfg_load(&fl, &cfg, NULL) < 0) {
        purr_cfg_defaults(&cfg);
    }
    purr_keybag_t bag;
    purr_keybag_build(&bag, purr_default_keys, purr_default_keys_count, &cfg);
    purr_verify_env_t env = {.chip_id = NET_INSTALL_CHIP, .bag = &bag, .crypto = &purr_crypto_mbedtls};
    purr_mem_read_ctx_t mrc = {rb, rc.len};
    purr_image_header_t hdr;
    purr_verify_result_t vr = purr_image_verify(&env, purr_mem_read, &mrc, rc.len, &hdr);
    if (vr != PURR_V_OK || hdr.image_type != PURR_IMG_MODULE) {
        ESP_LOGW("modules", "%s: %s", path, vr != PURR_V_OK ? purr_verify_name(vr) : "not a module image");
        free(rb);
        return -1;
    }
    if ((uint64_t)hdr.payload_offset + hdr.payload_size > rc.len) {
        ESP_LOGW("modules", "%s: payload runs past the file", path);
        free(rb);
        return -1;
    }

    const uint8_t *payload = rb + hdr.payload_offset;
    purr_module_layout_t layout;
    purr_module_layout_result_t lr = purr_module_parse_layout(payload, hdr.payload_size, &layout);
    if (lr != PURR_MOD_LAYOUT_OK) {
        ESP_LOGW("modules", "%s: bad layout: %s", path, purr_module_layout_result_name(lr));
        free(rb);
        return -1;
    }

    /* Two relocation passes against two different bases, not one (Modules/SPEC.md section
     * 4, revised): a word holding a FUNCTION's address (the entry point, or -- found the
     * hard way, on real hardware -- a function pointer sitting in an exported table) must
     * resolve to wherever the module ends up mapped EXECUTABLE, since that's the only alias
     * anything will ever be called through. A word holding an OBJECT's address (a global,
     * written to or just read; a string; a const table) must resolve to the plain WRITABLE
     * copy instead: the executable mapping is exec+read only, not writable, so a global
     * relocated to it can be read but never written -- a first version of this relocated
     * everything the same way and a module with one writable global crashed the device with
     * a hardware cache-safety trap the instant it tried to initialize that global. purrstrap
     * tells the two apart using the compiler's own FUNC/OBJECT symbol types (readelf, not
     * nm's section letters), not anything guessed here. */
    uint32_t *data_offsets = NULL;
    if (layout.data_reloc_count > 0) {
        data_offsets = malloc(layout.data_reloc_count * sizeof(uint32_t));
        if (data_offsets == NULL) {
            free(rb);
            return -1;
        }
        purr_module_read_data_relocs(payload, &layout, data_offsets);
    }
    uint32_t *code_offsets = NULL;
    if (layout.code_reloc_count > 0) {
        code_offsets = malloc(layout.code_reloc_count * sizeof(uint32_t));
        if (code_offsets == NULL) {
            free(data_offsets);
            free(rb);
            return -1;
        }
        purr_module_read_code_relocs(payload, &layout, code_offsets);
    }

    size_t want = CONFIG_MMU_PAGE_SIZE;
    if (layout.code_size > want) {
        ESP_LOGW("modules", "%s: too big for one page (%u bytes)", path, (unsigned)layout.code_size);
        free(data_offsets);
        free(code_offsets);
        free(rb);
        return -1;
    }
    uint8_t *databuf = heap_caps_aligned_alloc(CONFIG_MMU_PAGE_SIZE, want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (databuf == NULL) {
        free(data_offsets);
        free(code_offsets);
        free(rb);
        return -1;
    }
    memset(databuf, 0, want);
    memcpy(databuf, payload + layout.code_offset, layout.code_size);

    /* Data relocations only need databuf's own address, available immediately. */
    purr_reloc_result_t rr = purr_relocate(databuf, want, data_offsets, layout.data_reloc_count,
                                          (uint32_t)(uintptr_t)databuf);
    free(data_offsets);
    if (rr != PURR_RELOC_OK) {
        ESP_LOGW("modules", "%s: data relocation failed: %s", path, purr_reloc_result_name(rr));
        free(code_offsets);
        free(rb);
        free(databuf);
        return -1;
    }

    /* Code relocations need the executable alias's address, which only exists after mapping. */
    esp_err_t ee = esp_cache_msync(databuf, want, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_paddr_t paddr = 0;
    mmu_target_t target = 0;
    if (ee == ESP_OK) ee = esp_mmu_vaddr_to_paddr(databuf, &paddr, &target);
    void *exec_ptr = NULL;
    if (ee == ESP_OK) {
        ee = esp_mmu_map(paddr, want, MMU_TARGET_PSRAM0, MMU_MEM_CAP_EXEC | MMU_MEM_CAP_READ,
                         ESP_MMU_MMAP_FLAG_PADDR_SHARED, &exec_ptr);
    }
    if (ee != ESP_OK) {
        ESP_LOGW("modules", "%s: PSRAM exec mapping failed: %s", path, esp_err_to_name(ee));
        free(code_offsets);
        free(rb);
        free(databuf);
        return -1;
    }

    rr = purr_relocate(databuf, want, code_offsets, layout.code_reloc_count,
                       (uint32_t)(uintptr_t)exec_ptr);
    free(code_offsets);
    free(rb);
    if (rr != PURR_RELOC_OK) {
        ESP_LOGW("modules", "%s: code relocation failed: %s", path, purr_reloc_result_name(rr));
        esp_mmu_unmap(exec_ptr);
        free(databuf);
        return -1;
    }

    ee = esp_cache_msync(databuf, want, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    if (ee == ESP_OK) {
        ee = esp_cache_msync(exec_ptr, want,
                            ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    }
    if (ee != ESP_OK) {
        ESP_LOGW("modules", "%s: post-relocation cache sync failed: %s", path, esp_err_to_name(ee));
        esp_mmu_unmap(exec_ptr);
        free(databuf);
        return -1;
    }

    if (s_loaded_module_count >= PURR_MODULE_MAX) {
        ESP_LOGW("modules", "%s: too many modules already loaded", path);
        esp_mmu_unmap(exec_ptr);
        free(databuf);
        return -1;
    }

    purr_module_entry_fn entry = (purr_module_entry_fn)(uintptr_t)((uint8_t *)exec_ptr + layout.entry_offset);
    const purr_module_table_t *table = entry(&s_core_table);
    if (table == NULL || table->abi_version != PURR_MODULE_ABI_VERSION) {
        ESP_LOGW("modules", "%s: bad module table (abi mismatch or NULL)", path);
        esp_mmu_unmap(exec_ptr);
        free(databuf);
        return -1;
    }

    s_loaded_modules[s_loaded_module_count].databuf = databuf;
    s_loaded_modules[s_loaded_module_count].table = table;
    s_loaded_module_count++;
    ESP_LOGI("modules", "%s: loaded, %u command(s)", path, (unsigned)table->cmd_count);
    return 0;
}

static void load_modules_from(const char *dir)
{
    module_listing_t l = {.count = 0};
    if (purr_fs_list(&s_fs, dir, collect_module_row, &l) != 0) {
        return; /* no modules folder yet: nothing to load, not an error */
    }
    for (int i = 0; i < l.count; i++) {
        if (l.is_dir[i]) {
            continue;
        }
        size_t n = strlen(l.names[i]);
        if (n < 4 || strcmp(l.names[i] + n - 4, ".cat") != 0) {
            continue;
        }
        char path[80];
        snprintf(path, sizeof(path), "%s/%s", dir, l.names[i]);
        load_one_module_file(path);
    }
}

const purr_cmd_t *purr_commands(int *count)
{
    static purr_cmd_t *combined = NULL;
    static int combined_count = 0;

    if (combined == NULL) {
        int base_n = (int)(sizeof(s_cmds) / sizeof(s_cmds[0]));
        if (purr_fs_mounted(&s_fs)) {
            load_modules_from(PURR_MODULES_DIR);
        }
        int total = base_n;
        for (int i = 0; i < s_loaded_module_count; i++) {
            total += (int)s_loaded_modules[i].table->cmd_count;
        }
        combined = malloc(sizeof(purr_cmd_t) * (size_t)total);
        if (combined == NULL) {
            *count = base_n;
            return s_cmds;             /* out of memory: fall back to just the built-ins */
        }
        memcpy(combined, s_cmds, sizeof(purr_cmd_t) * (size_t)base_n);
        int pos = base_n;
        for (int i = 0; i < s_loaded_module_count; i++) {
            const purr_module_table_t *t = s_loaded_modules[i].table;
            memcpy(combined + pos, t->cmds, sizeof(purr_cmd_t) * t->cmd_count);
            pos += (int)t->cmd_count;
        }
        combined_count = total;
    }
    *count = combined_count;
    return combined;
}
