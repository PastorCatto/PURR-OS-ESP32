/*
 * The recovery loader (RecoveryLoader/SPEC.md): an internet recovery for KittenOS. Connects
 * to Wi-Fi, fetches the recovery manifest, downloads the newest KittenOS for this board,
 * verifies it, and writes it into the kittenos partition. No general shell, no filesystem, no
 * apps; only a small Wi-Fi diagnostic shell (wifi, net, exit), offered by a keypress at
 * startup, for checking the connection before or without doing a recovery.
 */
#include "recovery_loader.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "purr_abi.h"
#include "purr_cfgstore.h"
#include "purr_cli.h"
#include "purr_console.h"
#include "purr_crypto_mbedtls.h"
#include "purr_fetch.h"
#include "purr_kernel.h"
#include "purr_manifest.h"
#include "purr_net.h"
#include "purr_netrec.h"
#include "purr_util.h"
#include "purr_verify.h"
#include "purr_wifi.h"

static const char *TAG = "loader";

extern const purr_key_t purr_default_keys[];
extern const size_t purr_default_keys_count;

#if CONFIG_IDF_TARGET_ESP32S3
#define THIS_CHIP PURR_CHIP_ESP32S3
#else
#define THIS_CHIP PURR_CHIP_ESP32
#endif

#define MAX_IMAGE_SIZE (2 * 1024 * 1024)   /* generous; the kittenos partition is smaller */
#define MAX_MANIFEST_SIZE (32 * 1024)

/*
 * Compiled into every profile (main's SRCS is not per-profile), but only meaningful in the
 * minimal one: CONFIG_PURR_RECOVERY_MANIFEST_URL only exists there (its Kconfig entry
 * depends on PURR_PROFILE_MINIMAL), and the helpers below exist only to serve it.
 */
#if CONFIG_PURR_PROFILE_MINIMAL

/* ------------------------------------------------------------ small UI helpers */

static void console_puts(const char *s)
{
    while (*s) {
        purr_console_put(NULL, *s++);
    }
}

static void status(const char *line)
{
    ESP_LOGI(TAG, "%s", line);
    console_puts(line);
    console_puts("\n");
    purr_console_flush();
}

static void statusf(const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    status(buf);
}

static void halt(const char *why)
{
    status(why);
    status("halted. Power-cycle to try again, or use the boot menu.");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* A line of typed input: enter finishes, backspace edits, other control bytes are ignored.
 * No shell, so this is written by hand rather than reusing purr_cli's line editor. */
static void read_line(char *buf, size_t cap, int mask)
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
            console_puts("\b \b");
            purr_console_flush();
            continue;
        }
        if (c >= 0x20 && c <= 0x7E && len < cap - 1) {
            buf[len++] = c;
            char echo[2] = {mask ? '*' : c, 0};
            console_puts(echo);
            purr_console_flush();
        }
    }
    buf[len] = '\0';
    console_puts("\n");
    purr_console_flush();
}

/* ------------------------------------------------------------ the Wi-Fi diagnostic shell */

static int s_shell_exit;

static int cmd_wifi_scan(purr_cli_t *cli)
{
    purr_cli_puts(cli, "scanning...\n");
    purr_console_flush();
    purr_net_ap_t aps[16];
    int n = 0;
    if (purr_net_scan(aps, 16, &n) != ESP_OK) {
        purr_cli_puts(cli, "scan failed\n");
        return 1;
    }
    int shown = n > 16 ? 16 : n;
    for (int i = 0; i < shown; i++) {
        purr_cli_printf(cli, "  %-32s %4d dBm  %s\n", aps[i].ssid, aps[i].rssi,
                        aps[i].open ? "open" : "secured");
    }
    return 0;
}

