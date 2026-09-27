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

/* A short window at startup to drop into the shell instead of proceeding on its own. */
static void offer_wifi_shell(void)
{
    statusf("press S within 3 seconds for a Wi-Fi shell...");
    for (uint32_t waited = 0; waited < 3000; waited += 50) {
        char c = purr_kernel_key();
        if (c == 's' || c == 'S') {
            run_wifi_shell();
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

    if (purr_net_init(NULL) != ESP_OK) {          /* no filesystem here */
        halt("wifi did not start");
    }
    offer_wifi_shell();

    if (get_connected() != ESP_OK) {
        halt("could not connect");
    }
    status("connected");

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
