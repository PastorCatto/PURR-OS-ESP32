// server_mgr.c — PURR OS Server Manager wire protocol (responder + caller
// together). See server_mgr.h for the full design.

#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_random.h"
#include "esp_log.h"
#include <mbedtls/sha256.h>
#include <mbedtls/gcm.h>
#include "server_mgr.h"
#include "../pairing/pairing.h"
#include "../proximity_rpc/proximity_rpc.h"
#include "../claw_loader/claw_loader.h"
#include "../app_manager/app_manager.h"
#include "../user_mgr/user_mgr.h"
#include "../wifi_mgr/wifi_mgr.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/core/purr_module.h"

static const char *TAG = "server_mgr";

// ── Wire protocol ─────────────────────────────────────────────────────────
// Next free block after pairing.h's Remote OOBE (0x4000s). 0x3000
// (milkbar's old MILKBAR_ACTION_MSG_SEND) was retired this session along
// with the feature that used it — not reclaimed, same reasoning the OOBE
// block's own comment gives for starting fresh instead.
#define SRVMGR_ACTION_WIFI_STATUS      0x5000   // req: none -> resp: wifi_status_resp_t
#define SRVMGR_ACTION_WIFI_SET         0x5001   // req: wifi_set_req_t -> resp: 1B status (0=fail, 1=ok)
#define SRVMGR_ACTION_APP_UPLOAD_BEGIN 0x5002   // req: upload_begin_req_t -> resp: 1B status
#define SRVMGR_ACTION_APP_UPLOAD_CHUNK 0x5003   // req: 4B offset + up to SRVMGR_UPLOAD_CHUNK_MAX raw bytes -> resp: 1B status
#define SRVMGR_ACTION_APP_UPLOAD_END   0x5004   // req: upload_end_req_t -> resp: 1B status

#define SRVMGR_RPC_TIMEOUT_MS    3000UL   // same as pairing's/milkbar's own RPC timeout convention
#define SRVMGR_UPLOAD_CHUNK_MAX  1900     // comfortably under PROXIMITY_RPC_MAX_MSG (2048, proximity_rpc.h) with the 4B offset header
#define SRVMGR_MAX_UPLOAD_SIZE   (2u * 1024u * 1024u)   // sanity ceiling — well under any realistic personal_root() capacity

typedef struct __attribute__((packed)) {
    char ssid[33];
    char password[64];
} wifi_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t nonce[12];
    uint8_t ciphertext[sizeof(wifi_payload_t)];
    uint8_t tag[16];
} wifi_set_req_t;

typedef struct __attribute__((packed)) {
    uint8_t status;   // server_mgr_wifi_status_t
    char    ssid[33];
    char    ip[16];
} wifi_status_resp_t;

typedef struct __attribute__((packed)) {
    char     name[48];
    uint32_t size;
    uint8_t  tier;
} upload_begin_req_t;

typedef struct __attribute__((packed)) {
    uint32_t checksum;
} upload_end_req_t;

// ── Key derivation ────────────────────────────────────────────────────────
// Same SHA256(secret || nonce || label) construction pairing_module.c's
// own derive_msg_key() uses for USERAUTH/OOBE, with a DIFFERENT label for
// domain separation between the two protocols sharing one Phase A secret
// (pairing_get_shared_secret(), pairing.h) — see that header's own doc
// comment.
static void derive_msg_key(const uint8_t secret[32], const uint8_t nonce[12], uint8_t out[32])
{
    static const char *label = "purr_server_mgr";
    uint8_t buf[32 + 12 + 32];
    size_t label_len = strlen(label);
    memcpy(buf, secret, 32);
    memcpy(buf + 32, nonce, 12);
    memcpy(buf + 32 + 12, label, label_len);
    mbedtls_sha256(buf, 32 + 12 + label_len, out, 0);
}

// A plain rotate-and-add running checksum — NOT cryptographic, and doesn't
// need to be: the wire is already trust-gated (pairing_is_trusted()) and
// the one sensitive payload (WIFI_SET) is separately encrypted above. This
// exists purely to catch transfer corruption across a chunked upload, the
// same "did the bytes arrive intact" scope a TCP-style checksum has.
// Incremental so the responder can fold in each chunk as it arrives
// without ever holding the whole upload in RAM; the caller side computes
// the same way over its already-in-memory buffer in one pass.
static uint32_t checksum_update(uint32_t sum, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) sum = (uint32_t)((sum << 1) | (sum >> 31)) + data[i];
    return sum;
}

