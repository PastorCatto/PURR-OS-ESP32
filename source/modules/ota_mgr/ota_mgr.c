// ota_mgr.c — PURR OS OTA update manager
//
// See ota_mgr.h for the full design comment (integrity model, rollback
// ownership, SD-card extension point). This file covers WiFi HTTPS delivery
// only — esp_https_ota() (see below) does the network+flash-write plumbing;
// this module adds manifest fetch/version-check, NVS-persisted config, and
// the SHA-256 verification gate esp_https_ota doesn't provide on its own.
//
// esp_https_ota_finish()'s own doc comment is explicit: it "switches the
// boot partition to the OTA partition containing the new firmware image" —
// so the checksum verify below runs BEFORE finish(), between perform() and
// finish(), and a mismatch calls esp_https_ota_abort() instead. Verifying
// AFTER finish() would be too late: the device would already be pointed at
// the unverified image.

#include "ota_mgr.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/core/purr_module.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ota_mgr";
#define NVS_NS "ota_mgr"
#define MANIFEST_MAX 2048   // small JSON, generous cap

static ota_mgr_status_t s_status              = OTA_MGR_IDLE;
static char             s_manifest_url[256]   = "";
static char             s_available_version[16] = "";
static char             s_image_url[256]      = "";
static char             s_expected_sha256_hex[65] = "";   // 64 hex chars + NUL
static size_t           s_expected_size       = 0;
static int              s_progress_percent    = 0;
static char             s_error[96]           = "";

// ── NVS persistence — same shape as wifi_mgr's ssid/password ────────────────

static void load_manifest_url(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_manifest_url);
    nvs_get_str(h, "url", s_manifest_url, &len);
    nvs_close(h);
}

void ota_mgr_set_manifest_url(const char *url) {
    if (!url) return;
    strncpy(s_manifest_url, url, sizeof(s_manifest_url) - 1);
    s_manifest_url[sizeof(s_manifest_url) - 1] = '\0';

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "url", s_manifest_url);
    nvs_commit(h);
    nvs_close(h);
}

const char *ota_mgr_manifest_url(void) { return s_manifest_url; }

// ── Small blocking HTTP GET — manifest only, not the firmware image itself
// (that goes through esp_https_ota's own streaming path in ota_mgr_apply()) ──

static bool http_get(const char *url, char *out, size_t out_size) {
    esp_http_client_config_t cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    bool ok = false;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        if (esp_http_client_get_status_code(client) == 200) {
            int total = 0;
            while (total < (int)out_size - 1) {
                int n = esp_http_client_read(client, out + total, out_size - 1 - total);
                if (n <= 0) break;
                total += n;
            }
            out[total] = '\0';
            ok = total > 0;
        } else {
            ESP_LOGW(TAG, "manifest fetch: HTTP %d", esp_http_client_get_status_code(client));
        }
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    return ok;
}

// ── Manifest check ───────────────────────────────────────────────────────────

void ota_mgr_check(void) {
    if (s_manifest_url[0] == '\0') {
        snprintf(s_error, sizeof(s_error), "no manifest URL set");
        s_status = OTA_MGR_FAILED;
        return;
    }
    s_status = OTA_MGR_CHECKING;

    static char body[MANIFEST_MAX];   // static: too big for a worker task's stack
    if (!http_get(s_manifest_url, body, sizeof(body))) {
        snprintf(s_error, sizeof(s_error), "manifest fetch failed");
        s_status = OTA_MGR_FAILED;
        return;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        snprintf(s_error, sizeof(s_error), "manifest is not valid JSON");
        s_status = OTA_MGR_FAILED;
        return;
    }

    const cJSON *jversion = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *jurl     = cJSON_GetObjectItemCaseSensitive(root, "url");
    const cJSON *jsha     = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    const cJSON *jsize    = cJSON_GetObjectItemCaseSensitive(root, "size");

    if (!cJSON_IsString(jversion) || !cJSON_IsString(jurl) || !cJSON_IsString(jsha)) {
        snprintf(s_error, sizeof(s_error), "manifest missing version/url/sha256");
        cJSON_Delete(root);
        s_status = OTA_MGR_FAILED;
        return;
    }

    strncpy(s_available_version, jversion->valuestring, sizeof(s_available_version) - 1);
    s_available_version[sizeof(s_available_version) - 1] = '\0';
    strncpy(s_image_url, jurl->valuestring, sizeof(s_image_url) - 1);
    s_image_url[sizeof(s_image_url) - 1] = '\0';
    strncpy(s_expected_sha256_hex, jsha->valuestring, sizeof(s_expected_sha256_hex) - 1);
    s_expected_sha256_hex[sizeof(s_expected_sha256_hex) - 1] = '\0';
    s_expected_size = cJSON_IsNumber(jsize) ? (size_t)jsize->valuedouble : 0;

    cJSON_Delete(root);

    s_status = (purr_kernel_version_cmp(s_available_version, KITT_VERSION) > 0)
                   ? OTA_MGR_AVAILABLE : OTA_MGR_UP_TO_DATE;
}

