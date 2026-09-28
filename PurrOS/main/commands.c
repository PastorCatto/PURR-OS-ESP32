#include "commands.h"

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
#define PURR_MODULES_DIR "/modules"

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

static purr_fs_t s_apps_fs;
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

static int need_apps_fs(purr_cli_t *cli)
{
    if (purr_fs_mounted(&s_apps_fs)) {
        return 1;
    }
    purr_cli_puts(cli, "no apps filesystem mounted (run: appformat --yes)\n");
    return 0;
}

void purr_apps_setup(purr_cli_t *cli)
{
    purr_bd_t bd;
    if (purr_fs_flash_bd("apps", &bd) != 0) {
        purr_cli_puts(cli, "apps: no such partition\n");
        return;
    }
    int e = purr_fs_mount(&s_apps_fs, &bd);
    if (e == 0) {
        purr_appmgr_recover(&s_apps_fs);
        uint32_t used = 0, total = 0;
        purr_fs_usage(&s_apps_fs, &used, &total);
        purr_cli_printf(cli, "apps: mounted, %uK of %uK used\n", (unsigned)(used * 4), (unsigned)(total * 4));
    } else {
        purr_cli_printf(cli, "apps: not mounted (%s)\nrun: appformat --yes\n", purr_fs_strerror(e));
    }
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

static int cmd_apps(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!need_apps_fs(cli)) return 1;
    purr_cfg_t cfg;
    purr_appmgr_env_t env = apps_env(&cfg);
    purr_app_registry_t reg;
    purr_appmgr_scan(&s_apps_fs, &env, app_scratch(), APP_SCRATCH_CAP, &reg);
    if (reg.count == 0) {
        purr_cli_puts(cli, "no apps installed\n");
    }
    for (int i = 0; i < reg.count; i++) {
        const purr_app_entry_t *e = &reg.apps[i];
        purr_cli_printf(cli, "  %-16s %-10s %6uK  %s%s\n", e->name, e->version, (unsigned)(e->size / 1024),
                        e->verified ? "verified" : "unverified", e->chip_ok ? "" : "  (wrong chip)");
    }
    if (reg.dropped) {
        purr_cli_printf(cli, "  (%d app folder(s) skipped: unreadable or not a valid package)\n", reg.dropped);
    }
    return 0;
}

static int cmd_appinfo(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) {
        purr_cli_puts(cli, "usage: appinfo <name>\n");
        return 1;
    }
    if (!need_apps_fs(cli)) return 1;
    purr_cfg_t cfg;
    purr_appmgr_env_t env = apps_env(&cfg);
    purr_app_registry_t reg;
    purr_appmgr_scan(&s_apps_fs, &env, app_scratch(), APP_SCRATCH_CAP, &reg);
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
    if (!need_apps_fs(cli)) return 1;
    if (!is_url(argv[1]) && !need_fs(cli)) return 1;

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
    purr_appmgr_scan(&s_apps_fs, &env, scratch, APP_SCRATCH_CAP, &reg);

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

    purr_app_result_t r = purr_appmgr_add(&s_apps_fs, &env, &cfg, &reg, scratch, len);
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
    if (!need_apps_fs(cli)) return 1;
    if (purr_appmgr_remove(&s_apps_fs, argv[1]) != 0) {
        purr_cli_printf(cli, "appremove: no such app '%s'\n", argv[1]);
        return 1;
    }
    purr_cli_printf(cli, "%s removed\n", argv[1]);
    return 0;
}

static int cmd_appformat(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "--yes") != 0) {
        purr_cli_puts(cli, "this erases every installed app.\nrun: appformat --yes\n");
        return 1;
    }
    purr_bd_t bd;
    if (purr_fs_flash_bd("apps", &bd) != 0) {
        purr_cli_puts(cli, "apps: no such partition\n");
        return 1;
    }
    purr_fs_unmount(&s_apps_fs);
    int e = purr_fs_format(&s_apps_fs, &bd);
    if (e < 0) { fail(cli, "appformat", e); return 1; }
    purr_cli_puts(cli, "done.\n");
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

static int cmd_net(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    purr_net_status_t st;
    purr_net_status(&st);
    if (st.connected) {
        purr_cli_printf(cli, "wifi:    %s (%d dBm)\n", st.ssid, st.rssi);
        purr_cli_printf(cli, "ip:      %s\n", st.ip);
    } else {
        purr_cli_puts(cli, "wifi:    not connected\n");
    }
    return 0;
}