// ── WiFi — responder ───────────────────────────────────────────────────────

static bool handle_wifi_status(const uint8_t mac[6], uint16_t action_id,
                                const uint8_t *req, size_t req_len,
                                uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out)
{
    (void)action_id; (void)req; (void)req_len;
    if (resp_cap < sizeof(wifi_status_resp_t)) { *resp_len_out = 0; return false; }

    wifi_status_resp_t resp = {0};
    if (!pairing_is_trusted(mac) || !purr_kernel_get_module("wifi_mgr")) {
        // Untrusted peer, or this device never actually initialised
        // wifi_mgr (not in its own [modules] section — see server_mgr.h's
        // own doc comment on why that's still true for Heltec today).
        // wifi_mgr_status()/wifi_mgr_ip_str() would be SAFE to call either
        // way (their own static state just reads as zeroed/idle), but this
        // is the more honest answer: "unsupported" for a device that never
        // set it up, not a passthrough of state nothing ever populated.
        resp.status = SRVMGR_WIFI_UNSUPPORTED;
    } else {
        // +1: wifi_mgr_status_t's own WIFI_MGR_IDLE=0..WIFI_MGR_FAILED=3
        // shifted up by one to make room for SRVMGR_WIFI_UNSUPPORTED=0 —
        // see server_mgr.h's own enum.
        resp.status = (uint8_t)(wifi_mgr_status() + 1);
        const char *ip = wifi_mgr_ip_str();
        if (ip) snprintf(resp.ip, sizeof(resp.ip), "%s", ip);
        // wifi_mgr.h exposes no "current/target SSID" getter today — left
        // blank rather than guessed at; a real follow-up, not silently faked.
    }
    memcpy(resp_out, &resp, sizeof(resp));
    *resp_len_out = sizeof(resp);
    return true;
}

static bool handle_wifi_set(const uint8_t mac[6], uint16_t action_id,
                             const uint8_t *req, size_t req_len,
                             uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out)
{
    (void)action_id;
    if (req_len != sizeof(wifi_set_req_t) || resp_cap < 1) { *resp_len_out = 0; return false; }
    resp_out[0] = 0;
    *resp_len_out = 1;

    if (!pairing_is_trusted(mac)) return true;
    if (!purr_kernel_get_module("wifi_mgr")) return true;   // see handle_wifi_status()'s own comment

    uint8_t secret[32];
    if (!pairing_get_shared_secret(mac, secret)) return true;

    wifi_set_req_t r;
    memcpy(&r, req, sizeof(r));
    uint8_t msg_key[32];
    derive_msg_key(secret, r.nonce, msg_key);

    wifi_payload_t payload;
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, msg_key, 256);
    int rc = mbedtls_gcm_auth_decrypt(&gcm, sizeof(payload), r.nonce, sizeof(r.nonce), NULL, 0,
                                       r.tag, sizeof(r.tag), r.ciphertext, (uint8_t *)&payload);
    mbedtls_gcm_free(&gcm);
    if (rc != 0) {
        ESP_LOGW(TAG, "wifi set: GCM auth failed (rc=%d)", rc);
        return true;
    }

    char ssid[sizeof(payload.ssid) + 1];
    memcpy(ssid, payload.ssid, sizeof(payload.ssid));
    ssid[sizeof(payload.ssid)] = 0;
    char password[sizeof(payload.password) + 1];
    memcpy(password, payload.password, sizeof(payload.password));
    password[sizeof(payload.password)] = 0;

    if (!ssid[0]) return true;   // an empty SSID is never valid — don't even try

    wifi_mgr_connect(ssid, password);   // "" is wifi_mgr_connect()'s own open-network spelling, not NULL
    ESP_LOGI(TAG, "wifi set: connecting to '%s' (pushed remotely)", ssid);
    resp_out[0] = 1;
    return true;
}

// ── App upload — responder ─────────────────────────────────────────────────
// One transfer at a time (same "single pending op" precedent as pairing's
// own USERAUTH state) — a BEGIN while one is already active, or a CHUNK/
// END for a different mac than the one currently uploading, is refused.