// ── Apply ─────────────────────────────────────────────────────────────────────

static bool hex_to_sha256(const char *hex, uint8_t out[32]) {
    if (strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

void ota_mgr_apply(void) {
    if (s_status != OTA_MGR_AVAILABLE) {
        snprintf(s_error, sizeof(s_error), "no update available — call ota_mgr_check() first");
        s_status = OTA_MGR_FAILED;
        return;
    }

    // Same partition esp_https_ota_begin() will pick internally (deterministic:
    // only two OTA slots, always the non-running one) — obtained here too so
    // the verify step below has something to read back from.
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        snprintf(s_error, sizeof(s_error), "no OTA partition available — is device.pcat [device] ota = true?");
        s_status = OTA_MGR_FAILED;
        return;
    }

    s_status = OTA_MGR_DOWNLOADING;
    s_progress_percent = 0;

    esp_http_client_config_t http_cfg = {
        .url               = s_image_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 15000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        snprintf(s_error, sizeof(s_error), "ota begin failed: %s", esp_err_to_name(err));
        s_status = OTA_MGR_FAILED;
        return;
    }

    int image_size = esp_https_ota_get_image_size(handle);
    do {
        err = esp_https_ota_perform(handle);
        int read_so_far = esp_https_ota_get_image_len_read(handle);
        if (image_size > 0) s_progress_percent = (read_so_far * 100) / image_size;
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (err != ESP_OK) {
        snprintf(s_error, sizeof(s_error), "download failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        s_status = OTA_MGR_FAILED;
        return;
    }
    if (!esp_https_ota_is_complete_data_received(handle)) {
        snprintf(s_error, sizeof(s_error), "incomplete image received");
        esp_https_ota_abort(handle);
        s_status = OTA_MGR_FAILED;
        return;
    }

    // ── Verify BEFORE finish() — finish() is what switches the boot
    // partition, so this is the last point an abort() still leaves the
    // currently-running image as the boot target. ──────────────────────────
    s_status = OTA_MGR_VERIFYING;

    uint8_t expected[32];
    if (!hex_to_sha256(s_expected_sha256_hex, expected)) {
        snprintf(s_error, sizeof(s_error), "manifest sha256 is malformed");
        esp_https_ota_abort(handle);
        s_status = OTA_MGR_FAILED;
        return;
    }

    size_t image_len = s_expected_size ? s_expected_size : (size_t)esp_https_ota_get_image_len_read(handle);
    // Never read past the slot this image was written into, regardless of
    // what the manifest claims.
    if (image_len > update_part->size) image_len = update_part->size;

    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);   // 0 = SHA-256 (not the SHA-224 variant)

    uint8_t buf[1024];
    size_t  offset  = 0;
    bool    read_ok = true;
    while (offset < image_len) {
        size_t chunk = image_len - offset;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        if (esp_partition_read(update_part, offset, buf, chunk) != ESP_OK) {
            read_ok = false;
            break;
        }
        mbedtls_sha256_update(&sha_ctx, buf, chunk);
        offset += chunk;
    }

    uint8_t actual[32];
    mbedtls_sha256_finish(&sha_ctx, actual);
    mbedtls_sha256_free(&sha_ctx);

    if (!read_ok) {
        snprintf(s_error, sizeof(s_error), "could not read back written image");
        esp_https_ota_abort(handle);
        s_status = OTA_MGR_FAILED;
        return;
    }
    if (memcmp(actual, expected, sizeof(actual)) != 0) {
        ESP_LOGE(TAG, "SHA-256 mismatch — aborting, currently-running image stays the boot target");
        snprintf(s_error, sizeof(s_error), "checksum mismatch — image not trusted");
        esp_https_ota_abort(handle);
        s_status = OTA_MGR_FAILED;
        return;
    }

    // Verified — now safe to let finish() commit + switch the boot partition.
    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        snprintf(s_error, sizeof(s_error), "ota finish failed: %s", esp_err_to_name(err));
        s_status = OTA_MGR_FAILED;
        return;
    }

    s_progress_percent = 100;
    s_status = OTA_MGR_READY_TO_REBOOT;
    ESP_LOGI(TAG, "update to v%s verified and staged — reboot to apply", s_available_version);
}

// Reads the companion "<path>.sha256" file into `out` (33 bytes: 32-byte
// digest + implicit NUL from hex_to_sha256's own 64-char requirement).
// Tolerant of "sha256sum"-style "<hex>  <filename>\n" output as well as a
// bare hex file — either way, only the first 64 non-whitespace hex
// characters found matter.
static bool read_sha256_companion(const char *bin_path, uint8_t out[32]) {
    char sha_path[300];
    int n = snprintf(sha_path, sizeof(sha_path), "%s.sha256", bin_path);
    if (n < 0 || n >= (int)sizeof(sha_path)) return false;

    FILE *f = fopen(sha_path, "r");
    if (!f) {
        ESP_LOGW(TAG, "no companion checksum file: %s", sha_path);
        return false;
    }
    char hex[65] = {0};
    // fscanf's %64s stops at whitespace on its own, so this reads exactly
    // the leading hex token regardless of what (if anything) follows it.
    bool got = fscanf(f, "%64s", hex) == 1;
    fclose(f);
    return got && hex_to_sha256(hex, out);
}

bool ota_mgr_apply_from_sd(const char *path) {
    if (!ota_mgr_is_supported()) {
        snprintf(s_error, sizeof(s_error), "no OTA partition available — is device.pcat [device] ota = true?");
        s_status = OTA_MGR_FAILED;
        return false;
    }

    uint8_t expected[32];
    if (!read_sha256_companion(path, expected)) {
        snprintf(s_error, sizeof(s_error), "missing or malformed %s.sha256", path);
        s_status = OTA_MGR_FAILED;
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(s_error, sizeof(s_error), "cannot open %s", path);
        s_status = OTA_MGR_FAILED;
        return false;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size <= 0) {
        fclose(f);
        snprintf(s_error, sizeof(s_error), "empty or unreadable file: %s", path);
        s_status = OTA_MGR_FAILED;
        return false;
    }

    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        fclose(f);
        snprintf(s_error, sizeof(s_error), "no OTA partition available");
        s_status = OTA_MGR_FAILED;
        return false;
    }
    if ((size_t)file_size > update_part->size) {
        fclose(f);
        snprintf(s_error, sizeof(s_error), "%s (%ld B) is larger than the OTA slot (%lu B)",
                 path, file_size, (unsigned long)update_part->size);
        s_status = OTA_MGR_FAILED;
        return false;
    }

    s_status = OTA_MGR_DOWNLOADING;   // reusing the same status the WiFi path
                                       // uses for "writing the image" — see
                                       // ota_mgr_status_t's own doc comment.
    s_progress_percent = 0;
    s_available_version[0] = '\0';    // no manifest for this path — see header

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update_part, (size_t)file_size, &handle);
    if (err != ESP_OK) {
        fclose(f);
        if (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
            // The CURRENTLY RUNNING image hasn't confirmed itself yet (still
            // ESP_OTA_IMG_PENDING_VERIFY — see purr_crash_guard.c's rollback
            // block, which is what clears this on a clean boot). Stacking a
            // second update on top of an unconfirmed one is exactly what
            // this error exists to prevent.
            snprintf(s_error, sizeof(s_error), "current image not yet confirmed valid — reboot first");
        } else {
            snprintf(s_error, sizeof(s_error), "ota begin failed: %s", esp_err_to_name(err));
        }
        s_status = OTA_MGR_FAILED;
        return false;
    }

    uint8_t buf[4096];
    long    written  = 0;
    bool    write_ok = true;
    while (written < file_size) {
        size_t want = sizeof(buf);
        if ((long)want > file_size - written) want = (size_t)(file_size - written);
        size_t got = fread(buf, 1, want, f);
        if (got == 0) { write_ok = false; break; }
        if (esp_ota_write(handle, buf, got) != ESP_OK) { write_ok = false; break; }
        written += (long)got;
        s_progress_percent = (int)(written * 100 / file_size);
    }
    fclose(f);

    if (!write_ok || written != file_size) {
        snprintf(s_error, sizeof(s_error), "read/write failed at %ld/%ld bytes", written, file_size);
        esp_ota_abort(handle);
        s_status = OTA_MGR_FAILED;
        return false;
    }

    // esp_ota_end() validates the image header (magic byte etc.) and frees
    // the handle, but — unlike esp_https_ota_finish() — does NOT switch the
    // boot partition (confirmed against its own doc comment; no reordering
    // trap here the way ota_mgr_apply()'s WiFi path has). Safe to verify
    // after this and still be able to just not call
    // esp_ota_set_boot_partition() on a mismatch.
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        snprintf(s_error, sizeof(s_error), "ota end failed: %s", esp_err_to_name(err));
        s_status = OTA_MGR_FAILED;
        return false;
    }

    s_status = OTA_MGR_VERIFYING;

    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);
    size_t offset  = 0;
    bool   read_ok = true;
    while (offset < (size_t)file_size) {
        size_t chunk = (size_t)file_size - offset;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        if (esp_partition_read(update_part, offset, buf, chunk) != ESP_OK) { read_ok = false; break; }
        mbedtls_sha256_update(&sha_ctx, buf, chunk);
        offset += chunk;
    }
    uint8_t actual[32];
    mbedtls_sha256_finish(&sha_ctx, actual);
    mbedtls_sha256_free(&sha_ctx);

    if (!read_ok) {
        snprintf(s_error, sizeof(s_error), "could not read back written image");
        s_status = OTA_MGR_FAILED;
        return false;
    }
    if (memcmp(actual, expected, sizeof(actual)) != 0) {
        ESP_LOGE(TAG, "SHA-256 mismatch on SD image — refusing to boot it");
        snprintf(s_error, sizeof(s_error), "checksum mismatch — image not trusted");
        s_status = OTA_MGR_FAILED;
        return false;
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        snprintf(s_error, sizeof(s_error), "set_boot_partition failed: %s", esp_err_to_name(err));
        s_status = OTA_MGR_FAILED;
        return false;
    }

    s_progress_percent = 100;
    s_status = OTA_MGR_READY_TO_REBOOT;
    ESP_LOGI(TAG, "SD image verified and staged — reboot to apply");
    return true;
}

