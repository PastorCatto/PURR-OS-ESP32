// pairing_module.c — PURR_MOD_SYSTEM registration + pairing state machine
// + NVS persistence, riding on proximity_module.c's shared ESP-NOW frame
// dispatch (PROXIMITY_FRAME_PAIRING).
//
// State lives entirely in RAM except the durable "who am I paired with"
// fact, which follows the mesh_router.c/mc_contacts.cpp dirty-flag + small
// persist-task NVS pattern used elsewhere in this codebase. The frame
// handler (pairing_on_frame()) runs on proximity_task()'s own thread,
// which may have a PSRAM-backed stack — it only ever touches in-RAM state
// and sets a dirty flag, never NVS directly (same PSRAM-stack-vs-flash-
// cache-disable hazard documented in meshcore_module.cpp applies here);
// the actual NVS write happens on this module's own dedicated internal-
// RAM-stack pairing_task(), mirroring mc_persist_task().

#include "pairing.h"
#include "../proximity/proximity.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/core/purr_module.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// mbedtls_ecp_point's X/Y/Z fields are wrapped in MBEDTLS_PRIVATE() in this
// mbedtls version (accessor macros required instead of direct .X/.Y/.Z
// access by default). Must be defined before any mbedtls header is
// included — same requirement, same fix, as meshtastic's mesh_radio.c
// already applies for its own Curve25519 ECDH code.
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include <mbedtls/ecp.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/sha256.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "pairing";

#define PAIRING_NVS_NS      "purr_pairing"
#define PAIRING_TIMEOUT_MS  60000UL   // a pending request/confirm with no action expires

typedef enum {
    PAIRING_MSG_REQUEST = 1,   // initiator -> responder: {code, sender's name, initiator's pubkey}
    PAIRING_MSG_ACCEPT  = 2,   // responder -> initiator: {sender's name, responder's pubkey}
    PAIRING_MSG_REJECT  = 3,   // responder -> initiator
    PAIRING_MSG_UNPAIR  = 4,   // either -> other, best-effort notice
} pairing_msg_type_t;

// pubkey — X25519 ephemeral public key (see the "ECDH" section below).
// Present (nonzero) on REQUEST/ACCEPT; zero-filled and ignored on REJECT/
// UNPAIR, which need no key material. Growing this struct is fine — unlike
// paired_device_t (see this file's own load_paired()/save_paired()
// comment), pairing_wire_msg_t is never persisted, only ever put on the
// wire fresh each time, so there's no old-blob-format hazard here. 55
// bytes total, comfortably under PROXIMITY_MAX_PAYLOAD (249).
typedef struct __attribute__((packed)) {
    uint8_t  msg_type;
    uint16_t code;
    char     name[20];
    uint8_t  pubkey[32];
} pairing_wire_msg_t;

static pairing_state_t s_state = PAIRING_STATE_NONE;

static uint8_t  s_pending_mac[6];
static char     s_pending_name[20];
static uint16_t s_pending_code;
static uint32_t s_pending_started_ms;

// ── ECDH (Phase A of the remote-login work) ─────────────────────────────────
// A fresh X25519 keypair is generated for every negotiation (pairing_start()
// on the initiator, on receiving PAIRING_MSG_REQUEST on the responder) —
// ephemeral, never persisted; only the DERIVED shared secret survives past
// the handshake (see s_paired_secrets[] below). s_eph_priv/s_eph_pub and
// s_pending_peer_pub are scoped to one negotiation at a time, same lifetime
// as s_pending_mac/s_pending_name above — valid from PENDING_* through to
// PAIRED (or NONE on cancel/reject/timeout), overwritten by the next
// pairing_start()/incoming REQUEST.
static uint8_t s_eph_priv[32];
static uint8_t s_eph_pub[32];
static uint8_t s_pending_peer_pub[32];