static int cmd_wifi(purr_cli_t *cli, int argc, char **argv)
{
    if (argc < 2) {
        purr_cli_puts(cli, "usage: wifi scan|connect|forget|list\n");
        return 1;
    }
    if (strcmp(argv[1], "scan") == 0) {
        return cmd_wifi_scan(cli);
    }
    if (strcmp(argv[1], "connect") == 0) {
        if (argc < 3) {
            purr_cli_puts(cli, "usage: wifi connect <ssid> [password]\n");
            return 1;
        }
        purr_cli_printf(cli, "connecting to %s...\n", argv[2]);
        purr_console_flush();
        esp_err_t e = purr_net_connect(argv[2], argc > 3 ? argv[3] : "", 15000);
        purr_cli_printf(cli, e == ESP_OK ? "connected\n" : "failed: %s\n", esp_err_to_name(e));
        return e == ESP_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "forget") == 0) {
        if (argc < 3) {
            purr_cli_puts(cli, "usage: wifi forget <ssid>\n");
            return 1;
        }
        purr_net_forget(argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "list") == 0) {
        purr_net_ap_t saved[PURR_WIFI_MAX_SAVED];
        int n = purr_net_saved(saved, PURR_WIFI_MAX_SAVED);
        if (n == 0) {
            purr_cli_puts(cli, "no saved networks\n");
        }
        for (int i = 0; i < n; i++) {
            purr_cli_printf(cli, "  %s\n", saved[i].ssid);
        }
        return 0;
    }
    purr_cli_printf(cli, "wifi: unknown subcommand '%s'\n", argv[1]);
    return 1;
}

static int cmd_net(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    purr_net_status_t st;
    purr_net_status(&st);
    if (st.connected) {
        purr_cli_printf(cli, "wifi: %s (%d dBm)\n", st.ssid, st.rssi);
        purr_cli_printf(cli, "ip:   %s\n", st.ip);
    } else {
        purr_cli_puts(cli, "not connected\n");
    }
    return 0;
}

static int cmd_exit(purr_cli_t *cli, int argc, char **argv)
{
    (void)cli;
    (void)argc;
    (void)argv;
    s_shell_exit = 1;
    return 0;
}

static const purr_cmd_t s_shell_cmds[] = {
    {"help", "list the commands", purr_cli_cmd_help},
    {"wifi", "scan|connect|forget|list", cmd_wifi},
    {"net", "connection status", cmd_net},
    {"exit", "leave the shell and continue recovery", cmd_exit},
};