// ── Accessors ─────────────────────────────────────────────────────────────────

bool ota_mgr_is_supported(void) { return esp_ota_get_next_update_partition(NULL) != NULL; }

ota_mgr_status_t ota_mgr_status(void)            { return s_status; }
int               ota_mgr_progress_percent(void) { return s_progress_percent; }
const char       *ota_mgr_error(void)            { return s_error; }
const char       *ota_mgr_current_version(void)  { return KITT_VERSION; }
const char       *ota_mgr_available_version(void){ return s_available_version; }

// ── Module lifecycle ──────────────────────────────────────────────────────────

int ota_mgr_init(void) {
    // device.pcat [device] ota = true is what selects the dual-slot partition
    // table this module needs (see purrstrap.py's _generate_sdkconfig()) — a
    // device built without it has no second OTA slot at all. Declining rather
    // than registering into a partition layout that can't support an update
    // is the same "decline, don't half-run" choice wifi_mgr's own
    // SOC_WIFI_SUPPORTED stub makes.
    if (!esp_ota_get_next_update_partition(NULL)) {
        ESP_LOGW(TAG, "no second OTA slot on this device's partition table — declining");
        return PURR_MODULE_INIT_DECLINED;
    }
    load_manifest_url();
    ESP_LOGI(TAG, "init complete (current version %s)", KITT_VERSION);
    return 0;
}

void ota_mgr_deinit(void) {
    s_status = OTA_MGR_IDLE;
}

PURR_MODULE_REGISTER(ota_mgr) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "ota_mgr",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = ota_mgr_init,
    .deinit            = ota_mgr_deinit,
};