// Same mbedtls calling convention as meshtastic's mesh_radio.c
// (generate_curve25519_keypair()/mesh_radio_ecdh_shared()) — mirrored
// rather than shared, see this file's CMakeLists.txt comment on why
// pairing can't depend on meshtastic. RFC 7748 X25519 u-coordinate
// encoding is little-endian; mbedtls_mpi read/write _binary_le is the
// correct pairing for Curve25519 specifically (curve-agnostic API, but
// this is what makes it produce the standard X25519 wire encoding).
static bool ecdh_generate_ephemeral(uint8_t priv_out[32], uint8_t pub_out[32])
{
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point Q;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    static const char *pers = "purr_pairing_keygen";
    bool ok = false;
    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                               (const unsigned char *)pers, strlen(pers)) != 0) goto done;
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) goto done;
    if (mbedtls_ecp_gen_keypair(&grp, &d, &Q, mbedtls_ctr_drbg_random, &ctr_drbg) != 0) goto done;
    if (mbedtls_mpi_write_binary_le(&d, priv_out, 32) != 0) goto done;
    if (mbedtls_mpi_write_binary_le(&Q.X, pub_out, 32) != 0) goto done;
    ok = true;

done:
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

// Raw ECDH output gets SHA-256'd before use as the shared secret — same
// "don't use the raw ECDH output directly" precedent mesh_radio_ecdh_shared()
// already follows (matching real Meshtastic's CryptoEngine::hash()).
static bool ecdh_compute_shared(const uint8_t priv[32], const uint8_t their_pub[32], uint8_t shared_out[32])
{
    mbedtls_ecp_group grp;
    mbedtls_mpi d, shared;
    mbedtls_ecp_point Q;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&shared);
    mbedtls_ecp_point_init(&Q);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    bool ok = false;
    uint8_t raw_shared[32];
    // f_rng must not be NULL here (mbedtls_ecp_mul_restartable() rejects it
    // unconditionally, even for Curve25519 — see mesh_radio.c's own comment
    // on this exact gotcha, confirmed live there). X25519 itself stays
    // deterministic either way; this RNG only feeds mbedtls's internal
    // scalar-blinding side-channel countermeasure.
    static const char *pers = "purr_pairing_ecdh";
    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                               (const unsigned char *)pers, strlen(pers)) != 0) goto done;
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) goto done;
    if (mbedtls_mpi_read_binary_le(&d, priv, 32) != 0) goto done;
    // Curve25519 points in mbedtls are projective (X, Z) — Y is unused for
    // the Montgomery x25519 ladder. Z=1 marks this as an affine input point.
    if (mbedtls_mpi_read_binary_le(&Q.X, their_pub, 32) != 0) goto done;
    if (mbedtls_mpi_lset(&Q.Z, 1) != 0) goto done;
    if (mbedtls_ecdh_compute_shared(&grp, &shared, &Q, &d, mbedtls_ctr_drbg_random, &ctr_drbg) != 0) goto done;
    if (mbedtls_mpi_write_binary_le(&shared, raw_shared, 32) != 0) goto done;
    if (mbedtls_sha256(raw_shared, 32, shared_out, 0) != 0) goto done;
    ok = true;

done:
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&shared);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

// The human-facing code, upgraded from a bare esp_random()%10000 (no crypto
// binding — see this file's git history) to SHA256(initiator_pub ||
// responder_pub) truncated to 4 digits, computed independently on each
// side from the same fixed role order. A real integrity check tied to the
// actual exchanged keys: if either pubkey was substituted in transit
// (rather than merely relayed as-is), both sides land on different values.
// Still one-sided in this pass — see this file's own pairing_confirm()
// comment on why — but no longer purely decorative on the side that DOES
// check it.
static uint16_t compute_pairing_code(const uint8_t initiator_pub[32], const uint8_t responder_pub[32])
{
    uint8_t buf[64];
    memcpy(buf, initiator_pub, 32);
    memcpy(buf + 32, responder_pub, 32);
    uint8_t digest[32];
    mbedtls_sha256(buf, sizeof(buf), digest, 0);
    uint16_t v = ((uint16_t)digest[0] << 8) | digest[1];
    return v % 10000;
}

// Trust-list-parallel shared-secret storage — see this file's own
// load_paired()/save_paired() comment on why this can't just be a new
// field on paired_device_t (no format-version key to detect an old blob;
// growing that struct risks silently dropping every existing pairing on
// upgrade). Same treatment as s_home_base_mac below: its own NVS key,
// index-parallel to s_paired_devices[] and kept in sync with it by
// add_or_update_paired()/remove_paired() below.
static uint8_t s_paired_secrets[PAIRING_MAX_DEVICES][32];

