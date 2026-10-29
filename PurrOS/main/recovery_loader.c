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

#include "commands.h"
#include "purr_abi.h"
#include "purr_cfgstore.h"
#include "purr_cli.h"
#include "purr_console.h"
#include "purr_crypto_mbedtls.h"
#include "purr_fetch.h"
#include "oobe_job.h"
#include "oobe_ui.h"
#include "purr_oobe.h"
#include "purr_recovery_src.h"
#include "purr_kernel.h"
#include "purr_manifest.h"
#include "purr_slot_trailer.h"
#include "purr_trust_os.h"
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

/* The setup wizard (purr_oobe.c) runs the slow work as jobs. While it does, messages go to the job (the wizard shows
 * them) instead of the text console, and the progress figures too. */
static int s_wizard;
static oobe_job_t *s_job;

static void status(const char *line)
{
    ESP_LOGI(TAG, "%s", line);
    if (s_wizard) {
        oobe_job_report(s_job, line, -1);
        return;
    }
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
    if (s_wizard) {                                   /* inside a job: end it as failed, and say why */
        ESP_LOGE(TAG, "%s", why);
        oobe_job_abort(s_job, why);
    }
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
    if (purr_net_wired()) {
        purr_cli_puts(cli, "this build uses wired Ethernet: there is no Wi-Fi to scan or join.\n"
                           "use `net` to see the link and address.\n");
        return 1;
    }
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
    if (st.connected && purr_net_wired()) {
        purr_cli_puts(cli, "link: wired Ethernet\n");
        purr_cli_printf(cli, "ip:   %s\n", st.ip);
    } else if (st.connected) {
        purr_cli_printf(cli, "wifi: %s (%d dBm)\n", st.ssid, st.rssi);
        purr_cli_printf(cli, "ip:   %s\n", st.ip);
    } else {
        purr_cli_puts(cli, purr_net_wired() ? "no address yet (is the cable or the NIC missing?)\n"
                                            : "not connected\n");
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
    statusf("press S within 3 seconds for a network shell, F for a full component restore...");
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
    if (purr_net_wired()) {
        /* No network to pick and no password to type: the wire either has a link and a DHCP
         * address or it does not. Keep trying; the person can fix the cable or the NIC. */
        for (;;) {
            status("waiting for the wired link and an address...");
            if (purr_net_connect("", "", 30000) == ESP_OK) {
                return ESP_OK;
            }
            status("no link or no address yet. retrying.");
        }
    }

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

/* May an image that failed verification be written anyway? Never from a relay (a computer on the network) unless developer mode is on;
 * from the official server or a dev server, only while secure mode is off, as before. Strict is the default: this is the only place
 * that decides it. */
static bool unverified_ok(const purr_cfg_t *view)
{
    if (purr_recovery_source_is_relay()) {
        return purr_recovery_dev_mode();
    }
    return view->secure_mode == PURR_SECURE_OFF;
}



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
    purr_cfg_t view;
    os_cfg_view(&cfg, &view);
    purr_keybag_t bag;
    purr_keybag_build(&bag, purr_default_keys, purr_default_keys_count, &view);

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
    if (vr != PURR_V_OK && !unverified_ok(&view)) {
        statusf("verification failed: %s", purr_verify_name(vr));
        return -1;
    }
    if (vr != PURR_V_OK) {
        statusf("unverified (%s), accepted: nothing here requires it", purr_verify_name(vr));
    }

    const esp_partition_t *kp = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                          ESP_PARTITION_SUBTYPE_APP_FACTORY, "kittenos");
    if (kp == NULL) {
        status("no kittenos partition on this board");
        return -1;
    }
    if (!purr_slot_trailer_fits(kp, hdr.payload_size)) {
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

    /* The signature trailer (bootloader/SPEC.md section 3.1), then check the slot the way the
     * bootloader will. */
    if (purr_slot_trailer_write(kp, &hdr) != 0) {
        status("could not write the signature trailer");
        return -1;
    }
    purr_verify_result_t tv = purr_slot_trailer_check(&env, kp, PURR_IMG_RECOVERY, 0);
    if (tv != PURR_V_OK) {
        if (!unverified_ok(&view)) {
            statusf("installed, but the bootloader would reject it: %s", purr_verify_name(tv));
            return -1;
        }
        statusf("no valid signature trailer (%s); fine: nothing here requires it", purr_verify_name(tv));
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

/* Whether the slot's signature trailer verifies right now (bootloader/SPEC.md section 3.1).
 * The payload hash above says the image bytes are right; this says the bootloader will accept
 * them. Not required while secure_mode is off, where an unsigned slot boots anyway -- a restore
 * must not rewrite a healthy unsigned slot forever. */
static int partition_trailer_ok(const esp_partition_t *p, uint8_t want_type, uint8_t want_subtype)
{
    purr_flash_t fl;
    purr_cfg_t cfg;
    if (purr_cfgstore_open(&fl) != 0 || purr_cfg_load(&fl, &cfg, NULL) < 0) {
        purr_cfg_defaults(&cfg);
    }
    purr_cfg_t view;
    os_cfg_view(&cfg, &view);
    if (view.secure_mode == PURR_SECURE_OFF) {
        return 1;
    }
    purr_keybag_t bag;
    purr_keybag_build(&bag, purr_default_keys, purr_default_keys_count, &view);
    purr_verify_env_t env = {
        .chip_id = THIS_CHIP, .bag = &bag, .crypto = &purr_crypto_mbedtls,
        .bootloader_version = 0, .version_floor = 0, .enforce_floor = 0,
    };
    return purr_slot_trailer_check(&env, p, want_type, want_subtype) == PURR_V_OK;
}

/* 1 if the server lists a NEWER version of this component than the one sitting in the slot. The payload hash alone cannot tell: a relabelled or
 * re-signed image with the same bytes has the same hash, so it would be "already good" forever. The installed version is read from the slot's
 * signed header (the trailer, or the container itself for the boot package); if it cannot be read, the other checks decide as before. */
static int installed_is_older(const esp_partition_t *p, const purr_manifest_entry_t *entry)
{
    uint32_t want, have;
    if (p == NULL || entry == NULL || purr_version_parse(entry->version, strlen(entry->version), &want) != 0) {
        return 0;
    }
    purr_image_header_t h;
    if (purr_slot_trailer_header(p, &h) != 0) {
        if (esp_partition_read(p, 0, &h, sizeof(h)) != ESP_OK || h.magic != PURR_IMAGE_MAGIC) {
            return 0;
        }
    }
    if (purr_version_parse(h.version, strnlen(h.version, sizeof(h.version)), &have) != 0) {
        return 0;
    }
    return want > have;
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
    purr_cfg_t view;
    os_cfg_view(&cfg, &view);
    purr_keybag_t bag;
    purr_keybag_build(&bag, purr_default_keys, purr_default_keys_count, &view);
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
    resolve_url(purr_recovery_manifest_url(), entry->file, url, sizeof(url));
    status(purr_recovery_source_name() != NULL ? "Downloading PURR OS from your dev server" : "Downloading PURR OS");

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
    purr_cfg_t view;
    os_cfg_view(&cfg, &view);
    purr_keybag_t bag;
    purr_keybag_build(&bag, purr_default_keys, purr_default_keys_count, &view);
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
    if (vr != PURR_V_OK && !unverified_ok(&view)) {
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

    if (whole_container ? write_len > slot->size : !purr_slot_trailer_fits(slot, write_len)) {
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

    /* The signature trailer (bootloader/SPEC.md section 3.1) for the raw app slots; the boot
     * package already holds its whole container. Then check the slot the way the bootloader
     * will. */
    if (!whole_container) {
        if (purr_slot_trailer_write(slot, &hdr) != 0) {
            status("could not write the signature trailer");
            return -1;
        }
        purr_verify_result_t tv = purr_slot_trailer_check(&env, slot, PURR_IMG_MODULE, subtype);
        if (tv != PURR_V_OK) {
            if (!unverified_ok(&view)) {
                statusf("%s installed, but the bootloader would reject it: %s", name, purr_verify_name(tv));
                return -1;
            }
            statusf("no valid signature trailer (%s); fine: nothing here requires it", purr_verify_name(tv));
        }
    }
    statusf("%s %s installed", name, hdr.version);
    return 0;
}

/* Checks kernel, kittenos and bootpkg each against what's already there, and only fetches and
 * writes the ones that fail. Never touches LittleFS (RecoveryLoader/SPEC.md section 1) --
 * CoreOS/AppManager/the runtimes/the drivers bundle/apps stay KittenOS's own job once it's
 * running again (section 2.1). */
/* The screen's bar: which step of how many, and how much of the download in progress has arrived. */
static int s_step, s_steps = 1;
static bool s_setup;        /* the wizard is only collecting the network details (no saved network); the restore runs after the restart */

static void wiz_progress(int step, int steps, int within)
{
    if (s_wizard && steps > 0) {
        oobe_job_report(s_job, NULL, (step * 1000 + within) / steps);
    }
}

static void fetch_progress(size_t got, size_t total)
{
    if (total > 0) {
        wiz_progress(s_step, s_steps, (int)((uint64_t)got * 1000 / total));
    }
}

/* What recovery puts back. By default only what KittenOS needs to restart: the KittenOS slot, and the boot package (the boot menu and
 * screens; best effort, the device starts without it). KittenOS then carries on: it asks for the account, and installs the kernel and
 * the modules in the background (stage 2, commands.c). include_kernel is the old "restore everything at once" (the F key in the text
 * flow). Failures return -1 except the boot package, which never stops the restore. */
static bool s_emergency_ok;                        /* nobody to ask (automatic recovery): the loader may pick a recovery computer by itself */

static int restore_components(int include_kernel)
{
    int steps = include_kernel ? 3 : 2, step = 0;
    s_steps = steps;
    s_step = 0;
    wiz_progress(0, steps, 0);
    purr_fetch_set_progress(fetch_progress);
    status("Checking what to install...");
    uint8_t *manifest_buf = NULL;
    size_t manifest_len = 0;
    if (purr_fetch_alloc(purr_recovery_manifest_url(), &manifest_buf, &manifest_len,
                         MAX_MANIFEST_SIZE) != ESP_OK) {
        status("Couldn't reach the install server. Looking for a dev server (emergency mode)...");
        if (!s_emergency_ok || purr_recovery_emergency() != 0 ||
            purr_fetch_alloc(purr_recovery_manifest_url(), &manifest_buf, &manifest_len, MAX_MANIFEST_SIZE) != ESP_OK) {
            status("Couldn't reach the install server.");
            return -1;
        }
        status("Emergency mode: restoring from a dev server on this network");
    }
    purr_manifest_t man;
    purr_manifest_parse(&man, (const char *)manifest_buf, manifest_len);
    free(manifest_buf);

    int failures = 0;

    if (include_kernel) {
        s_step = step;
        wiz_progress(step++, steps, 0);
        const esp_partition_t *kp = esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
                                                              ESP_PARTITION_SUBTYPE_ANY, "kernel");
        const purr_manifest_entry_t *kentry = purr_manifest_find(&man, "kernel", CONFIG_IDF_TARGET,
                                                                  purr_board()->name);
        if (kp != NULL && !purr_slot_marked_bad(kp) && partition_matches_manifest(kp, kentry) &&    /* F-14 */
            partition_trailer_ok(kp, PURR_IMG_MODULE, PURR_MOD_KERNEL) && !installed_is_older(kp, kentry)) {
            status("kernel: already good");
        } else if (fetch_and_install_module("kernel", "kernel", PURR_MOD_KERNEL, &man) != 0) {
            failures++;
        }
    }

    s_step = step;
    wiz_progress(step++, steps, 0);
    const esp_partition_t *bp = esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
                                                          ESP_PARTITION_SUBTYPE_ANY, "bootpkg");
    const purr_manifest_entry_t *bentry = purr_manifest_find(&man, "bootpkg", CONFIG_IDF_TARGET, purr_board()->name);
    if (bp != NULL && partition_verify_in_place(bp, PURR_IMG_MODULE, PURR_MOD_BOOTPKG) && !installed_is_older(bp, bentry)) {
        status("The boot menu is already good");
    } else if (fetch_and_install_module("bootpkg", "bootpkg", PURR_MOD_BOOTPKG, &man) != 0) {
        status("The boot menu couldn't be updated, carrying on without it");     /* optional: never stops the restore */
    }

    s_step = step;
    wiz_progress(step++, steps, 0);
    const esp_partition_t *kop = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                           ESP_PARTITION_SUBTYPE_APP_FACTORY,
                                                           "kittenos");
    const purr_manifest_entry_t *kosentry = purr_manifest_find(&man, "kittenos", CONFIG_IDF_TARGET,
                                                                 purr_board()->name);
    if (kop != NULL && !purr_slot_marked_bad(kop) && partition_matches_manifest(kop, kosentry) &&    /* F-14 */
        partition_trailer_ok(kop, PURR_IMG_RECOVERY, 0) && !installed_is_older(kop, kosentry)) {
        status("KittenOS is already good");
    } else {
        const purr_manifest_entry_t *entry = kosentry;
        uint8_t *image = NULL;
        size_t image_len = 0;
        char url[256];
        if (entry == NULL) {
            status("The server has nothing for this device yet.");
            failures++;
        } else {
            resolve_url(purr_recovery_manifest_url(), entry->file, url, sizeof(url));
            status(purr_recovery_source_name() != NULL ? "Downloading PURR OS from your dev server" : "Downloading PURR OS");
            uint8_t got_hash[PURR_SHA256_LEN];
            if (purr_fetch_alloc(url, &image, &image_len, MAX_IMAGE_SIZE) != ESP_OK) {
                status("The download didn't finish.");
                failures++;
            } else {
                purr_sha256(image, image_len, got_hash);
                if (image_len != entry->size ||
                    memcmp(got_hash, entry->sha256, PURR_SHA256_LEN) != 0) {
                    status("The download didn't match what the server promised.");
                    failures++;
                } else if (install_kittenos(image, image_len) != 0) {
                    failures++;
                }
                free(image);
            }
        }
    }

    purr_fetch_set_progress(NULL);
    wiz_progress(steps, steps, 0);
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
 * only costs a slower recovery (back through the ladder again), not a correctness problem.
 *
 * unattended: true only from run_auto_mode() -- also sets PURR_CFGF_UNATTENDED_SETUP, so
 * login.c's first_time_setup() knows not to block on a typed username nobody is there to
 * provide (purr_abi.h's own comment on that flag). false from every attended call site here
 * (a human already sat through offer_menu()/get_connected()), which is still right there for
 * the login prompt that follows. */
static void mark_install_success_ex(bool unattended, bool continue_install)
{
    purr_flash_t fl;
    purr_cfg_t cfg;
    if (purr_cfgstore_open(&fl) != 0 || purr_cfg_load(&fl, &cfg, NULL) < 0) {
        purr_cfg_defaults(&cfg);
    }
    cfg.boot_fail_count = 0;
    if (continue_install) {
        purr_cfg_set_flag(&cfg, PURR_CFGF_CONTINUE_INSTALL);
        if (unattended) {
            purr_cfg_set_flag(&cfg, PURR_CFGF_UNATTENDED_SETUP);
        }
    }
    if (purr_cfg_store(&fl, &cfg) != 0) {
        ESP_LOGW(TAG, "purrcfg: could not record the successful install");
    }
}

static void mark_install_success(bool unattended) { mark_install_success_ex(unattended, true); }

/* A reset that keeps the data (the recovery app's "Reinstall PURR OS"): the kernel was marked BAD and the device is not being wiped
 * (FRESH is not set). The loader has the RAM that KittenOS does not, so it puts the kernel back itself, and since the modules and the
 * files are still there, PURR OS can start straight away: no KittenOS stage 2. */
static bool restoring_in_place(void)
{
    purr_flash_t fl;
    purr_cfg_t cfg;
    if (purr_cfgstore_open(&fl) != 0 || purr_cfg_load(&fl, &cfg, NULL) < 0) {
        return false;
    }
    return (cfg.flags & PURR_CFGF_BAD_KERNEL) != 0 && (cfg.flags & PURR_CFGF_FRESH) == 0;
}


/* ------------------------------------------------------------ auto mode (RecoveryLoader/SPEC.md
 * section 2.1): no Wi-Fi shell offer, no menu -- nothing summoned a person to answer one. Only
 * the saved recovery network is tried; a full component restore follows, then a normal restart
 * lets the (hopefully now fixed) boot chain run again. */
static void run_auto_mode(void)
{
    status("automatic recovery: repeated boot failure detected");
    s_emergency_ok = true;
    if (purr_net_init(NULL) != ESP_OK) {
        halt(purr_net_wired() ? "the wired network did not start" : "wifi did not start");
    }
    if (purr_net_wired()) {
        /* Nothing to remember and nothing to type: the wire has an address or it does not. */
        status("waiting for the wired link and an address...");
        if (purr_net_connect("", "", 30000) != ESP_OK) {
            halt("no wired link or address: cannot recover unattended");
        }
    } else {
        char ssid[PURR_NETREC_SSID_LEN] = "", pass[PURR_NETREC_PASS_LEN] = "";
        if (purr_netrec_load(ssid, pass) != 0) {
            halt("no saved network: cannot recover unattended");
        }
        statusf("connecting to %s...", ssid);
        if (purr_net_connect(ssid, pass, 15000) != ESP_OK) {
            halt("could not connect to the saved network");
        }
    }
    status("connected");

    bool in_place = restoring_in_place();
    if (restore_components(1) != 0) {
        halt("automatic recovery failed");
    }

    mark_install_success_ex(true, !in_place);
    status("done. restarting...");
    purr_console_flush();
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

/* ------------------------------------------------------------ the setup wizard (colour T-Deck boards) */

#if CONFIG_PURR_BOARD_TDECK || CONFIG_PURR_BOARD_TDECK_PLUS

#define WIZ_MAX_AP 8
#define LEGAL_MAX (16 * 1024)

static purr_oobe_ap_t s_aps[WIZ_MAX_AP];
static int s_nap;
static char *s_legal_text;
static char s_arg_a[64], s_arg_b[72];
static int s_auto;                                  /* the failure ladder sent us here: nobody to ask, restore everything */

static int job_scan(oobe_job_t *job, void *arg)
{
    (void)arg;
    purr_net_ap_t aps[WIZ_MAX_AP];
    int n = 0;
    oobe_job_report(job, "Looking for networks...", -1);
    if (purr_net_scan(aps, WIZ_MAX_AP, &n) != ESP_OK) {
        oobe_job_report(job, "Couldn't scan for networks.", -1);
        return 1;
    }
    s_nap = 0;
    for (int i = 0; i < n && s_nap < WIZ_MAX_AP; i++) {
        snprintf(s_aps[s_nap].ssid, sizeof(s_aps[0].ssid), "%s", aps[i].ssid);
        s_aps[s_nap].rssi = aps[i].rssi;
        s_aps[s_nap].secure = !aps[i].open;
        s_nap++;
    }
    return 0;
}

static purr_oobe_server_t s_servers[4];
static int s_nsrv;

static int job_devscan(oobe_job_t *job, void *arg)
{
    (void)arg;
    purr_lan_server_t found[4];
    oobe_job_report(job, "Looking for servers...", -1);
    int n = purr_lan_discover(found, 4, 4000);          /* probes, and listens for the announcements a server sends */
    s_nsrv = 0;
    for (int i = 0; i < n; i++) {
        if (!purr_recovery_server_allowed(found[i].id)) {
            continue;                                   /* a developer's server while developer recovery is off */
        }
        snprintf(s_servers[s_nsrv].base, sizeof(s_servers[0].base), "%s", found[i].base);
        snprintf(s_servers[s_nsrv].name, sizeof(s_servers[0].name), "%s", found[i].name);
        snprintf(s_servers[s_nsrv].id, sizeof(s_servers[0].id), "%s", found[i].id);
        s_nsrv++;
    }
    return 0;
}

static int job_devverify(oobe_job_t *job, void *arg)
{
    (void)arg;
    char msg[80];
    int r = purr_recovery_verify(s_arg_a, msg, sizeof(msg));
    oobe_job_report(job, msg, -1);
    return r == 0 ? 0 : 1;
}

static int job_connect(oobe_job_t *job, void *arg)
{
    (void)arg;
    if (purr_net_wired()) {                         /* nothing to pick and no password: the wire has an address or it doesn't */
        oobe_job_report(job, "Waiting for the wired link and an address...", -1);
        if (purr_net_connect("", "", 30000) != ESP_OK) {
            oobe_job_report(job, "No wired link or address yet.", -1);
            return 1;
        }
        return 0;
    }
    char pass[PURR_NETREC_PASS_LEN];
    snprintf(pass, sizeof(pass), "%.*s", (int)sizeof(pass) - 1, s_arg_b);
    if (pass[0] == '\0') {                          /* a remembered network connects without typing the password again */
        char rs[PURR_NETREC_SSID_LEN], rp[PURR_NETREC_PASS_LEN];
        if (purr_netrec_load(rs, rp) == 0 && strcmp(rs, s_arg_a) == 0) {
            snprintf(pass, sizeof(pass), "%.*s", (int)sizeof(pass) - 1, rp);
        }
    }
    oobe_job_report(job, "Connecting...", -1);
    if (purr_net_connect(s_arg_a, pass, 15000) != ESP_OK) {
        oobe_job_report(job, "Couldn't connect. Check the password and try again.", -1);
        return 1;
    }
    purr_netrec_save(s_arg_a, pass);                /* so the next boot (KittenOS) can carry on without asking */
    return 0;
}

static int job_legal(oobe_job_t *job, void *arg)
{
    (void)arg;
    char url[256];
    snprintf(url, sizeof(url), "%s", purr_recovery_legal_url());
    uint8_t *buf = NULL;
    size_t len = 0;
    free(s_legal_text);
    s_legal_text = NULL;
    if (purr_fetch_alloc(url, &buf, &len, LEGAL_MAX) != ESP_OK) {
        oobe_job_report(job, "Couldn't load it.", -1);
        return 1;
    }
    s_legal_text = malloc(len + 1);                 /* a NUL-terminated copy, whatever the fetch left after the bytes */
    if (s_legal_text != NULL) {
        memcpy(s_legal_text, buf, len);
        s_legal_text[len] = '\0';
    }
    free(buf);
    return s_legal_text != NULL ? 0 : 1;
}

/* Joins the saved network (or the wire) with nobody to ask: the unattended path. */
static void connect_saved_or_halt(void)
{
    if (purr_net_wired()) {
        status("Waiting for the wired link and an address...");
        if (purr_net_connect("", "", 30000) != ESP_OK) {
            halt("No wired link or address, so we can't recover on our own.");
        }
        return;
    }
    char ssid[PURR_NETREC_SSID_LEN] = "", pass[PURR_NETREC_PASS_LEN] = "";
    if (purr_netrec_load(ssid, pass) != 0) {
        halt("There's no saved network, so we can't recover on our own.");
    }
    status("Connecting to the saved network...");
    if (purr_net_connect(ssid, pass, 15000) != ESP_OK) {
        halt("Couldn't connect to the saved network.");
    }
}

static int job_restore(oobe_job_t *job, void *arg)
{
    (void)arg;
    if (s_auto) {
        s_emergency_ok = true;
        connect_saved_or_halt();
    }
    /* Normally only what KittenOS needs to restart; KittenOS asks for the account, installs the kernel and the modules, and does the rest.
     * After a reset the kernel comes from here as well (this task has the RAM), and an in-place restore needs nothing more. */
    bool in_place = restoring_in_place();
    if (restore_components(1) != 0) {
        halt("Something went wrong while installing. Power it off and on to try again.");
    }
    mark_install_success_ex(s_auto != 0, !in_place);
    oobe_job_report(job, "All done. Restarting...", 1000);
    return 0;
}

/* ---- the wizard's operations (purr_oobe.h) */

static uint16_t op_reasons(void)
{
    purr_handoff_t h;
    return purr_kernel_handoff(&h) == 0 ? h.recovery_reasons : 0;
}

static const char *op_legal(void) { return s_legal_text; }
static int op_wired(void) { return purr_net_wired(); }

static const char *op_remembered(void)
{
    static char ssid[PURR_NETREC_SSID_LEN];
    char pass[PURR_NETREC_PASS_LEN];
    return purr_netrec_load(ssid, pass) == 0 ? ssid : NULL;
}

static int op_job_start(purr_oobe_job_t kind, const char *a, const char *b)
{
    oobe_job_fn fn;
    snprintf(s_arg_a, sizeof(s_arg_a), "%s", a != NULL ? a : "");
    snprintf(s_arg_b, sizeof(s_arg_b), "%s", b != NULL ? b : "");
    switch (kind) {
    case PURR_OOBE_JOB_DEVSCAN:   fn = job_devscan; break;
    case PURR_OOBE_JOB_DEVVERIFY: fn = job_devverify; break;
    case PURR_OOBE_JOB_SCAN:    fn = job_scan; break;
    case PURR_OOBE_JOB_CONNECT: fn = job_connect; break;
    case PURR_OOBE_JOB_LEGAL:   fn = job_legal; break;
    case PURR_OOBE_JOB_RESTORE: fn = job_restore; break;
    default:                    return -1;
    }
    return oobe_job_start_stack(s_job, fn, NULL, kind == PURR_OOBE_JOB_RESTORE ? OOBE_STACK_INSTALL : OOBE_STACK_NET);
}

static int op_job_poll(purr_oobe_job_t kind, char *msg, size_t cap, int *per_mille)
{
    (void)kind;
    return oobe_job_poll(s_job, msg, cap, per_mille);
}

static int op_scan_results(purr_oobe_ap_t *out, int max)
{
    int n = s_nap < max ? s_nap : max;
    memcpy(out, s_aps, (size_t)n * sizeof(out[0]));
    return n;
}

static int op_dev_recovery(void) { return purr_recovery_dev_allowed(); }

static int op_server_results(purr_oobe_server_t *out, int max)
{
    int n = s_nsrv < max ? s_nsrv : max;
    memcpy(out, s_servers, (size_t)n * sizeof(out[0]));
    return n;
}

static void op_use_server(const purr_oobe_server_t *s)
{
    purr_lan_server_t srv;
    if (s == NULL) {
        purr_recovery_use(NULL);
        return;
    }
    snprintf(srv.base, sizeof(srv.base), "%s", s->base);
    snprintf(srv.name, sizeof(srv.name), "%s", s->name);
    snprintf(srv.id, sizeof(srv.id), "%s", s->id);
    purr_recovery_use(&srv);
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2500));                 /* long enough to read "All done" */
    esp_restart();
}

static void op_finish(void)
{
    if (s_setup) {                                   /* the details are saved: restart and let the automatic reinstall use them */
        purr_flash_t fl;
        purr_cfg_t cfg;
        if (purr_cfgstore_open(&fl) == 0 && purr_cfg_load(&fl, &cfg, NULL) >= 0) {
            purr_cfg_set_flag(&cfg, PURR_CFGF_AUTO_REINSTALL);
            purr_cfg_store(&fl, &cfg);
        }
    }
    xTaskCreatePinnedToCore(restart_task, "restart", 2048, NULL, 3, NULL, 0);   /* core 0: see park_app_cpu() in the bootloader */
}

static const purr_oobe_ops_t k_ops = {
    .reasons = op_reasons, .legal = op_legal, .dev_recovery = op_dev_recovery, .server_results = op_server_results,
    .use_server = op_use_server, .wired = op_wired, .remembered_ssid = op_remembered,
    .job_start = op_job_start, .job_poll = op_job_poll, .scan_results = op_scan_results, .background = NULL,
    .finish = op_finish,
};

#endif /* colour T-Deck */

/* ------------------------------------------------------------ entry point */

void purr_recovery_loader_main(void)
{
    ESP_LOGI(TAG, "PURR internet recovery starting");
    int hw_ok = purr_kernel_init() == ESP_OK;
    if (hw_ok) {
        /* No filesystem here, so the display, keyboard and touch drivers come from the copies baked into this image. */
        ESP_LOGI(TAG, "%d driver module(s) loaded from memory", purr_loader_load_drivers());
    }
    if (!hw_ok || purr_kernel_display() == NULL) {
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
    int wizard = oobe_ui_available();
    if (wizard) {
        purr_console_set_muted(1);                     /* the setup wizard draws the screen; the log text stays in the serial log */
    } else {
        status("PURR internet recovery");
    }

    /* PURR_CFGF_AUTO_REINSTALL flows bootloader -> here (purr_boot.c's failure-count ladder,
     * bootloader/SPEC.md section 6), the opposite direction from a shell's one-shot request:
     * read and cleared here, not by the bootloader. Its presence means the ladder chose the
     * loader on its own, not a human or a shell command, so this boot runs silently instead of
     * offering anything nobody may be there to answer (RecoveryLoader/SPEC.md section 2.1). */
    int auto_mode = 0;
    {
        purr_flash_t cfg_fl;
        purr_cfg_t cfg;
        if (purr_cfgstore_open(&cfg_fl) == 0 && purr_cfg_load(&cfg_fl, &cfg, NULL) >= 0 &&
            purr_cfg_take_flag(&cfg, PURR_CFGF_AUTO_REINSTALL)) {
            if (purr_cfg_store(&cfg_fl, &cfg) != 0) {
                ESP_LOGW(TAG, "purrcfg: could not clear the auto-reinstall request");
            }
            auto_mode = 1;
        }
    }

    /* A reset asks for the automatic reinstall, which joins the saved network. With no saved network (and no wire) there is nothing to join,
     * so fall back to the questions instead of stopping with "no saved network". */
    if (auto_mode && !purr_net_wired()) {
        char ssid0[PURR_NETREC_SSID_LEN] = "", pass0[PURR_NETREC_PASS_LEN] = "";
        if (purr_netrec_load(ssid0, pass0) != 0) {
            auto_mode = 0;
        }
    }

    /* Every entry to the loader is a restore, and the restore always runs the same way: automatically, from the saved network (or the wire),
     * with nothing else running. With no saved network the wizard only asks for one (setup), saves it, and restarts; the automatic
     * restore then runs on a fresh heap. */
    bool setup_mode = false;
    if (wizard && !purr_net_wired()) {
        char ssid1[PURR_NETREC_SSID_LEN] = "", pass1[PURR_NETREC_PASS_LEN] = "";
        setup_mode = purr_netrec_load(ssid1, pass1) != 0;
    }
    if (wizard && !setup_mode) {
        auto_mode = 1;
    }
    s_setup = setup_mode;

    if (purr_net_init(NULL) != ESP_OK) {          /* no filesystem here */
        if (wizard) {
            purr_console_set_muted(0);
            wizard = 0;
        }
        halt(purr_net_wired() ? "the wired network did not start" : "wifi did not start");
    }

#if CONFIG_PURR_BOARD_TDECK || CONFIG_PURR_BOARD_TDECK_PLUS
    if (wizard) {
        /* The wizard: "something went wrong" (or "first time?"), Wi-Fi, the legal text, restoring. Unattended, it opens on the
         * restoring page and joins the saved network by itself. It does not return unless LVGL could not start. */
        s_wizard = 1;
        s_auto = auto_mode;
        s_job = oobe_job_new();
        oobe_ui_run(setup_mode ? PURR_OOBE_PART_LOADER_SETUP : PURR_OOBE_PART_LOADER, &k_ops, auto_mode ? -1 : 0);
        s_wizard = 0;                              /* LVGL did not start: fall back to the text screens */
        purr_console_set_muted(0);
        status("PURR internet recovery");
    }
#endif

    if (auto_mode) {
        run_auto_mode();
        /* run_auto_mode() only returns via halt(), which itself never returns. */
    }

    bool want_full_restore = false;
    offer_menu(&want_full_restore);

    if (get_connected() != ESP_OK) {
        halt("could not connect");
    }
    status("connected");

    if (restore_components(want_full_restore ? 1 : 0) != 0) {     /* F = everything at once; the default is KittenOS only */
        halt("recovery failed");
    }
    mark_install_success(false);
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