static void run_wifi_shell(void)
{
    purr_cli_t cli;
    purr_cli_init(&cli, s_shell_cmds, sizeof(s_shell_cmds) / sizeof(s_shell_cmds[0]),
                 purr_console_put, NULL, "wifi> ");
    s_shell_exit = 0;
    purr_cli_puts(&cli, "\nWi-Fi shell. Type help for the commands, exit to continue.\n");
    purr_cli_prompt(&cli);
    purr_console_flush();
    while (!s_shell_exit) {
        char c = purr_kernel_key();
        if (c != 0) {
            purr_cli_feed(&cli, c);
            purr_console_flush();
        } else {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
}

/* A short window at startup to drop into the shell, or ask for a full component restore
 * instead of the default (restore KittenOS only) -- this is "menu mode"
 * (RecoveryLoader/SPEC.md section 2.1). Never runs in auto mode: nothing summoned a person to
 * read a prompt, so there's no window to offer. */
static void offer_menu(bool *want_full_restore)
{
    statusf("press S within 3 seconds for a Wi-Fi shell, F for a full component restore...");
    for (uint32_t waited = 0; waited < 3000; waited += 50) {
        char c = purr_kernel_key();
        if (c == 's' || c == 'S') {
            run_wifi_shell();
            return;
        }
        if (c == 'f' || c == 'F') {
            *want_full_restore = true;
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ------------------------------------------------------------ networking */

static esp_err_t get_connected(void)
{
    char ssid[PURR_NETREC_SSID_LEN] = "", pass[PURR_NETREC_PASS_LEN] = "";
    if (purr_netrec_load(ssid, pass) == 0) {
        statusf("connecting to %s...", ssid);
        if (purr_net_connect(ssid, pass, 15000) == ESP_OK) {
            return ESP_OK;
        }
        status("could not connect to the remembered network");
    }

    for (;;) {
        status("type the Wi-Fi network name:");
        read_line(ssid, sizeof(ssid), 0);
        status("type the password:");
        read_line(pass, sizeof(pass), 1);
        statusf("connecting to %s...", ssid);
        if (purr_net_connect(ssid, pass, 15000) == ESP_OK) {
            return ESP_OK;
        }
        status("could not connect. try again.");
    }
}

/* Resolves `name` against the directory `base_url` (everything up to and including its
 * last '/') sits in. `name` is a plain file name from the manifest, never a full URL. */
static void resolve_url(const char *base_url, const char *name, char *out, size_t cap)
{
    const char *slash = strrchr(base_url, '/');
    size_t dir_len = slash ? (size_t)(slash - base_url) + 1 : 0;
    if (dir_len >= cap) {
        dir_len = 0;
    }
    memcpy(out, base_url, dir_len);
    snprintf(out + dir_len, cap - dir_len, "%s", name);
}

/* ------------------------------------------------------------ verifying and writing */

typedef struct {
    const uint8_t *base;
    size_t size;
} mem_read_ctx_t;

static int mem_read(void *ctx, uint32_t offset, void *buf, uint32_t len)
{
    mem_read_ctx_t *m = ctx;
    if ((uint64_t)offset + len > m->size) {
        return -1;
    }
    memcpy(buf, m->base + offset, len);
    return 0;
}

static int install_kittenos(const uint8_t *image, size_t image_len)
{
    purr_flash_t fl;
    purr_cfg_t cfg;
    if (purr_cfgstore_open(&fl) != 0 || purr_cfg_load(&fl, &cfg, NULL) < 0) {
        purr_cfg_defaults(&cfg);
    }
    purr_keybag_t bag;
    purr_keybag_build(&bag, purr_default_keys, purr_default_keys_count, &cfg);

    purr_verify_env_t env = {
        .chip_id = THIS_CHIP,
        .bag = &bag,
        .crypto = &purr_crypto_mbedtls,
        .bootloader_version = 0,
        .version_floor = 0,
        .enforce_floor = 0,
    };
    mem_read_ctx_t rdctx = {image, image_len};
    purr_image_header_t hdr;
    purr_verify_result_t vr = purr_image_verify(&env, mem_read, &rdctx, (uint32_t)image_len, &hdr);

    if (hdr.magic != PURR_IMAGE_MAGIC || hdr.image_type != PURR_IMG_RECOVERY) {
        status("not a KittenOS image");
        return -1;
    }
    if (vr != PURR_V_OK && cfg.secure_mode != PURR_SECURE_OFF) {
        statusf("verification failed: %s", purr_verify_name(vr));
        return -1;
    }
    if (vr != PURR_V_OK) {
        statusf("unverified (%s), accepted because secure mode is off", purr_verify_name(vr));
    }

    const esp_partition_t *kp = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                          ESP_PARTITION_SUBTYPE_APP_FACTORY, "kittenos");
    if (kp == NULL) {
        status("no kittenos partition on this board");
        return -1;
    }
    if (hdr.payload_size > kp->size) {
        status("the image is bigger than the kittenos partition");
        return -1;
    }

    status("writing to the kittenos partition...");
    const uint8_t *payload = image + hdr.payload_offset;
    if (esp_partition_erase_range(kp, 0, kp->size) != ESP_OK ||
        esp_partition_write(kp, 0, payload, hdr.payload_size) != ESP_OK) {
        status("write failed");
        return -1;
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
        if (esp_partition_read(kp, off, chunk, n) != ESP_OK) {
            status("read-back failed");
            return -1;
        }
        purr_sha256_update(&sha, chunk, n);
    }
    purr_sha256_final(&sha, digest);
    if (memcmp(digest, hdr.payload_sha256, sizeof(digest)) != 0) {
        status("read-back hash mismatch: the write did not take");
        return -1;
    }
    statusf("kittenos %s installed", hdr.version);
    return 0;
}

/* ------------------------------------------------------------ component restore (auto mode,
 * and the menu's "full component restore" choice, RecoveryLoader/SPEC.md section 2.1) */

typedef struct {
    const esp_partition_t *p;
} part_read_ctx_t;

static int part_read(void *ctx, uint32_t offset, void *buf, uint32_t len)
{
    part_read_ctx_t *c = ctx;
    if ((uint64_t)offset + len > c->p->size) {
        return -1;
    }
    return esp_partition_read(c->p, offset, buf, len) == ESP_OK ? 0 : -1;
}

/* F-14: kernel and kittenos hold only the stripped payload once installed
 * (purr_net_install_run()/fetch_and_install_module() below only ever write
 * hdr.payload_offset..+payload_size -- the signature and most of the header never land on
 * the partition), so re-verifying them as a whole PURR container in place
 * (partition_verify_in_place(), below) can never succeed: there is nothing there to verify,
 * and every restore rewrote them even when they were already fine. Compare the partition's
 * content hash against the manifest's own record of what the payload should be instead --
 * the same check a fresh download already goes through, just without downloading again.
 * Needs the manifest entry's optional payload_size/payload_sha256 (purr_manifest.h); an
 * older manifest without them means "cannot tell", not "good" -- this falls back to a real
 * restore exactly like before this fix, it never silently skips one. */
static int partition_matches_manifest(const esp_partition_t *p, const purr_manifest_entry_t *entry)
{
    if (entry == NULL || !entry->have_payload_sha256 || entry->payload_size == 0 ||
        entry->payload_size > p->size) {
        return 0;
    }
    purr_sha256_t sha;
    purr_sha256_init(&sha);
    uint8_t chunk[512];
    for (uint32_t off = 0; off < entry->payload_size; off += sizeof(chunk)) {
        uint32_t n = entry->payload_size - off;
        if (n > sizeof(chunk)) {
            n = sizeof(chunk);
        }
        if (esp_partition_read(p, off, chunk, n) != ESP_OK) {
            return 0;
        }
        purr_sha256_update(&sha, chunk, n);
    }
    uint8_t digest[PURR_SHA256_LEN];
    purr_sha256_final(&sha, digest);
    return memcmp(digest, entry->payload_sha256, PURR_SHA256_LEN) == 0;
}

/* Whether what's already sitting in `p` verifies as image_type/subtype right now, without
 * touching the network -- so a restore only ever writes the components that actually need it.
 * Only meaningful for a component whose partition holds the WHOLE signed container (bootpkg;
 * OTA/SPEC.md section 3) -- kernel and kittenos need partition_matches_manifest() above
 * instead (F-14). */
static int partition_verify_in_place(const esp_partition_t *p, uint8_t want_type, uint8_t want_subtype)
{
    purr_flash_t fl;
    purr_cfg_t cfg;
    if (purr_cfgstore_open(&fl) != 0 || purr_cfg_load(&fl, &cfg, NULL) < 0) {
        purr_cfg_defaults(&cfg);
    }
    purr_keybag_t bag;
    purr_keybag_build(&bag, purr_default_keys, purr_default_keys_count, &cfg);
    purr_verify_env_t env = {
        .chip_id = THIS_CHIP, .bag = &bag, .crypto = &purr_crypto_mbedtls,
        .bootloader_version = 0, .version_floor = 0, .enforce_floor = 0,
    };
    part_read_ctx_t rdctx = {p};
    purr_image_header_t hdr;
    purr_verify_result_t vr = purr_image_verify(&env, part_read, &rdctx, (uint32_t)p->size, &hdr);
    if (hdr.magic != PURR_IMAGE_MAGIC) {
        return 0;
    }
    if (want_type == PURR_IMG_MODULE) {
        if (hdr.image_type != PURR_IMG_MODULE || PURR_FLAGS_SUBTYPE(hdr.flags) != want_subtype) {
            return 0;
        }
    } else if (hdr.image_type != want_type) {
        return 0;
    }
    return vr == PURR_V_OK;
}

/* Fetches, verifies and writes a PURR_IMG_MODULE component (kernel or bootpkg -- kittenos
 * stays on install_kittenos() above, a different image type and a function already proven).
 * Mirrors purr_net_install_run()'s per-component logic in commands.c, since both do the same
 * thing against the same partitions; this copy runs from the loader's own environment (no
 * filesystem, its own fetch/verify calls already in scope here). */
static int fetch_and_install_module(const char *name, const char *label, uint8_t subtype,
                                    purr_manifest_t *man)
{
    const purr_manifest_entry_t *entry = purr_manifest_find(man, name, CONFIG_IDF_TARGET,
                                                             purr_board()->name);
    if (entry == NULL) {
        statusf("no %s entry in the manifest", name);
        return -1;
    }
    char url[256];
    resolve_url(CONFIG_PURR_RECOVERY_MANIFEST_URL, entry->file, url, sizeof(url));
    statusf("downloading %s...", entry->file);

    uint8_t *image = NULL;
    size_t image_len = 0;
    if (purr_fetch_alloc(url, &image, &image_len, MAX_IMAGE_SIZE) != ESP_OK) {
        status("download failed");
        return -1;
    }
    uint8_t got_hash[PURR_SHA256_LEN];
    purr_sha256(image, image_len, got_hash);
    if (image_len != entry->size || memcmp(got_hash, entry->sha256, PURR_SHA256_LEN) != 0) {
        free(image);
        status("download does not match the manifest (size or hash)");
        return -1;
    }

    purr_flash_t fl;
    purr_cfg_t cfg;
    if (purr_cfgstore_open(&fl) != 0 || purr_cfg_load(&fl, &cfg, NULL) < 0) {
        purr_cfg_defaults(&cfg);
    }
    purr_keybag_t bag;
    purr_keybag_build(&bag, purr_default_keys, purr_default_keys_count, &cfg);
    purr_verify_env_t env = {
        .chip_id = THIS_CHIP, .bag = &bag, .crypto = &purr_crypto_mbedtls,
        .bootloader_version = 0, .version_floor = 0, .enforce_floor = 0,
    };
    mem_read_ctx_t rdctx = {image, image_len};
    purr_image_header_t hdr;
    purr_verify_result_t vr = purr_image_verify(&env, mem_read, &rdctx, (uint32_t)image_len, &hdr);

    if (hdr.magic != PURR_IMAGE_MAGIC || hdr.image_type != PURR_IMG_MODULE ||
        PURR_FLAGS_SUBTYPE(hdr.flags) != subtype) {
        free(image);
        statusf("not a %s image", name);
        return -1;
    }
    if (vr != PURR_V_OK && cfg.secure_mode != PURR_SECURE_OFF) {
        free(image);
        statusf("%s verification failed: %s", name, purr_verify_name(vr));
        return -1;
    }

    const esp_partition_t *slot = esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
                                                            ESP_PARTITION_SUBTYPE_ANY, label);
    if (slot == NULL) {
        free(image);
        statusf("no %s partition on this board", label);
        return -1;
    }
    /* documentation/FINDINGS.md F-13, fixed 2026-10-01: bootpkg's own partition must hold the
     * WHOLE signed container -- purr_bootpkg.c's load_package() reads and verifies a
     * purr_image_header_t directly from the partition's own start. kernel is a plain ESP app
     * image with no such header, so it still gets just the payload. Writing the stripped
     * payload into bootpkg's slot (the old behavior here too) left the bootloader unable to
     * find a PURR image there at all, quietly removing the boot menu on the very next boot. */
    int whole_container = (subtype == PURR_MOD_BOOTPKG);
    const uint8_t *write_data = whole_container ? image : (image + hdr.payload_offset);
    uint32_t write_len = whole_container ? (uint32_t)image_len : hdr.payload_size;

    if (write_len > slot->size) {
        free(image);
        statusf("the image is bigger than the %s partition", label);
        return -1;
    }

    statusf("writing to %s...", label);
    int ok = (esp_partition_erase_range(slot, 0, slot->size) == ESP_OK &&
             esp_partition_write(slot, 0, write_data, write_len) == ESP_OK);
    free(image);
    if (!ok) {
        status("write failed");
        return -1;
    }
    statusf("%s %s installed", name, hdr.version);
    return 0;
}

/* Checks kernel, kittenos and bootpkg each against what's already there, and only fetches and
 * writes the ones that fail. Never touches LittleFS (RecoveryLoader/SPEC.md section 1) --
 * CoreOS/AppManager/the runtimes/the drivers bundle/apps stay KittenOS's own job once it's
 * running again (section 2.1). */
static int restore_components(void)
{
    status("fetching the recovery manifest...");
    uint8_t *manifest_buf = NULL;
    size_t manifest_len = 0;
    if (purr_fetch_alloc(CONFIG_PURR_RECOVERY_MANIFEST_URL, &manifest_buf, &manifest_len,
                         MAX_MANIFEST_SIZE) != ESP_OK) {
        status("could not fetch the manifest");
        return -1;
    }
    purr_manifest_t man;
    purr_manifest_parse(&man, (const char *)manifest_buf, manifest_len);
    free(manifest_buf);

    int failures = 0;

    const esp_partition_t *kp = esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
                                                          ESP_PARTITION_SUBTYPE_ANY, "kernel");
    const purr_manifest_entry_t *kentry = purr_manifest_find(&man, "kernel", CONFIG_IDF_TARGET,
                                                              purr_board()->name);
    if (kp != NULL && partition_matches_manifest(kp, kentry)) {    /* F-14 */
        status("kernel: already good");
    } else if (fetch_and_install_module("kernel", "kernel", PURR_MOD_KERNEL, &man) != 0) {
        failures++;
    }

    const esp_partition_t *bp = esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
                                                          ESP_PARTITION_SUBTYPE_ANY, "bootpkg");
    if (bp != NULL && partition_verify_in_place(bp, PURR_IMG_MODULE, PURR_MOD_BOOTPKG)) {
        status("bootpkg: already good");
    } else if (fetch_and_install_module("bootpkg", "bootpkg", PURR_MOD_BOOTPKG, &man) != 0) {
        failures++;
    }

    const esp_partition_t *kop = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                           ESP_PARTITION_SUBTYPE_APP_FACTORY,
                                                           "kittenos");
    const purr_manifest_entry_t *kosentry = purr_manifest_find(&man, "kittenos", CONFIG_IDF_TARGET,
                                                                 purr_board()->name);
    if (kop != NULL && partition_matches_manifest(kop, kosentry)) {    /* F-14 */
        status("kittenos: already good");
    } else {
        const purr_manifest_entry_t *entry = kosentry;
        uint8_t *image = NULL;
        size_t image_len = 0;
        char url[256];
        if (entry == NULL) {
            status("no kittenos entry in the manifest");
            failures++;
        } else {
            resolve_url(CONFIG_PURR_RECOVERY_MANIFEST_URL, entry->file, url, sizeof(url));
            statusf("downloading %s...", entry->file);
            uint8_t got_hash[PURR_SHA256_LEN];
            if (purr_fetch_alloc(url, &image, &image_len, MAX_IMAGE_SIZE) != ESP_OK) {
                status("download failed");
                failures++;
            } else {
                purr_sha256(image, image_len, got_hash);
                if (image_len != entry->size ||
                    memcmp(got_hash, entry->sha256, PURR_SHA256_LEN) != 0) {
                    status("download does not match the manifest (size or hash)");
                    failures++;
                } else if (install_kittenos(image, image_len) != 0) {
                    failures++;
                }
                free(image);
            }
        }
    }

    return failures == 0 ? 0 : -1;
}