// Trust list — see pairing.h's top comment. Index 0 is what the two
// existing single-device callers (nearby_app.c, oled_ui_module.c) see
// through pairing_is_paired()/pairing_get_paired_mac()/_name()/
// pairing_unpair(); new multi-device-aware code uses the indexed API.
static paired_device_t s_paired_devices[PAIRING_MAX_DEVICES];
static int      s_paired_count = 0;
static volatile bool s_dirty = false;

// Home base — see pairing.h's comment. Own NVS key, not a paired_device_t
// field (avoids the blob-versioning risk documented there). Persisted via
// the same s_dirty/pairing_task() path as the trust list itself, so
// PSRAM-stack callers (UI) never touch NVS directly.
static uint8_t s_home_base_mac[6];
static bool    s_home_base_set = false;

// -1 if not found. Internal only — callers use pairing_is_trusted()/
// pairing_device_at().
static int find_paired_idx(const uint8_t mac[6]) {
    for (int i = 0; i < s_paired_count; i++) {
        if (memcmp(s_paired_devices[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

// Adds mac/name to the trust list, or updates the name in place if mac is
// already present (re-pairing with a device whose display name changed).
// Silently drops the add if the list is full — same "cap and warn" shape
// as mesh_router.c's MAX_NODES handling, not a hard error the caller needs
// to check for (a full trust list is a real but rare edge case, not
// something worth threading a bool return through every ACCEPT/CONFIRM
// call site for).
//
// secret may be NULL to leave whatever's already stored at this index
// untouched (re-pairing with an already-trusted mac only updates the name
// today; nothing currently re-pairs an existing device through this path
// with a NEW secret without going through remove_paired() first, but the
// NULL option exists so a future caller could).
static void add_or_update_paired(const uint8_t mac[6], const char *name, const uint8_t secret[32]) {
    int idx = find_paired_idx(mac);
    if (idx < 0) {
        if (s_paired_count >= PAIRING_MAX_DEVICES) {
            ESP_LOGW(TAG, "trust list full (%d devices) — not adding new pairing", PAIRING_MAX_DEVICES);
            return;
        }
        idx = s_paired_count++;
        memcpy(s_paired_devices[idx].mac, mac, 6);
    }
    strncpy(s_paired_devices[idx].name, name ? name : "", sizeof(s_paired_devices[idx].name) - 1);
    s_paired_devices[idx].name[sizeof(s_paired_devices[idx].name) - 1] = 0;
    if (secret) {
        memcpy(s_paired_secrets[idx], secret, 32);
    }
    s_dirty = true;
}

// Removes mac from the trust list if present, compacting the array (order
// doesn't matter — nothing depends on trust-list index stability across a
// forget(), only within a single enumeration pass). s_paired_secrets[] is
// swap-compacted right alongside s_paired_devices[] so the two arrays stay
// index-parallel.
static bool remove_paired(const uint8_t mac[6]) {
    int idx = find_paired_idx(mac);
    if (idx < 0) return false;
    s_paired_devices[idx] = s_paired_devices[s_paired_count - 1];
    memcpy(s_paired_secrets[idx], s_paired_secrets[s_paired_count - 1], 32);
    s_paired_count--;
    // A forgotten device can't stay the home base — leaving it set would
    // point homebase.c's presence watcher at a MAC that's no longer trusted.
    if (s_home_base_set && memcmp(s_home_base_mac, mac, 6) == 0) {
        s_home_base_set = false;
    }
    s_dirty = true;
    return true;
}

// Internal accessor for the Phase B/C RPC handlers (not exposed via
// pairing.h — the secret never needs to leave this file, see this file's
// own s_paired_secrets[] comment). -1 device not found.
static bool get_paired_secret(const uint8_t mac[6], uint8_t out[32]) {
    int idx = find_paired_idx(mac);
    if (idx < 0) return false;
    memcpy(out, s_paired_secrets[idx], 32);
    return true;
}

static TaskHandle_t s_task = NULL;

// ── NVS ──────────────────────────────────────────────────────────────────

// Whole trust list as one blob, same shape as mesh_router.c's own
// "nodes" blob — a bounded fixed-layout array is simpler than per-device
// NVS keys and the list is small (PAIRING_MAX_DEVICES=8, ~26 bytes each).
static void load_paired(void) {
    nvs_handle_t h;
    if (nvs_open(PAIRING_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t count = 0;
    if (nvs_get_u8(h, "count", &count) == ESP_OK && count > 0) {
        if (count > PAIRING_MAX_DEVICES) count = PAIRING_MAX_DEVICES;
        size_t blob_len = sizeof(paired_device_t) * (size_t)count;
        if (nvs_get_blob(h, "devices", s_paired_devices, &blob_len) == ESP_OK) {
            s_paired_count = count;
        }
    }

    // Shared secrets — own key, index-parallel to "devices" above. A
    // missing/short "secrets" blob (upgrading from a build that predates
    // this) just leaves s_paired_secrets zeroed for existing pairings —
    // harmless: Phase B/C simply treats a zeroed secret as "not
    // established," same as an unpaired device, until each is re-paired.
    if (s_paired_count > 0) {
        size_t sec_len = sizeof(s_paired_secrets[0]) * (size_t)s_paired_count;
        nvs_get_blob(h, "secrets", s_paired_secrets, &sec_len);
    }

    size_t hb_len = sizeof(s_home_base_mac);
    if (nvs_get_blob(h, "home_base", s_home_base_mac, &hb_len) == ESP_OK && hb_len == sizeof(s_home_base_mac)) {
        s_home_base_set = true;
    }
    nvs_close(h);
}

static void save_paired(void) {
    nvs_handle_t h;
    if (nvs_open(PAIRING_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "count", (uint8_t)s_paired_count);
    if (s_paired_count > 0) {
        nvs_set_blob(h, "devices", s_paired_devices, sizeof(paired_device_t) * (size_t)s_paired_count);
        nvs_set_blob(h, "secrets", s_paired_secrets, sizeof(s_paired_secrets[0]) * (size_t)s_paired_count);
    }
    if (s_home_base_set) {
        nvs_set_blob(h, "home_base", s_home_base_mac, sizeof(s_home_base_mac));
    } else {
        nvs_erase_key(h, "home_base");   // best-effort; ESP_ERR_NVS_NOT_FOUND if already absent
    }
    nvs_commit(h);
    nvs_close(h);
}

// ── Frame handling ───────────────────────────────────────────────────────
// Runs on proximity_task() — see this file's header comment. No NVS here.

// pubkey may be NULL for REJECT/UNPAIR, which carry no key material.
static void send_msg(const uint8_t *mac, pairing_msg_type_t type, const char *name, const uint8_t pubkey[32]) {
    pairing_wire_msg_t msg = {0};
    msg.msg_type = (uint8_t)type;
    msg.code = (type == PAIRING_MSG_REQUEST) ? s_pending_code : 0;
    if (name) {
        strncpy(msg.name, name, sizeof(msg.name) - 1);
    }
    if (pubkey) {
        memcpy(msg.pubkey, pubkey, sizeof(msg.pubkey));
    }
    // Diagnostic: current WiFi channel at send time — compare against the
    // peer's own logged channel to confirm/rule out a channel mismatch
    // (see on_espnow_send()'s comment in proximity_module.c).
    uint8_t chan = 0; wifi_second_chan_t second;
    esp_wifi_get_channel(&chan, &second);
    ESP_LOGI(TAG, "sending msg_type=%d to %02X:%02X:%02X:%02X:%02X:%02X on channel %d",
             (int)type, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (int)chan);
    bool ok = proximity_send_unicast(mac, PROXIMITY_FRAME_PAIRING, (const uint8_t *)&msg, sizeof(msg));
    if (!ok) {
        ESP_LOGW(TAG, "proximity_send_unicast() returned false for msg_type=%d", (int)type);
    }
}

static void pairing_on_frame(const uint8_t *mac, int8_t rssi, const uint8_t *payload, size_t len) {
    (void)rssi;
    if (len != sizeof(pairing_wire_msg_t)) {
        ESP_LOGW(TAG, "pairing frame from %02X:%02X:%02X:%02X:%02X:%02X wrong size (%u, expected %u)",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 (unsigned)len, (unsigned)sizeof(pairing_wire_msg_t));
        return;
    }

    pairing_wire_msg_t msg;
    memcpy(&msg, payload, sizeof(msg));
    char name[sizeof(msg.name) + 1];
    memcpy(name, msg.name, sizeof(msg.name));
    name[sizeof(msg.name)] = 0;

    uint8_t chan = 0; wifi_second_chan_t second;
    esp_wifi_get_channel(&chan, &second);
    ESP_LOGI(TAG, "recv msg_type=%d from %02X:%02X:%02X:%02X:%02X:%02X (\"%s\") on channel %d, our state=%d",
             (int)msg.msg_type, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], name, (int)chan, (int)s_state);

    switch ((pairing_msg_type_t)msg.msg_type) {
    case PAIRING_MSG_REQUEST: {
        // Drop only if we're mid-negotiation with someone else — an
        // already-PAIRED state (with one or more OTHER devices) doesn't
        // block accepting a new request; see pairing.h's multi-device
        // trust-list comment. No request queueing/stealing an
        // in-progress negotiation, though.
        if (s_state != PAIRING_STATE_NONE && s_state != PAIRING_STATE_PAIRED) return;
        memcpy(s_pending_mac, mac, 6);
        strncpy(s_pending_name, name, sizeof(s_pending_name) - 1);
        s_pending_name[sizeof(s_pending_name) - 1] = 0;
        memcpy(s_pending_peer_pub, msg.pubkey, 32);

        // Our own ephemeral keypair for THIS negotiation — generated now,
        // not reused from any prior attempt (see this file's own
        // s_eph_priv/s_eph_pub comment). If generation fails (should only
        // happen under real RNG/entropy trouble), fall back to the
        // pre-ECDH behavior — a random display code, no derivable shared
        // secret — rather than pairing with an all-zero key silently.
        if (ecdh_generate_ephemeral(s_eph_priv, s_eph_pub)) {
            s_pending_code = compute_pairing_code(s_pending_peer_pub, s_eph_pub);
        } else {
            ESP_LOGE(TAG, "ECDH keypair generation failed — pairing code will not be a real integrity check");
            memset(s_eph_priv, 0, sizeof(s_eph_priv));
            memset(s_eph_pub, 0, sizeof(s_eph_pub));
            s_pending_code = (uint16_t)(esp_random() % 10000);
        }
        s_pending_started_ms = (uint32_t)purr_kernel_uptime_ms();
        s_state = PAIRING_STATE_PENDING_INCOMING;

        char notify_body[64];
        snprintf(notify_body, sizeof(notify_body), "Pairing request from %s", name);
        purr_kernel_notify("Nearby device", notify_body, "pairing");
        break;
    }

    case PAIRING_MSG_ACCEPT: {
        if (s_state != PAIRING_STATE_PENDING_OUTGOING) return;
        if (memcmp(mac, s_pending_mac, 6) != 0) return;
        // s_eph_priv/pub here are OUR OWN keypair, generated in
        // pairing_start() below — msg.pubkey is the responder's. Derive
        // the same shared secret pairing_confirm() computed on their side.
        uint8_t secret[32];
        bool have_secret = ecdh_compute_shared(s_eph_priv, msg.pubkey, secret);
        if (!have_secret) {
            ESP_LOGE(TAG, "ECDH shared-secret derivation failed on ACCEPT — pairing without a session key");
            memset(secret, 0, sizeof(secret));
        }
        // We're the initiator here (s_eph_pub = ours), so the same
        // (initiator_pub, responder_pub) role order compute_pairing_code()
        // uses on the responder's side above. Not shown in any UI today
        // (see this file's own PAIRING_MSG_REQUEST comment on why the
        // initiator's dialog can't show it in time to matter) — logged so
        // a two-device debugging session can still confirm both sides
        // landed on the same value.
        ESP_LOGI(TAG, "pairing code (initiator view) = %04u",
                 (unsigned)compute_pairing_code(s_eph_pub, msg.pubkey));
        add_or_update_paired(mac, name, secret);
        s_state = PAIRING_STATE_PAIRED;
        break;
    }

    case PAIRING_MSG_REJECT:
        if (s_state != PAIRING_STATE_PENDING_OUTGOING) return;
        if (memcmp(mac, s_pending_mac, 6) != 0) return;
        s_state = PAIRING_STATE_NONE;
        break;

    case PAIRING_MSG_UNPAIR:
        if (!remove_paired(mac)) return;
        if (s_state == PAIRING_STATE_PAIRED) s_state = PAIRING_STATE_NONE;
        break;

    default:
        break;
    }
}

// ── Task ─────────────────────────────────────────────────────────────────
// Internal-RAM stack (plain xTaskCreate, no WithCaps) — the only place in
// this module allowed to touch NVS.

static void pairing_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if ((s_state == PAIRING_STATE_PENDING_OUTGOING || s_state == PAIRING_STATE_PENDING_INCOMING) &&
            (uint32_t)purr_kernel_uptime_ms() - s_pending_started_ms >= PAIRING_TIMEOUT_MS) {
            ESP_LOGI(TAG, "pending pairing timed out");
            s_state = PAIRING_STATE_NONE;
        }

        if (s_dirty) {
            s_dirty = false;
            save_paired();
        }
    }
}

// ── Public API ───────────────────────────────────────────────────────────

pairing_state_t pairing_get_state(void) { return s_state; }

bool pairing_start(const uint8_t mac[6], const char *peer_name) {
    // PAIRED doesn't block starting a new negotiation with a DIFFERENT
    // device — only being mid-negotiation already does. See pairing.h's
    // multi-device trust-list comment.
    if (!mac || (s_state != PAIRING_STATE_NONE && s_state != PAIRING_STATE_PAIRED)) return false;

    memcpy(s_pending_mac, mac, 6);
    s_pending_name[0] = 0;
    if (peer_name) {
        strncpy(s_pending_name, peer_name, sizeof(s_pending_name) - 1);
    }
    // Decorative only on this side — see this file's own PAIRING_MSG_ACCEPT
    // comment on why the initiator can't derive the real (pubkey-based)
    // code before the responder's pubkey arrives, and why nothing displays
    // this value today. Unchanged from before ECDH: same
    // esp_random()%10000 this field has always held during this window.
    s_pending_code = (uint16_t)(esp_random() % 10000);
    s_pending_started_ms = (uint32_t)purr_kernel_uptime_ms();
    s_state = PAIRING_STATE_PENDING_OUTGOING;

    // Fresh ephemeral keypair for this negotiation (see this file's own
    // s_eph_priv/s_eph_pub comment). Same fallback as the REQUEST handler:
    // proceed with an all-zero pubkey rather than fail the whole pairing
    // attempt outright if RNG/entropy trouble strikes — the responder
    // still gets a (non-cryptographic) pairing code either way, matching
    // this module's pre-ECDH behavior exactly in that failure case.
    if (!ecdh_generate_ephemeral(s_eph_priv, s_eph_pub)) {
        ESP_LOGE(TAG, "ECDH keypair generation failed — pairing without a session key");
        memset(s_eph_priv, 0, sizeof(s_eph_priv));
        memset(s_eph_pub, 0, sizeof(s_eph_pub));
    }

    char own_name[20];
    proximity_get_own_name(own_name, sizeof(own_name));
    send_msg(mac, PAIRING_MSG_REQUEST, own_name, s_eph_pub);
    return true;
}

void pairing_cancel(void) {
    if (s_state == PAIRING_STATE_PENDING_OUTGOING) s_state = PAIRING_STATE_NONE;
}

bool pairing_get_pending_code(char *out, size_t out_len) {
    if (s_state != PAIRING_STATE_PENDING_INCOMING && s_state != PAIRING_STATE_PENDING_OUTGOING) return false;
    if (!out || out_len == 0) return false;
    snprintf(out, out_len, "%04u", (unsigned)s_pending_code);
    return true;
}

bool pairing_get_pending_peer_name(char *out, size_t out_len) {
    if (s_state != PAIRING_STATE_PENDING_INCOMING && s_state != PAIRING_STATE_PENDING_OUTGOING) return false;
    if (!out || out_len == 0) return false;
    snprintf(out, out_len, "%s", s_pending_name);
    return true;
}

void pairing_confirm(void) {
    if (s_state != PAIRING_STATE_PENDING_INCOMING) return;

    // s_eph_priv/s_pending_peer_pub were set together when the REQUEST
    // arrived (pairing_on_frame()) — same derivation the initiator performs
    // on ACCEPT, just with our/their roles swapped.
    uint8_t secret[32];
    bool have_secret = ecdh_compute_shared(s_eph_priv, s_pending_peer_pub, secret);
    if (!have_secret) {
        ESP_LOGE(TAG, "ECDH shared-secret derivation failed on confirm — pairing without a session key");
        memset(secret, 0, sizeof(secret));
    }
    add_or_update_paired(s_pending_mac, s_pending_name, secret);
    s_state = PAIRING_STATE_PAIRED;

    char own_name[20];
    proximity_get_own_name(own_name, sizeof(own_name));
    send_msg(s_pending_mac, PAIRING_MSG_ACCEPT, own_name, s_eph_pub);
}

void pairing_reject(void) {
    if (s_state != PAIRING_STATE_PENDING_INCOMING) return;
    send_msg(s_pending_mac, PAIRING_MSG_REJECT, NULL, NULL);
    s_state = PAIRING_STATE_NONE;
}

bool pairing_is_paired(void) { return s_paired_count > 0; }

bool pairing_get_paired_mac(uint8_t out_mac[6]) {
    if (s_paired_count == 0 || !out_mac) return false;
    memcpy(out_mac, s_paired_devices[0].mac, 6);
    return true;
}

bool pairing_get_paired_name(char *out, size_t out_len) {
    if (s_paired_count == 0 || !out || out_len == 0) return false;
    snprintf(out, out_len, "%s", s_paired_devices[0].name);
    return true;
}

void pairing_unpair(void) {
    if (s_paired_count == 0) return;
    pairing_forget(s_paired_devices[0].mac);
}

int pairing_device_count(void) { return s_paired_count; }

bool pairing_device_at(int idx, paired_device_t *out) {
    if (idx < 0 || idx >= s_paired_count || !out) return false;
    *out = s_paired_devices[idx];
    return true;
}

bool pairing_is_trusted(const uint8_t mac[6]) {
    if (!mac) return false;
    return find_paired_idx(mac) >= 0;
}

void pairing_forget(const uint8_t mac[6]) {
    if (!mac || find_paired_idx(mac) < 0) return;
    send_msg(mac, PAIRING_MSG_UNPAIR, NULL, NULL);
    remove_paired(mac);
    if (s_state == PAIRING_STATE_PAIRED) s_state = PAIRING_STATE_NONE;
}

bool pairing_set_home_base(const uint8_t mac[6]) {
    if (!mac || find_paired_idx(mac) < 0) return false;
    memcpy(s_home_base_mac, mac, 6);
    s_home_base_set = true;
    s_dirty = true;
    return true;
}

bool pairing_get_home_base(uint8_t out_mac[6]) {
    if (!s_home_base_set || !out_mac) return false;
    memcpy(out_mac, s_home_base_mac, 6);
    return true;
}

bool pairing_is_home_base(const uint8_t mac[6]) {
    if (!mac || !s_home_base_set) return false;
    return memcmp(mac, s_home_base_mac, 6) == 0;
}

void pairing_clear_home_base(void) {
    if (!s_home_base_set) return;
    s_home_base_set = false;
    s_dirty = true;
}

// ── TEMPORARY selftest ───────────────────────────────────────────────────
// Proves the ECDH math itself is self-consistent — both "sides" computed
// in this one process, since a real two-device handshake needs a second
// physical board to verify end-to-end (see this session's own remote-login
// plan doc). Simulates exactly what pairing_start()/pairing_confirm() do:
// two independent ephemeral keypairs, each side deriving the shared secret
// from ITS OWN private key + the OTHER's public key, and confirms they
// land on the identical 32 bytes — plus confirms compute_pairing_code()
// is deterministic for the same two pubkeys. Called from a temporary boot
// hook, then reverted once confirmed — same discipline this branch's
// claw_loader work already established.
void pairing_selftest_ecdh(void) {
    uint8_t privA[32], pubA[32], privB[32], pubB[32];
    bool genA = ecdh_generate_ephemeral(privA, pubA);
    bool genB = ecdh_generate_ephemeral(privB, pubB);
    ESP_LOGI(TAG, "selftest: keygen A=%d B=%d", genA, genB);
    if (!genA || !genB) { ESP_LOGE(TAG, "selftest: SELFTEST FAIL (keygen)"); return; }

    uint8_t sharedA[32], sharedB[32];
    bool okA = ecdh_compute_shared(privA, pubB, sharedA);   // A's view: my priv + B's pub
    bool okB = ecdh_compute_shared(privB, pubA, sharedB);   // B's view: my priv + A's pub
    ESP_LOGI(TAG, "selftest: shared-secret compute A=%d B=%d", okA, okB);
    if (!okA || !okB) { ESP_LOGE(TAG, "selftest: SELFTEST FAIL (compute_shared)"); return; }

    bool secrets_match = memcmp(sharedA, sharedB, 32) == 0;
    ESP_LOGI(TAG, "selftest: shared secrets match = %d", secrets_match);

    uint16_t codeA = compute_pairing_code(pubA, pubB);   // A is initiator here
    uint16_t codeB = compute_pairing_code(pubA, pubB);   // same call B would make with the same two pubkeys
    bool codes_match = codeA == codeB;
    ESP_LOGI(TAG, "selftest: pairing code A=%04u B=%04u match=%d", (unsigned)codeA, (unsigned)codeB, codes_match);

    bool pass = secrets_match && codes_match;
    ESP_LOGI(TAG, "selftest: %s", pass ? "SELFTEST PASS" : "SELFTEST FAIL");
}

// ── Lifecycle ────────────────────────────────────────────────────────────

int pairing_init(void) {
    // Safe here: pairing_init() runs on the kernel's module-loader task
    // (internal RAM stack), same reasoning meshcore_module.cpp documents
    // for why its own identity load moved out of mc_task() and into
    // mc_manager_init() — NVS access here, not on pairing_task() the first
    // time, is fine, and the load only happens once at boot anyway.
    load_paired();

    proximity_register_handler(PROXIMITY_FRAME_PAIRING, pairing_on_frame);

    // Small/no PSRAM hazard here either way, but internal RAM keeps this
    // consistent with "only pairing_task() touches NVS" — plain
    // xTaskCreatePinnedToCore (not WithCaps), core 1 alongside the mesh/
    // radio-companion tasks it's conceptually part of (see mc_persist_task).
    //
    // Tried un-pinning this (leaving it tskNO_AFFINITY) on the theory that
    // it was adding to core 1 contention against cupcake's UI task — live
    // hardware testing showed the opposite: the "UI TASK UNRESPONSIVE"
    // crash-guard hang tripped FASTER unpinned (~33s) than pinned (~89s).
    // An unpinned task can still land on core 1 (and migrate there
    // unpredictably), so pinning it deterministically seems to help, not
    // hurt. The underlying stall is still unexplained — see the "Remote
    // radio companion" plan notes / meshtastic's own mesh_task pinning
    // comment; leading suspect is CPU monopolization somewhere in the
    // sx1262_rl/RadioLib radio-polling path (a documented precedent exists
    // in sx1262.c's wait_busy() for the old hand-rolled driver — same
    // "UI TASK UNRESPONSIVE" signature, fixed there already; sx1262_rl's
    // RadioLib-vendored busy-wait hasn't been audited the same way yet).
    BaseType_t ok = xTaskCreatePinnedToCore(pairing_task, "pairing", 3072, NULL, 2, &s_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create pairing task — persistence disabled");
    }

    ESP_LOGI(TAG, "ready (%d paired device%s)", s_paired_count, s_paired_count == 1 ? "" : "s");
    return 0;
}

void pairing_deinit(void) {
    if (s_task) {
        if (s_dirty) {
            s_dirty = false;
            save_paired();
        }
        vTaskDelete(s_task);
        s_task = NULL;
    }
    proximity_register_handler(PROXIMITY_FRAME_PAIRING, NULL);
    s_state = PAIRING_STATE_NONE;
}

// ── Module header ─────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(pairing) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "pairing",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = pairing_init,
    .deinit            = pairing_deinit,
};