static int cmd_wifi_scan(purr_cli_t *cli)
{
    purr_cli_puts(cli, "scanning...\n");
    purr_console_flush();
    purr_net_ap_t aps[16];
    int n = 0;
    esp_err_t e = purr_net_scan(aps, 16, &n);
    if (e != ESP_OK) {
        purr_cli_printf(cli, "scan failed: %s\n", esp_err_to_name(e));
        return 1;
    }
    int shown = n > 16 ? 16 : n;
    for (int i = 0; i < shown; i++) {
        purr_cli_printf(cli, "  %-32s %4d dBm  %s\n", aps[i].ssid, aps[i].rssi,
                        aps[i].open ? "open" : "secured");
    }
    if (n > shown) {
        purr_cli_printf(cli, "  (%d more not shown)\n", n - shown);
    }
    return 0;
}

/* Words a person reads, not an ESP-IDF error name. */
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

static void print_connect_error(purr_cli_t *cli, esp_err_t e)
{
    const char *why = e == ESP_ERR_TIMEOUT ? net_reason_text(purr_net_last_disconnect_reason()) : NULL;
    if (e == ESP_ERR_TIMEOUT) {
        purr_cli_puts(cli, "connection timed out. Could not connect.");
    } else {
        purr_cli_printf(cli, "could not connect (%s).", esp_err_to_name(e));
    }
    purr_cli_printf(cli, why ? " (%s)\n" : "\n", why ? why : "");
}

static int cmd_wifi_connect(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) {
        purr_cli_puts(cli, "usage: wifi connect <ssid> [password]\n");
        return 1;
    }
    const char *pass = argc > 2 ? argv[2] : "";
    purr_cli_printf(cli, "connecting to %s...\n", argv[1]);
    purr_console_flush();
    esp_err_t e = purr_net_connect(argv[1], pass, 15000);
    if (e != ESP_OK) {
        print_connect_error(cli, e);
        return 1;
    }
    purr_net_status_t st;
    purr_net_status(&st);
    purr_cli_printf(cli, "connected, ip %s\n", st.ip);
    return 0;
}

static int cmd_wifi_forget(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) {
        purr_cli_puts(cli, "usage: wifi forget <ssid>\n");
        return 1;
    }
    purr_net_forget(argv[1]);
    purr_cli_puts(cli, "forgotten (if it was saved)\n");
    return 0;
}

static int cmd_wifi_list(purr_cli_t *cli)
{
    purr_net_ap_t saved[PURR_WIFI_MAX_SAVED];
    int n = purr_net_saved(saved, PURR_WIFI_MAX_SAVED);
    if (n == 0) {
        purr_cli_puts(cli, "no saved networks\n");
        return 0;
    }
    for (int i = 0; i < n; i++) {
        purr_cli_printf(cli, "  %-32s %s\n", saved[i].ssid, saved[i].open ? "(open)" : "");
    }
    return 0;
}

static int cmd_wifi(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) {
        purr_cli_puts(cli, "usage: wifi scan|connect|forget|list|status\n");
        return 1;
    }
    if (strcmp(argv[1], "scan") == 0) return cmd_wifi_scan(cli);
    if (strcmp(argv[1], "connect") == 0) return cmd_wifi_connect(cli, argc - 1, argv + 1);
    if (strcmp(argv[1], "forget") == 0) return cmd_wifi_forget(cli, argc - 1, argv + 1);
    if (strcmp(argv[1], "list") == 0) return cmd_wifi_list(cli);
    if (strcmp(argv[1], "status") == 0) return cmd_net(cli, 0, NULL);
    purr_cli_printf(cli, "wifi: unknown subcommand '%s'\n", argv[1]);
    return 1;
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
    {"apps",       "list installed apps",           cmd_apps},
    {"appinfo",    "show one app's details",        cmd_appinfo},
    {"appinstall", "install a .cat package",        cmd_appinstall},
    {"appremove",  "remove an app",                 cmd_appremove},
    {"appformat",  "erase and create the apps fs",  cmd_appformat},
    {"wifi",    "scan|connect|forget|list|status", cmd_wifi},
    {"net",       "connection status",             cmd_net},
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

static const purr_core_table_t s_core_table = {.puts = core_table_puts};

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