/* Called right before every successful-install restart (bootloader/SPEC.md section 6,
 * purr_abi.h's PURR_CFGF_CONTINUE_INSTALL): resets boot_fail_count to 0 -- a real flash just
 * succeeded, so the ladder's count from however many attempts it took to get here must not
 * carry forward, or the very next boot re-escalates straight back to this loader before the
 * fix it just installed ever gets a chance to run (a real, reproducible auto-recovery loop,
 * found 2026-09-30, where every single cycle was a genuine success that could never reach a
 * healthy boot to prove it) -- and sets CONTINUE_INSTALL so KittenOS knows to carry on
 * installing the rest of the system on its very next boot, rather than stopping at a normal
 * login prompt as if this were an ordinary first boot. Best-effort: a write failure here
 * only costs a slower recovery (back through the ladder again), not a correctness problem. */
static void mark_install_success(void)
{
    purr_flash_t fl;
    purr_cfg_t cfg;
    if (purr_cfgstore_open(&fl) != 0 || purr_cfg_load(&fl, &cfg, NULL) < 0) {
        purr_cfg_defaults(&cfg);
    }
    cfg.boot_fail_count = 0;
    purr_cfg_set_flag(&cfg, PURR_CFGF_CONTINUE_INSTALL);
    if (purr_cfg_store(&fl, &cfg) != 0) {
        ESP_LOGW(TAG, "purrcfg: could not record the successful install");
    }
}