static bool     s_upload_active = false;
static uint8_t  s_upload_mac[6];
static char     s_upload_name[48];
static uint32_t s_upload_size     = 0;
static uint32_t s_upload_received = 0;
static uint32_t s_upload_checksum = 0;
static FILE    *s_upload_file     = NULL;
static char     s_upload_partial_path[300];

// ── Pending approval — one at a time, local to this device ────────────────
static bool    s_pending_active = false;
static char    s_pending_name[48];
static char    s_pending_device_name[20];
static uint8_t s_pending_mac[6];

static void abort_upload(void)
{
    if (s_upload_file) { fclose(s_upload_file); s_upload_file = NULL; }
    if (s_upload_partial_path[0]) remove(s_upload_partial_path);
    s_upload_active = false;
    s_upload_partial_path[0] = 0;
}

static bool handle_upload_begin(const uint8_t mac[6], uint16_t action_id,
                                 const uint8_t *req, size_t req_len,
                                 uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out)
{
    (void)action_id;
    if (req_len != sizeof(upload_begin_req_t) || resp_cap < 1) { *resp_len_out = 0; return false; }
    resp_out[0] = 0;
    *resp_len_out = 1;

    if (!pairing_is_trusted(mac)) return true;
    if (s_upload_active) {
        ESP_LOGW(TAG, "upload begin refused — another transfer already in flight");
        return true;
    }
    if (s_pending_active) {
        ESP_LOGW(TAG, "upload begin refused — an earlier upload is still awaiting local approval");
        return true;
    }

    upload_begin_req_t r;
    memcpy(&r, req, sizeof(r));
    char name[49];
    memcpy(name, r.name, sizeof(r.name));
    name[48] = 0;
    if (!name[0] || r.size == 0 || r.size > SRVMGR_MAX_UPLOAD_SIZE) return true;

    const char *root = claw_loader_personal_root();
    if (!root) {
        ESP_LOGW(TAG, "upload begin refused — no personal-space storage on this device");
        return true;
    }

    char pending_dir[300];
    snprintf(pending_dir, sizeof(pending_dir), "%s/pending", root);
    struct stat st;
    if (stat(root, &st) != 0 && mkdir(root, 0755) != 0) return true;
    if (stat(pending_dir, &st) != 0 && mkdir(pending_dir, 0755) != 0) return true;

    char partial_path[300];
    snprintf(partial_path, sizeof(partial_path), "%s/%s.claw.partial", pending_dir, name);
    FILE *f = fopen(partial_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "upload begin: fopen %s failed", partial_path);
        return true;
    }

    memcpy(s_upload_mac, mac, 6);
    snprintf(s_upload_name, sizeof(s_upload_name), "%s", name);
    s_upload_size     = r.size;
    s_upload_received = 0;
    s_upload_checksum = 0;
    s_upload_file     = f;
    snprintf(s_upload_partial_path, sizeof(s_upload_partial_path), "%s", partial_path);
    s_upload_active = true;

    ESP_LOGI(TAG, "upload begin: '%s' (%u bytes)", name, (unsigned)r.size);
    resp_out[0] = 1;
    return true;
}

static bool handle_upload_chunk(const uint8_t mac[6], uint16_t action_id,
                                 const uint8_t *req, size_t req_len,
                                 uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out)
{
    (void)action_id;
    if (resp_cap < 1) { *resp_len_out = 0; return false; }
    resp_out[0] = 0;
    *resp_len_out = 1;

    if (!pairing_is_trusted(mac)) return true;
    if (!s_upload_active || memcmp(mac, s_upload_mac, 6) != 0) return true;
    if (req_len < 4) return true;

    uint32_t offset;
    memcpy(&offset, req, 4);
    const uint8_t *data = req + 4;
    size_t data_len = req_len - 4;

    // Strictly in-order, no gaps/overwrites — the caller side (below)
    // always sends chunks in order, so anything else means a dropped/
    // duplicated/reordered call this simple protocol has no recovery for.
    // Abort cleanly rather than accept a file that can't be right.
    if (offset != s_upload_received || (uint64_t)s_upload_received + data_len > s_upload_size) {
        ESP_LOGW(TAG, "upload chunk: out-of-order/oversized (offset=%u expected=%u len=%u)",
                 (unsigned)offset, (unsigned)s_upload_received, (unsigned)data_len);
        abort_upload();
        return true;
    }

    size_t written = fwrite(data, 1, data_len, s_upload_file);
    if (written != data_len) {
        ESP_LOGE(TAG, "upload chunk: short write to %s", s_upload_partial_path);
        abort_upload();
        return true;
    }

    s_upload_checksum = checksum_update(s_upload_checksum, data, data_len);
    s_upload_received += (uint32_t)data_len;
    resp_out[0] = 1;
    return true;
}

