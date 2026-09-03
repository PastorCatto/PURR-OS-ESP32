// sig_mgr.c — PURR OS artifact signing/verification. See sig_mgr.h for the
// full design (tier model, trust roots, why the device identity key is
// separate from USER-tier verification, and the one thing this pass could
// not empirically confirm — read that before touching this file).

#include "sig_mgr.h"
#include "purr_module.h"
#include "ed_25519.h"
#include "esp_random.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "sig_mgr";
#define NVS_NS       "purr_sig"
#define NVS_KEY_SEED "dev_seed"

// ── Baked-in trust roots ─────────────────────────────────────────────────────
// Public keys only — see sig_mgr.h's own doc comment: the matching private
// keys never live on a device. Generated for this project via Python's
// `cryptography` package (standard Ed25519, RFC 8032) — NOT with this
// library's own ed25519_create_seed(), which is unusable on-device anyway
// (its two branches are #ifdef _WIN32 CryptGenRandom / else fopen(
// "/dev/urandom") — neither resolves on bare-metal FreeRTOS; the device
// identity key below uses esp_fill_random() instead, same as user_mgr's
// own credential-salt generation).
static const uint8_t OFFICIAL_PUBKEY[32] = {
    0xbb, 0xe3, 0xaa, 0x51, 0x11, 0x95, 0x06, 0x28, 0x13, 0xed, 0x13, 0x37,
    0xa6, 0x07, 0x74, 0x53, 0x3b, 0xbd, 0xe5, 0xe3, 0x77, 0x8f, 0xd0, 0xb2,
    0x1d, 0x0c, 0x2b, 0xee, 0x71, 0x5f, 0xab, 0x90,
};
static const uint8_t DEV_PUBKEY[32] = {
    0x41, 0x64, 0xed, 0x25, 0xf3, 0xa9, 0xd7, 0x00, 0xa5, 0x4d, 0x70, 0xcd,
    0x16, 0x53, 0x55, 0x35, 0xbb, 0xc3, 0xa0, 0xf5, 0xce, 0xbd, 0x2f, 0xf6,
    0x43, 0xb8, 0xe7, 0x10, 0x35, 0xe5, 0x30, 0x3b,
};

// ── Device identity key ──────────────────────────────────────────────────────

static uint8_t s_device_pub[32];
static uint8_t s_device_priv[64];   // orlp/ed25519's own expanded form, not a raw seed
static bool    s_device_key_ready = false;

static void load_or_create_device_key(void) {
    uint8_t seed[32];
    bool got_seed = false;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        size_t len = sizeof(seed);
        if (nvs_get_blob(h, NVS_KEY_SEED, seed, &len) == ESP_OK && len == sizeof(seed)) {
            got_seed = true;
        } else {
            esp_fill_random(seed, sizeof(seed));
            if (nvs_set_blob(h, NVS_KEY_SEED, seed, sizeof(seed)) == ESP_OK) {
                nvs_commit(h);
            }
            got_seed = true;
            ESP_LOGI(TAG, "generated a new device identity key");
        }
        nvs_close(h);
    }

    if (!got_seed) {
        // NVS unavailable — usable for this boot, just not persisted, rather
        // than leaving sig_mgr_sign_with_device_key() permanently broken.
        ESP_LOGW(TAG, "NVS unavailable — device key will not survive reboot");
        esp_fill_random(seed, sizeof(seed));
    }

    ed25519_create_keypair(s_device_pub, s_device_priv, seed);
    s_device_key_ready = true;
}

bool sig_mgr_device_pubkey(uint8_t out[32]) {
    if (!s_device_key_ready) return false;
    memcpy(out, s_device_pub, 32);
    return true;
}

bool sig_mgr_sign_with_device_key(const uint8_t *data, size_t len, uint8_t out_sig[64]) {
    if (!s_device_key_ready) return false;
    ed25519_sign(out_sig, data, len, s_device_pub, s_device_priv);
    return true;
}

// ── Classification ───────────────────────────────────────────────────────────

const char *sig_tier_name(sig_tier_t tier) {
    switch (tier) {
        case SIG_TIER_UNSIGNED: return "unsigned";
        case SIG_TIER_TAMPERED: return "TAMPERED";
        case SIG_TIER_USER:     return "user-signed";
        case SIG_TIER_DEV:      return "dev-signed";
        case SIG_TIER_OFFICIAL: return "official";
        default:                return "?";
    }
}

static bool sha256_of_file(const char *path, uint8_t out[32]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    uint8_t buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        mbedtls_sha256_update(&ctx, buf, n);
    }
    fclose(f);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
    return true;
}

static bool read_exact(const char *path, uint8_t *out, size_t expected_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(out, 1, expected_len, f);
    fclose(f);
    return n == expected_len;
}

// Shared by both public entry points below — takes the artifact's hash
// (however the caller got it) plus the path used purely to locate
// "<path>.sig"/"<path>.pub". Neither classify() nor classify_buffer() do
// anything except get `hash` and hand it here.
static sig_tier_t classify_from_hash(const uint8_t hash[32], const char *path) {
    char companion[300];
    if (snprintf(companion, sizeof(companion), "%s.sig", path) >= (int)sizeof(companion)) {
        return SIG_TIER_UNSIGNED;
    }
    uint8_t sig[64];
    if (!read_exact(companion, sig, sizeof(sig))) return SIG_TIER_UNSIGNED;   // no .sig at all

    if (ed25519_verify(sig, hash, 32, OFFICIAL_PUBKEY)) return SIG_TIER_OFFICIAL;
    if (ed25519_verify(sig, hash, 32, DEV_PUBKEY))      return SIG_TIER_DEV;

    if (snprintf(companion, sizeof(companion), "%s.pub", path) >= (int)sizeof(companion)) {
        return SIG_TIER_UNSIGNED;
    }
    uint8_t pub[32];
    if (!read_exact(companion, pub, sizeof(pub))) {
        // .sig exists and matches neither trust root, and there is no
        // co-located key to even attempt verifying it against — nothing
        // to call this but unsigned; there is no PAIR here to call tampered.
        return SIG_TIER_UNSIGNED;
    }

    // A .sig + co-located .pub is a self-consistent pair by construction
    // (that is what "user-signed" IS) — if it does not verify, the
    // artifact changed after signing. See sig_mgr.h's own doc comment on
    // why this is reported distinctly from UNSIGNED.
    return ed25519_verify(sig, hash, 32, pub) ? SIG_TIER_USER : SIG_TIER_TAMPERED;
}

sig_tier_t sig_mgr_classify(const char *path) {
    if (!path) return SIG_TIER_UNSIGNED;
    uint8_t hash[32];
    if (!sha256_of_file(path, hash)) return SIG_TIER_UNSIGNED;   // artifact itself unreadable
    return classify_from_hash(hash, path);
}

sig_tier_t sig_mgr_classify_buffer(const uint8_t *data, size_t len, const char *path) {
    if (!path || (!data && len)) return SIG_TIER_UNSIGNED;
    uint8_t hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    if (len) mbedtls_sha256_update(&ctx, data, len);
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    return classify_from_hash(hash, path);
}

// ── Module lifecycle ──────────────────────────────────────────────────────────

int sig_mgr_init(void) {
    load_or_create_device_key();
    ESP_LOGI(TAG, "init complete");
    return 0;
}

void sig_mgr_deinit(void) {
    s_device_key_ready = false;
}

PURR_MODULE_REGISTER(sig_mgr) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "sig_mgr",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = sig_mgr_init,
    .deinit            = sig_mgr_deinit,
};