/* ------------------------------------------------------------ auto mode (RecoveryLoader/SPEC.md
 * section 2.1): no Wi-Fi shell offer, no menu -- nothing summoned a person to answer one. Only
 * the saved recovery network is tried; a full component restore follows, then a normal restart
 * lets the (hopefully now fixed) boot chain run again. */
static void run_auto_mode(void)
{
    status("automatic recovery: repeated boot failure detected");
    if (purr_net_init(NULL) != ESP_OK) {
        halt("wifi did not start");
    }
    char ssid[PURR_NETREC_SSID_LEN] = "", pass[PURR_NETREC_PASS_LEN] = "";
    if (purr_netrec_load(ssid, pass) != 0) {
        halt("no saved network: cannot recover unattended");
    }
    statusf("connecting to %s...", ssid);
    if (purr_net_connect(ssid, pass, 15000) != ESP_OK) {
        halt("could not connect to the saved network");
    }
    status("connected");

    if (restore_components() != 0) {
        halt("automatic recovery failed");
    }

    mark_install_success();
    status("done. restarting...");
    purr_console_flush();
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

/* ------------------------------------------------------------ entry point */

void purr_recovery_loader_main(void)
{
    ESP_LOGI(TAG, "PURR internet recovery starting");
    if (purr_kernel_init() != ESP_OK || purr_kernel_display() == NULL) {
        for (;;) {
            ESP_LOGE(TAG, "display did not come up");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    const purr_display_v2_t *d = purr_kernel_display();
    if (d->set_brightness) {
        d->set_brightness(255);
    }
    purr_console_init(d);
    status("PURR internet recovery");

    /* PURR_CFGF_AUTO_REINSTALL flows bootloader -> here (purr_boot.c's failure-count ladder,
     * bootloader/SPEC.md section 6), the opposite direction from a shell's one-shot request:
     * read and cleared here, not by the bootloader. Its presence means the ladder chose the
     * loader on its own, not a human or a shell command, so this boot runs silently instead of
     * offering anything nobody may be there to answer (RecoveryLoader/SPEC.md section 2.1). */
    {
        purr_flash_t cfg_fl;
        purr_cfg_t cfg;
        if (purr_cfgstore_open(&cfg_fl) == 0 && purr_cfg_load(&cfg_fl, &cfg, NULL) >= 0 &&
            purr_cfg_take_flag(&cfg, PURR_CFGF_AUTO_REINSTALL)) {
            if (purr_cfg_store(&cfg_fl, &cfg) != 0) {
                ESP_LOGW(TAG, "purrcfg: could not clear the auto-reinstall request");
            }
            run_auto_mode();
            /* run_auto_mode() only returns via halt(), which itself never returns. */
        }
    }

    if (purr_net_init(NULL) != ESP_OK) {          /* no filesystem here */
        halt("wifi did not start");
    }
    bool want_full_restore = false;
    offer_menu(&want_full_restore);

    if (get_connected() != ESP_OK) {
        halt("could not connect");
    }
    status("connected");

    if (want_full_restore) {
        if (restore_components() != 0) {
            halt("recovery failed");
        }
        mark_install_success();
        status("done. restarting...");
        purr_console_flush();
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }

    status("fetching the recovery manifest...");
    uint8_t *manifest_buf = NULL;
    size_t manifest_len = 0;
    if (purr_fetch_alloc(CONFIG_PURR_RECOVERY_MANIFEST_URL, &manifest_buf, &manifest_len,
                         MAX_MANIFEST_SIZE) != ESP_OK) {
        halt("could not fetch the manifest");
    }
    purr_manifest_t man;
    purr_manifest_parse(&man, (const char *)manifest_buf, manifest_len);
    free(manifest_buf);

    const purr_manifest_entry_t *entry = purr_manifest_find(&man, "kittenos", CONFIG_IDF_TARGET,
                                                             purr_board()->name);
    if (entry == NULL) {
        halt("no kittenos entry for this board in the manifest");
    }
    statusf("found kittenos %s (%u bytes)", entry->version, (unsigned)entry->size);

    char image_url[256];
    resolve_url(CONFIG_PURR_RECOVERY_MANIFEST_URL, entry->file, image_url, sizeof(image_url));
    statusf("downloading %s...", entry->file);

    uint8_t *image = NULL;
    size_t image_len = 0;
    if (purr_fetch_alloc(image_url, &image, &image_len, MAX_IMAGE_SIZE) != ESP_OK) {
        halt("download failed");
    }
    uint8_t got_hash[PURR_SHA256_LEN];
    purr_sha256(image, image_len, got_hash);
    if (image_len != entry->size || memcmp(got_hash, entry->sha256, PURR_SHA256_LEN) != 0) {
        free(image);
        halt("download does not match the manifest (size or hash)");
    }

    status("verifying...");
    int ok = install_kittenos(image, image_len);
    free(image);
    if (ok != 0) {
        halt("recovery failed");
    }

    mark_install_success();
    status("done. restarting...");
    purr_console_flush();
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

#else /* !CONFIG_PURR_PROFILE_MINIMAL */

void purr_recovery_loader_main(void)
{
}

#endif /* CONFIG_PURR_PROFILE_MINIMAL */