static bool handle_upload_end(const uint8_t mac[6], uint16_t action_id,
                               const uint8_t *req, size_t req_len,
                               uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out)
{
    (void)action_id;
    if (req_len != sizeof(upload_end_req_t) || resp_cap < 1) { *resp_len_out = 0; return false; }
    resp_out[0] = 0;
    *resp_len_out = 1;

    if (!pairing_is_trusted(mac)) return true;
    if (!s_upload_active || memcmp(mac, s_upload_mac, 6) != 0) return true;

    upload_end_req_t r;
    memcpy(&r, req, sizeof(r));

    bool size_ok     = (s_upload_received == s_upload_size);
    bool checksum_ok = (r.checksum == s_upload_checksum);
    if (s_upload_file) { fclose(s_upload_file); s_upload_file = NULL; }

    if (!size_ok || !checksum_ok) {
        ESP_LOGW(TAG, "upload end: verify failed (size_ok=%d checksum_ok=%d)", size_ok, checksum_ok);
        remove(s_upload_partial_path);
        s_upload_active = false;
        s_upload_partial_path[0] = 0;
        return true;
    }

    const char *root = claw_loader_personal_root();
    bool moved = false;
    if (root) {
        char final_pending_path[300];
        snprintf(final_pending_path, sizeof(final_pending_path), "%s/pending/%s.claw", root, s_upload_name);
        moved = (rename(s_upload_partial_path, final_pending_path) == 0);
        if (!moved) ESP_LOGE(TAG, "upload end: rename %s -> %s failed", s_upload_partial_path, final_pending_path);
    }

    if (moved) {
        char device_name[20] = "?";
        int n = pairing_device_count();
        for (int i = 0; i < n; i++) {
            paired_device_t pd;
            if (pairing_device_at(i, &pd) && memcmp(pd.mac, s_upload_mac, 6) == 0) {
                snprintf(device_name, sizeof(device_name), "%s", pd.name);
                break;
            }
        }
        memcpy(s_pending_mac, s_upload_mac, 6);
        snprintf(s_pending_name, sizeof(s_pending_name), "%s", s_upload_name);
        snprintf(s_pending_device_name, sizeof(s_pending_device_name), "%s", device_name);
        s_pending_active = true;
        ESP_LOGI(TAG, "upload end: '%s' verified, awaiting local approval", s_upload_name);
        resp_out[0] = 1;
    }

    s_upload_active = false;
    s_upload_partial_path[0] = 0;
    return true;
}

// ── App approval — server (local) side ───────────────────────────────────

bool server_mgr_get_pending_app(char *out_name, size_t name_sz,
                                 char *out_device_name, size_t device_name_sz)
{
    if (!s_pending_active) return false;
    if (out_name) snprintf(out_name, name_sz, "%s", s_pending_name);
    if (out_device_name) snprintf(out_device_name, device_name_sz, "%s", s_pending_device_name);
    return true;
}

void server_mgr_approve_app(void)
{
    if (!s_pending_active) return;
    const char *root = claw_loader_personal_root();
    if (!root) { s_pending_active = false; return; }   // storage vanished between upload and approval — nothing to move

    char pending_path[300];
    snprintf(pending_path, sizeof(pending_path), "%s/pending/%s.claw", root, s_pending_name);

    // <root>/<username>/ — the exact shape claw_loader_personal_load()
    // already expects (claw_loader.h). Ensured here directly (mkdir, not
    // claw_loader_personal_add()) since that function's own job is
    // "write these bytes I already have in RAM" — this file is already
    // ON DISK at pending_path, moving it is a rename(), not a read-then-
    // rewrite.
    const char *username = user_mgr_default_username();
    struct stat st;
    if (stat(root, &st) != 0) mkdir(root, 0755);
    char user_dir[300];
    snprintf(user_dir, sizeof(user_dir), "%s/%s", root, username);
    if (stat(user_dir, &st) != 0) mkdir(user_dir, 0755);

    char final_path[300];
    snprintf(final_path, sizeof(final_path), "%s/%s.claw", user_dir, s_pending_name);

    if (rename(pending_path, final_path) == 0) {
        ESP_LOGI(TAG, "app approved: '%s' -> %s", s_pending_name, final_path);
        // Immediately visible over REMOTEAPPS_ACTION_LIST (app_manager_
        // remote.h) to any other connected client — existing mechanism,
        // nothing new needed there. See the "Server Manager" plan doc's
        // own "Files served, not run locally" section.
        app_manager_scan();
    } else {
        ESP_LOGE(TAG, "app approve: rename %s -> %s failed", pending_path, final_path);
    }
    s_pending_active = false;
}

void server_mgr_reject_app(void)
{
    if (!s_pending_active) return;
    const char *root = claw_loader_personal_root();
    if (root) {
        char pending_path[300];
        snprintf(pending_path, sizeof(pending_path), "%s/pending/%s.claw", root, s_pending_name);
        remove(pending_path);
    }
    ESP_LOGI(TAG, "app rejected: '%s'", s_pending_name);
    s_pending_active = false;
}

// ── WiFi — client (caller) side ─────────────────────────────────────────

bool server_mgr_wifi_status(const uint8_t mac[6], server_mgr_wifi_status_t *out_status,
                             char *out_ssid, size_t ssid_sz, char *out_ip, size_t ip_sz)
{
    if (out_ssid && ssid_sz) out_ssid[0] = 0;
    if (out_ip && ip_sz)     out_ip[0]   = 0;
    if (out_status) *out_status = SRVMGR_WIFI_UNSUPPORTED;
    if (!mac) return false;

    uint8_t resp[sizeof(wifi_status_resp_t)]; size_t resp_len = 0;
    bool ok = proximity_rpc_call(mac, SRVMGR_ACTION_WIFI_STATUS, NULL, 0,
                                  resp, sizeof(resp), &resp_len, SRVMGR_RPC_TIMEOUT_MS);
    if (!ok || resp_len != sizeof(wifi_status_resp_t)) return false;

    wifi_status_resp_t r;
    memcpy(&r, resp, sizeof(r));
    if (out_status) *out_status = (server_mgr_wifi_status_t)r.status;
    if (out_ssid) snprintf(out_ssid, ssid_sz, "%s", r.ssid);
    if (out_ip)   snprintf(out_ip, ip_sz, "%s", r.ip);
    return true;
}

bool server_mgr_wifi_set(const uint8_t mac[6], const char *ssid, const char *password)
{
    if (!mac || !ssid || !ssid[0]) return false;

    uint8_t secret[32];
    if (!pairing_get_shared_secret(mac, secret)) return false;

    wifi_payload_t payload = {0};
    snprintf(payload.ssid, sizeof(payload.ssid), "%s", ssid);
    if (password) snprintf(payload.password, sizeof(payload.password), "%s", password);

    wifi_set_req_t req = {0};
    esp_fill_random(req.nonce, sizeof(req.nonce));
    uint8_t msg_key[32];
    derive_msg_key(secret, req.nonce, msg_key);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, msg_key, 256);
    mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, sizeof(payload), req.nonce, sizeof(req.nonce), NULL, 0,
                               (const uint8_t *)&payload, req.ciphertext, sizeof(req.tag), req.tag);
    mbedtls_gcm_free(&gcm);
    memset(&payload, 0, sizeof(payload));   // done with the plaintext copy

    uint8_t resp[4]; size_t resp_len = 0;
    bool ok = proximity_rpc_call(mac, SRVMGR_ACTION_WIFI_SET,
                                  (const uint8_t *)&req, sizeof(req), resp, sizeof(resp), &resp_len,
                                  SRVMGR_RPC_TIMEOUT_MS);
    return ok && resp_len == 1 && resp[0] == 1;
}

// ── App transfer — client (caller) side ──────────────────────────────────

bool server_mgr_app_upload(const uint8_t mac[6], const char *name, const uint8_t *data, size_t len)
{
    if (!mac || !name || !name[0] || !data || len == 0 || len > SRVMGR_MAX_UPLOAD_SIZE) return false;

    upload_begin_req_t begin = {0};
    snprintf(begin.name, sizeof(begin.name), "%s", name);
    begin.size = (uint32_t)len;
    begin.tier = (uint8_t)APP_TIER_PERSONAL;

    uint8_t resp[4]; size_t resp_len = 0;
    bool ok = proximity_rpc_call(mac, SRVMGR_ACTION_APP_UPLOAD_BEGIN,
                                  (const uint8_t *)&begin, sizeof(begin), resp, sizeof(resp), &resp_len,
                                  SRVMGR_RPC_TIMEOUT_MS);
    if (!ok || resp_len != 1 || resp[0] != 1) {
        ESP_LOGE(TAG, "app upload: begin refused for '%s'", name);
        return false;
    }

    uint32_t offset = 0;
    uint32_t checksum = 0;
    uint8_t chunk_req[4 + SRVMGR_UPLOAD_CHUNK_MAX];
    while (offset < (uint32_t)len) {
        size_t chunk_len = len - offset;
        if (chunk_len > SRVMGR_UPLOAD_CHUNK_MAX) chunk_len = SRVMGR_UPLOAD_CHUNK_MAX;

        memcpy(chunk_req, &offset, 4);
        memcpy(chunk_req + 4, data + offset, chunk_len);

        resp_len = 0;
        ok = proximity_rpc_call(mac, SRVMGR_ACTION_APP_UPLOAD_CHUNK,
                                 chunk_req, 4 + chunk_len, resp, sizeof(resp), &resp_len,
                                 SRVMGR_RPC_TIMEOUT_MS);
        if (!ok || resp_len != 1 || resp[0] != 1) {
            ESP_LOGE(TAG, "app upload: chunk at offset %u failed", (unsigned)offset);
            return false;
        }
        checksum = checksum_update(checksum, data + offset, chunk_len);
        offset += (uint32_t)chunk_len;
    }

    upload_end_req_t end = { .checksum = checksum };
    resp_len = 0;
    ok = proximity_rpc_call(mac, SRVMGR_ACTION_APP_UPLOAD_END,
                             (const uint8_t *)&end, sizeof(end), resp, sizeof(resp), &resp_len,
                             SRVMGR_RPC_TIMEOUT_MS);
    if (!ok || resp_len != 1 || resp[0] != 1) {
        ESP_LOGE(TAG, "app upload: end/verify failed for '%s'", name);
        return false;
    }
    ESP_LOGI(TAG, "app upload: '%s' (%u bytes) transferred, awaiting approval on the server", name, (unsigned)len);
    return true;
}

// ── Module lifecycle ──────────────────────────────────────────────────────

int server_mgr_init(void)
{
    memset(s_upload_mac, 0, sizeof(s_upload_mac));
    s_upload_active = false;
    s_upload_partial_path[0] = 0;
    s_pending_active = false;

    // Always-on — see server_mgr.h's own top comment for why this isn't
    // scoped to any app's lifetime.
    proximity_rpc_register(SRVMGR_ACTION_WIFI_STATUS,      handle_wifi_status);
    proximity_rpc_register(SRVMGR_ACTION_WIFI_SET,         handle_wifi_set);
    proximity_rpc_register(SRVMGR_ACTION_APP_UPLOAD_BEGIN, handle_upload_begin);
    proximity_rpc_register(SRVMGR_ACTION_APP_UPLOAD_CHUNK, handle_upload_chunk);
    proximity_rpc_register(SRVMGR_ACTION_APP_UPLOAD_END,   handle_upload_end);

    ESP_LOGI(TAG, "ready");
    return 0;
}

void server_mgr_deinit(void)
{
    proximity_rpc_register(SRVMGR_ACTION_WIFI_STATUS,      NULL);
    proximity_rpc_register(SRVMGR_ACTION_WIFI_SET,         NULL);
    proximity_rpc_register(SRVMGR_ACTION_APP_UPLOAD_BEGIN, NULL);
    proximity_rpc_register(SRVMGR_ACTION_APP_UPLOAD_CHUNK, NULL);
    proximity_rpc_register(SRVMGR_ACTION_APP_UPLOAD_END,   NULL);
    abort_upload();
    s_pending_active = false;
}

// ── Module header ─────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(server_mgr) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "server_mgr",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = server_mgr_init,
    .deinit            = server_mgr_deinit,
};
