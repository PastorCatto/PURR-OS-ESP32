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
#include "../proximity_rpc/proximity_rpc.h"
#include "../user_mgr/user_mgr.h"
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
#include <mbedtls/gcm.h>
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

// Internal accessor for the Phase B/C RPC handlers. Also the backing
// implementation for pairing_get_shared_secret() (public, further down) —
// server_mgr.h needs the same Phase A secret to encrypt ITS OWN payloads
// (WiFi credentials) the same way this file's own USERAUTH/OOBE messages
// already are, rather than either sending them in the clear or inventing
// a second handshake. False if mac isn't a trusted peer.
static bool get_paired_secret(const uint8_t mac[6], uint8_t out[32]) {
    int idx = find_paired_idx(mac);
    if (idx < 0) return false;
    memcpy(out, s_paired_secrets[idx], 32);
    return true;
}

// ── Remote-login: user-access credentials (Phase B/C) ───────────────────────
// See this session's approved remote-login plan for the full protocol this
// implements. Short version: Phase B is a rare, human-gated event (first
// contact with a given username on a given paired device) that ends with
// this device holding a fresh random 32-byte key; Phase C is the common,
// cheap path (every later "log in as this user again") that only ever
// proves possession of that key via challenge-response, never resending it.
//
// One unified table serves BOTH roles — "I issued this key to a client"
// (this device is the account's real owner) and "I registered this key
// with a server" (this device is the one logging in) are the same fact
// shape, (mac, username) -> key, just populated by different code paths
// (handle_userauth_register() vs pairing_register_user_key()). A device
// can be either role, or both roles for different peers, without needing
// two tables.
#define USERAUTH_MAX_KEYS      8    // modest — matches PAIRING_MAX_DEVICES' own "small, bounded" scale
#define USERAUTH_USERNAME_MAX  32   // matches USER_MGR_USERNAME_MAX without pulling that header in just for the constant
// 30 days — Phase D's lazy expiry threshold (this file's own top comment
// on "the key gets burnt" after prolonged inactivity). Applied only on the
// SERVER side, only when purr_kernel_time_is_synced() — see
// userauth_key_expired()'s own comment on why an unsynced clock must never
// be treated as "expired".
#define USERAUTH_KEY_EXPIRY_S  (30UL * 24UL * 3600UL)
// A pending user-access request gets longer than PAIRING_TIMEOUT_MS's 60s —
// that one gates a person who's already looking at their screen mid-
// handshake; this one waits on a notification being NOTICED, which can
// reasonably take minutes.
#define USERAUTH_REQ_TIMEOUT_MS (5UL * 60UL * 1000UL)

typedef struct __attribute__((packed)) {
    uint8_t  mac[6];
    char     username[USERAUTH_USERNAME_MAX];
    uint8_t  key[32];
    // Real wall-clock seconds (purr_kernel_time_now()) of the last
    // successful Phase C verify (or the Phase B registration that created
    // this entry) — 0 if it was ever recorded while the clock was
    // unsynced. See userauth_key_expired()'s own comment for why that
    // means "never expire this entry" rather than "expired since 1970".
    uint32_t last_used_epoch_s;
} userauth_key_t;

static userauth_key_t s_userauth_keys[USERAUTH_MAX_KEYS];
static int            s_userauth_key_count = 0;
// Persisted through the same s_dirty/pairing_task() path as the trust
// list — see load_paired()/save_paired() below.

static int userauth_find_key(const uint8_t mac[6], const char *username) {
    for (int i = 0; i < s_userauth_key_count; i++) {
        if (memcmp(s_userauth_keys[i].mac, mac, 6) == 0 &&
            strncmp(s_userauth_keys[i].username, username, USERAUTH_USERNAME_MAX) == 0) {
            return i;
        }
    }
    return -1;
}

// Adds a new (mac, username) -> key entry, or overwrites the key on an
// existing one (a fresh Phase B registration for a username this device
// already had a — possibly expired, possibly just replaced-by-choice —
// key for). Silently drops the add if the table is full, same "cap and
// warn, not a threaded-through error" precedent add_or_update_paired()
// already sets for this file.
static void userauth_store_key(const uint8_t mac[6], const char *username, const uint8_t key[32]) {
    int idx = userauth_find_key(mac, username);
    if (idx < 0) {
        if (s_userauth_key_count >= USERAUTH_MAX_KEYS) {
            ESP_LOGW(TAG, "userauth key table full (%d entries) — not storing key for '%s'",
                     USERAUTH_MAX_KEYS, username);
            return;
        }
        idx = s_userauth_key_count++;
        memcpy(s_userauth_keys[idx].mac, mac, 6);
        strncpy(s_userauth_keys[idx].username, username, USERAUTH_USERNAME_MAX - 1);
        s_userauth_keys[idx].username[USERAUTH_USERNAME_MAX - 1] = 0;
    }
    memcpy(s_userauth_keys[idx].key, key, 32);
    s_userauth_keys[idx].last_used_epoch_s = purr_kernel_time_is_synced() ? (uint32_t)purr_kernel_time_now() : 0;
    s_dirty = true;
}

// Marks an existing entry as just-used (Phase C's own successful verify) —
// this is what keeps Phase D's expiry from ever firing on a key that's
// actually still in regular use.
static void userauth_touch_key(int idx) {
    if (idx < 0 || idx >= s_userauth_key_count) return;
    s_userauth_keys[idx].last_used_epoch_s = purr_kernel_time_is_synced() ? (uint32_t)purr_kernel_time_now() : 0;
    s_dirty = true;
}

static void userauth_remove_key_at(int idx) {
    if (idx < 0 || idx >= s_userauth_key_count) return;
    s_userauth_keys[idx] = s_userauth_keys[s_userauth_key_count - 1];
    s_userauth_key_count--;
    s_dirty = true;
}

// True if this SERVER-held entry should be treated as gone — Phase D. Only
// ever applied on the serving side (handle_userauth_challenge() below), and
// only when the clock is actually synced: last_used_epoch_s == 0 means
// either "never recorded under a synced clock" or a genuinely fresh device
// that hasn't synced yet — either way, comparing against epoch 0 would
// make every entry look 56 years overdue and expire the instant this
// function is ever called on an unsynced device. Treat "can't tell" as
// "not expired" rather than risk discarding a real, actively-used
// credential just because NTP/RTC hasn't caught up yet this boot.
static bool userauth_key_expired(int idx) {
    if (!purr_kernel_time_is_synced()) return false;
    if (idx < 0 || idx >= s_userauth_key_count) return true;
    if (s_userauth_keys[idx].last_used_epoch_s == 0) return false;
    time_t now = purr_kernel_time_now();
    if (now < (time_t)s_userauth_keys[idx].last_used_epoch_s) return false;   // clock moved backward — don't punish for that
    return ((uint32_t)now - s_userauth_keys[idx].last_used_epoch_s) > USERAUTH_KEY_EXPIRY_S;
}

// ── Crypto helpers for the Phase B/C wire messages ──────────────────────────

// Per-message AES-256-GCM key for one Phase B request — SHA256(shared
// secret || nonce || a fixed label). HKDF (MBEDTLS_HKDF_C) isn't enabled
// in this build; a plain concatenation hash is a standard, sufficient
// substitute for deriving ONE symmetric key from an established secret +
// a fresh nonce (no extract-then-expand, multi-key domain separation
// needed here — GCM itself, not this derivation, is what actually
// authenticates the message). A fresh `nonce` every call (esp_fill_random,
// at each Phase B call site) means a fresh msg_key every call even though
// `secret` is long-lived.
static void derive_msg_key(const uint8_t secret[32], const uint8_t nonce[12], uint8_t out[32]) {
    static const char *label = "purr_pairing_access";
    uint8_t buf[32 + 12 + 32];
    size_t label_len = strlen(label);
    memcpy(buf, secret, 32);
    memcpy(buf + 32, nonce, 12);
    memcpy(buf + 32 + 12, label, label_len);
    mbedtls_sha256(buf, 32 + 12 + label_len, out, 0);
}

// Standard HMAC-SHA256 (RFC 2104), hand-implemented on top of the plain
// mbedtls_sha256_* calls this file (and user_mgr.c) already use rather
// than reaching for mbedtls_md_hmac() — MBEDTLS_MD_C's own enablement in
// this build wasn't confirmed, and this construction is simple enough to
// not be worth adding that uncertainty for. Used for Phase C's proof
// (HMAC(registered_key, challenge)) — plain SHA256(key || message) was
// deliberately NOT used here despite being fine for THIS protocol's own
// non-attacker-suppliable-message shape (see this session's own design
// discussion): HMAC is the textbook-correct primitive and costs nothing
// extra to use correctly from the start.
static void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[32]) {
    uint8_t key_block[64] = {0};
    if (key_len > 64) {
        mbedtls_sha256(key, key_len, key_block, 0);   // fills first 32 bytes, rest stays 0-padded
    } else {
        memcpy(key_block, key, key_len);
    }
    uint8_t k_ipad[64], k_opad[64];
    for (int i = 0; i < 64; i++) {
        k_ipad[i] = (uint8_t)(key_block[i] ^ 0x36);
        k_opad[i] = (uint8_t)(key_block[i] ^ 0x5c);
    }

    uint8_t inner[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, k_ipad, 64);
    mbedtls_sha256_update(&ctx, msg, msg_len);
    mbedtls_sha256_finish(&ctx, inner);
    mbedtls_sha256_free(&ctx);

    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, k_opad, 64);
    mbedtls_sha256_update(&ctx, inner, 32);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

static bool constant_time_eq32(const uint8_t a[32], const uint8_t b[32]) {
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

// ── Wire structs for the USERAUTH_* proximity_rpc actions ───────────────────
// Fresh action-ID block — REMOTEAPPS_ACTION_* (app_manager_remote.h) uses
// 0x1000, milkbar's own MILKBAR_ACTION_MSG_SEND uses 0x3000; neither
// overlaps this file's 0x2000 range (action_id namespacing is the
// caller's own responsibility, per proximity_rpc.h's own doc comment).
#define PAIRING_ACTION_USERAUTH_SALT      0x2000   // req: username -> resp: 16B salt (0-length = no such user/no password)
#define PAIRING_ACTION_USERAUTH_REQUEST   0x2001   // req: userauth_request_req_t -> resp: 1B status (0=denied, 1=pending)
#define PAIRING_ACTION_USERAUTH_STATUS    0x2002   // req: username -> resp: 1B status (0=none, 1=pending, 2=approved)
#define PAIRING_ACTION_USERAUTH_REGISTER  0x2003   // req: userauth_register_req_t -> resp: 1B status (0=fail, 1=ok)
#define PAIRING_ACTION_USERAUTH_CHALLENGE 0x2004   // req: username -> resp: 16B challenge (0-length = not registered)
#define PAIRING_ACTION_USERAUTH_VERIFY    0x2005   // req: userauth_verify_req_t -> resp: 1B status (0=fail, 1=ok)

// Remote OOBE (first-run setup pushed from an already-trusted client) —
// see pairing.h's own "Remote OOBE" doc comment for the full design. Next
// free block: 0x3000 (milkbar's old MILKBAR_ACTION_MSG_SEND) was retired
// this session along with the feature that used it, so this starts fresh
// at 0x4000 rather than reclaiming a freed value — a stale client still
// running old firmware would otherwise get a real (if wrong) response
// instead of a clean "unknown action" from proximity_rpc's own dispatch.
#define PAIRING_ACTION_OOBE_QUERY 0x4000   // req: none -> resp: 1B (1=needs setup, 0=already configured)
#define PAIRING_ACTION_OOBE_PUSH  0x4001   // req: oobe_push_req_t -> resp: 1B status (0=fail, 1=ok)

#define PAIRING_RPC_TIMEOUT_MS 3000UL   // same as milkbar's own RPC_TIMEOUT_MS convention

typedef struct __attribute__((packed)) {
    char    username[USERAUTH_USERNAME_MAX];
    uint8_t nonce[12];
    uint8_t ciphertext[32];   // AES-256-GCM(pwhash)
    uint8_t tag[16];
} userauth_request_req_t;

typedef struct __attribute__((packed)) {
    char    username[USERAUTH_USERNAME_MAX];
    uint8_t nonce[12];
    uint8_t ciphertext[32];   // AES-256-GCM(the new 32-byte key)
    uint8_t tag[16];
} userauth_register_req_t;

typedef struct __attribute__((packed)) {
    char    username[USERAUTH_USERNAME_MAX];
    uint8_t proof[32];        // HMAC-SHA256(registered_key, challenge)
} userauth_verify_req_t;

// Remote OOBE push — the inner payload is encrypted the same way
// userauth_request_req_t's password hash is (this carries a real
// plaintext password, deserves the same protection, not less): AES-256-
// GCM under the Phase A pairing secret, a fresh nonce per call.
// username=="" means "keep the peer's own bootstrap default account,
// no password" — oobe_app.c's own on_skip() path, just pushed remotely.
typedef struct __attribute__((packed)) {
    char username[USERAUTH_USERNAME_MAX];
    char password[64];
} oobe_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t nonce[12];
    uint8_t ciphertext[sizeof(oobe_payload_t)];
    uint8_t tag[16];
} oobe_push_req_t;

// Copies req into a NUL-terminated username buffer, bounded by both the
// wire field's own size and req_len (a shorter-than-USERNAME_MAX request
// is legal — the SALT/STATUS/CHALLENGE requests send just the raw name
// bytes, no fixed-size struct).
static void copy_req_username(const uint8_t *req, size_t req_len, char out[USERAUTH_USERNAME_MAX]) {
    size_t n = req_len < (size_t)(USERAUTH_USERNAME_MAX - 1) ? req_len : (size_t)(USERAUTH_USERNAME_MAX - 1);
    memcpy(out, req, n);
    out[n] = 0;
}

// ── Pending user-access request (Phase B's human-approval gate) ─────────────
// One at a time, same simplicity precedent as this file's own device-level
// PENDING_* — a second request arriving while one is already pending is
// just refused (handle_userauth_request() below), not queued.
typedef enum {
    USERAUTH_REQ_NONE = 0,
    USERAUTH_REQ_PENDING,     // password verified server-side, waiting on pairing_confirm_user_access()/_reject_user_access()
    USERAUTH_REQ_APPROVED,    // approved, waiting for the client to actually call REGISTER
} userauth_req_state_t;

static userauth_req_state_t s_userauth_req_state = USERAUTH_REQ_NONE;
static uint8_t  s_userauth_req_mac[6];
static char     s_userauth_req_username[USERAUTH_USERNAME_MAX];
static char     s_userauth_req_device_name[20];   // matches paired_device_t.name's own size
static uint32_t s_userauth_req_started_ms;

// ── Pending Phase C challenge ─────────────────────────────────────────────
// Single slot, same reasoning as the request state above: proximity_task()
// processes one inbound frame at a time, so CHALLENGE immediately followed
// by VERIFY (the only pattern pairing_verify_user() ever produces) can
// never actually interleave with a second, different (mac, username)
// challenge in flight. No separate timeout — answered or overwritten
// within one blocking proximity_rpc_call() round trip in practice; a
// VERIFY that never follows just leaves a stale, unusable (mac, username)
// pair sitting here until the next CHALLENGE overwrites it.
static uint8_t s_userauth_challenge[16];
static uint8_t s_userauth_challenge_mac[6];
static char    s_userauth_challenge_username[USERAUTH_USERNAME_MAX];

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

    // Remote-login key table (Phase B/C) — own count/blob keys, same
    // "whole array as one blob" shape as "devices"/"count" above. A
    // missing "uak_count" (upgrading from a build that predates this)
    // just leaves the table empty, same as a fresh install.
    uint8_t uak_count = 0;
    if (nvs_get_u8(h, "uak_count", &uak_count) == ESP_OK && uak_count > 0) {
        if (uak_count > USERAUTH_MAX_KEYS) uak_count = USERAUTH_MAX_KEYS;
        size_t uak_len = sizeof(userauth_key_t) * (size_t)uak_count;
        if (nvs_get_blob(h, "uak_keys", s_userauth_keys, &uak_len) == ESP_OK) {
            s_userauth_key_count = uak_count;
        }
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
    nvs_set_u8(h, "uak_count", (uint8_t)s_userauth_key_count);
    if (s_userauth_key_count > 0) {
        nvs_set_blob(h, "uak_keys", s_userauth_keys, sizeof(userauth_key_t) * (size_t)s_userauth_key_count);
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

// ── USERAUTH RPC handlers (Phase B/C) ───────────────────────────────────────
// Registered via proximity_rpc_register() in pairing_init() below. Same
// thread as pairing_on_frame() (proximity_task()'s own thread, possibly
// PSRAM-stacked) — no NVS here either, only s_dirty + the in-RAM tables
// (userauth_store_key()/_touch_key() both already follow that rule).
//
// mac is pre-authorized by proximity_rpc itself before any handler here
// runs (proximity_rpc.h's own doc comment: "every inbound frame is checked
// against pairing_is_trusted() before being dispatched anywhere") — so
// find_paired_idx(mac)/get_paired_secret(mac, ...) succeeding is an
// invariant here, not something these handlers need to treat as a normal
// failure path; they still check defensively rather than assume it.

static bool handle_userauth_salt(const uint8_t mac[6], uint16_t action_id,
                                  const uint8_t *req, size_t req_len,
                                  uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out) {
    (void)mac; (void)action_id;
    char username[USERAUTH_USERNAME_MAX];
    copy_req_username(req, req_len, username);

    uint8_t salt[16];
    if (resp_cap < 16 || !user_mgr_get_salt(username, salt)) {
        *resp_len_out = 0;   // valid response: "no such user" / no-password account
        return true;
    }
    memcpy(resp_out, salt, 16);
    *resp_len_out = 16;
    return true;
}

static bool handle_userauth_request(const uint8_t mac[6], uint16_t action_id,
                                     const uint8_t *req, size_t req_len,
                                     uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out) {
    (void)action_id;
    if (req_len != sizeof(userauth_request_req_t) || resp_cap < 1) { *resp_len_out = 0; return false; }
    userauth_request_req_t r;
    memcpy(&r, req, sizeof(r));
    char username[USERAUTH_USERNAME_MAX];
    copy_req_username((const uint8_t *)r.username, sizeof(r.username), username);

    // Only one pending request at a time (see this file's own top comment
    // on that state) — a second REQUEST while one's already pending (from
    // this or any other device) is refused outright, not queued.
    if (s_userauth_req_state != USERAUTH_REQ_NONE) {
        ESP_LOGW(TAG, "userauth request for '%s' refused — another request is already pending", username);
        resp_out[0] = 0; *resp_len_out = 1;
        return true;
    }

    uint8_t secret[32];
    if (!get_paired_secret(mac, secret)) { resp_out[0] = 0; *resp_len_out = 1; return true; }
    uint8_t msg_key[32];
    derive_msg_key(secret, r.nonce, msg_key);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, msg_key, 256);
    uint8_t pwhash[32];
    int rc = mbedtls_gcm_auth_decrypt(&gcm, 32, r.nonce, sizeof(r.nonce), NULL, 0, r.tag, sizeof(r.tag), r.ciphertext, pwhash);
    mbedtls_gcm_free(&gcm);
    if (rc != 0) {
        ESP_LOGW(TAG, "userauth request for '%s': GCM auth failed (rc=%d)", username, rc);
        resp_out[0] = 0; *resp_len_out = 1;
        return true;
    }

    if (!user_mgr_verify_hash(username, pwhash)) {
        ESP_LOGW(TAG, "userauth request: wrong password for '%s'", username);
        resp_out[0] = 0; *resp_len_out = 1;
        return true;
    }

    int pidx = find_paired_idx(mac);
    const char *device_name = (pidx >= 0) ? s_paired_devices[pidx].name : "A device";

    memcpy(s_userauth_req_mac, mac, 6);
    strncpy(s_userauth_req_username, username, sizeof(s_userauth_req_username) - 1);
    s_userauth_req_username[sizeof(s_userauth_req_username) - 1] = 0;
    strncpy(s_userauth_req_device_name, device_name, sizeof(s_userauth_req_device_name) - 1);
    s_userauth_req_device_name[sizeof(s_userauth_req_device_name) - 1] = 0;
    s_userauth_req_state = USERAUTH_REQ_PENDING;
    s_userauth_req_started_ms = (uint32_t)purr_kernel_uptime_ms();

    // device_name is up to sizeof(paired_device_t::name)-1 (19) chars,
    // username up to USERAUTH_USERNAME_MAX-1 (31) — the fixed portion of
    // the format string is 25 more, so worst case needs 19+31+25+1(NUL) =
    // 76 bytes. 96 leaves real headroom rather than sizing to the exact
    // byte — confirmed live as a real (not false-positive) -Werror=format-
    // truncation on a target/optimization level where GCC could actually
    // prove the old 64-byte buffer too small for the worst case.
    char notify_body[96];
    snprintf(notify_body, sizeof(notify_body), "%s wants to access user '%s'", device_name, username);
    purr_kernel_notify("Remote login request", notify_body, "pairing");

    resp_out[0] = 1;   // pending
    *resp_len_out = 1;
    return true;
}

static bool handle_userauth_status(const uint8_t mac[6], uint16_t action_id,
                                    const uint8_t *req, size_t req_len,
                                    uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out) {
    (void)action_id;
    if (resp_cap < 1) return false;
    char username[USERAUTH_USERNAME_MAX];
    copy_req_username(req, req_len, username);

    uint8_t status = 0;   // none/denied
    if (memcmp(mac, s_userauth_req_mac, 6) == 0 &&
        strncmp(username, s_userauth_req_username, USERAUTH_USERNAME_MAX) == 0) {
        if (s_userauth_req_state == USERAUTH_REQ_PENDING) status = 1;
        else if (s_userauth_req_state == USERAUTH_REQ_APPROVED) status = 2;
    }
    resp_out[0] = status;
    *resp_len_out = 1;
    return true;
}

static bool handle_userauth_register(const uint8_t mac[6], uint16_t action_id,
                                      const uint8_t *req, size_t req_len,
                                      uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out) {
    (void)action_id;
    if (req_len != sizeof(userauth_register_req_t) || resp_cap < 1) { *resp_len_out = 0; return false; }
    userauth_register_req_t r;
    memcpy(&r, req, sizeof(r));
    char username[USERAUTH_USERNAME_MAX];
    copy_req_username((const uint8_t *)r.username, sizeof(r.username), username);

    if (s_userauth_req_state != USERAUTH_REQ_APPROVED ||
        memcmp(mac, s_userauth_req_mac, 6) != 0 ||
        strncmp(username, s_userauth_req_username, USERAUTH_USERNAME_MAX) != 0) {
        resp_out[0] = 0; *resp_len_out = 1;
        return true;
    }

    uint8_t secret[32];
    if (!get_paired_secret(mac, secret)) { resp_out[0] = 0; *resp_len_out = 1; return true; }
    uint8_t msg_key[32];
    derive_msg_key(secret, r.nonce, msg_key);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, msg_key, 256);
    uint8_t new_key[32];
    int rc = mbedtls_gcm_auth_decrypt(&gcm, 32, r.nonce, sizeof(r.nonce), NULL, 0, r.tag, sizeof(r.tag), r.ciphertext, new_key);
    mbedtls_gcm_free(&gcm);
    if (rc != 0) {
        ESP_LOGW(TAG, "userauth register for '%s': GCM auth failed (rc=%d)", username, rc);
        resp_out[0] = 0; *resp_len_out = 1;
        return true;
    }

    userauth_store_key(mac, username, new_key);
    ESP_LOGI(TAG, "userauth: registered a key for '%s' (device %s)", username, s_userauth_req_device_name);
    s_userauth_req_state = USERAUTH_REQ_NONE;   // consumed — this negotiation is done

    resp_out[0] = 1;
    *resp_len_out = 1;
    return true;
}

static bool handle_userauth_challenge(const uint8_t mac[6], uint16_t action_id,
                                       const uint8_t *req, size_t req_len,
                                       uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out) {
    (void)action_id;
    char username[USERAUTH_USERNAME_MAX];
    copy_req_username(req, req_len, username);

    int idx = userauth_find_key(mac, username);
    if (idx < 0 || userauth_key_expired(idx) || resp_cap < 16) {
        if (idx >= 0 && userauth_key_expired(idx)) {
            ESP_LOGI(TAG, "userauth: key for '%s' expired (no activity in %lu+ days) — removing",
                     username, (unsigned long)(USERAUTH_KEY_EXPIRY_S / 86400UL));
            userauth_remove_key_at(idx);
        }
        *resp_len_out = 0;   // "not registered" — client falls back to Phase B
        return true;
    }

    esp_fill_random(s_userauth_challenge, sizeof(s_userauth_challenge));
    memcpy(s_userauth_challenge_mac, mac, 6);
    strncpy(s_userauth_challenge_username, username, sizeof(s_userauth_challenge_username) - 1);
    s_userauth_challenge_username[sizeof(s_userauth_challenge_username) - 1] = 0;

    memcpy(resp_out, s_userauth_challenge, sizeof(s_userauth_challenge));
    *resp_len_out = sizeof(s_userauth_challenge);
    return true;
}

// Response is 2 bytes: [0]=1/0 ok, [1]=is_admin (meaningless when [0]==0,
// still written as 0 for a predictable byte). Grown from 1 byte so the
// client learns whether the account it just logged into is an admin on
// the server without a separate round trip — milkbar's own dashboard-vs-
// desktop routing is the reason this exists (see user_mgr_is_admin()'s
// doc comment). Safe to grow: this changed before any real two-device
// deployment, both sides of this exchange are always the same firmware
// build anyway (no wire-compat concern the way a public protocol would
// have).
static bool handle_userauth_verify(const uint8_t mac[6], uint16_t action_id,
                                    const uint8_t *req, size_t req_len,
                                    uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out) {
    (void)action_id;
    if (req_len != sizeof(userauth_verify_req_t) || resp_cap < 2) { *resp_len_out = 0; return false; }
    userauth_verify_req_t r;
    memcpy(&r, req, sizeof(r));
    char username[USERAUTH_USERNAME_MAX];
    copy_req_username((const uint8_t *)r.username, sizeof(r.username), username);

    bool ok = false;
    if (memcmp(mac, s_userauth_challenge_mac, 6) == 0 &&
        strncmp(username, s_userauth_challenge_username, USERAUTH_USERNAME_MAX) == 0) {
        int idx = userauth_find_key(mac, username);
        if (idx >= 0) {
            uint8_t expected[32];
            hmac_sha256(s_userauth_keys[idx].key, sizeof(s_userauth_keys[idx].key),
                        s_userauth_challenge, sizeof(s_userauth_challenge), expected);
            ok = constant_time_eq32(expected, r.proof);
            if (ok) userauth_touch_key(idx);
        }
    }

    // Single-use — clear regardless of outcome so the same challenge can
    // never be replayed for a second VERIFY attempt.
    memset(s_userauth_challenge, 0, sizeof(s_userauth_challenge));
    memset(s_userauth_challenge_mac, 0, sizeof(s_userauth_challenge_mac));
    s_userauth_challenge_username[0] = 0;

    resp_out[0] = ok ? 1 : 0;
    resp_out[1] = (ok && user_mgr_is_admin(username)) ? 1 : 0;
    *resp_len_out = 2;
    return true;
}

// ── Remote OOBE (server side) ────────────────────────────────────────────
// See pairing.h's own "Remote OOBE" doc comment. Both handlers require
// pairing_is_trusted(mac) — this device must already trust the peer, same
// rule every higher-level feature built on the trust list applies to its
// own inbound frames (this header's own top comment). Unlike the USERAUTH
// handlers above (which check trust implicitly via get_paired_secret()
// succeeding, since only a paired peer has a secret to decrypt with),
// QUERY carries no encrypted payload at all, so it needs an explicit
// pairing_is_trusted() check of its own to avoid leaking "still needs
// setup" to a stranger.

static bool handle_oobe_query(const uint8_t mac[6], uint16_t action_id,
                               const uint8_t *req, size_t req_len,
                               uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out) {
    (void)action_id; (void)req; (void)req_len;
    if (resp_cap < 1) { *resp_len_out = 0; return false; }
    resp_out[0] = (pairing_is_trusted(mac) && !user_mgr_oobe_completed()) ? 1 : 0;
    *resp_len_out = 1;
    return true;
}

static bool handle_oobe_push(const uint8_t mac[6], uint16_t action_id,
                              const uint8_t *req, size_t req_len,
                              uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out) {
    (void)action_id;
    if (req_len != sizeof(oobe_push_req_t) || resp_cap < 1) { *resp_len_out = 0; return false; }
    resp_out[0] = 0;
    *resp_len_out = 1;

    // A trusted-but-already-configured peer, or an untrusted one, both get
    // the same flat "fail" — this is a first-setup mechanism, permanently
    // gated off once done (see this file's own header comment on why),
    // not something worth distinguishing "wrong" reasons for over the air.
    if (!pairing_is_trusted(mac) || user_mgr_oobe_completed()) return true;

    uint8_t secret[32];
    if (!get_paired_secret(mac, secret)) return true;

    oobe_push_req_t r;
    memcpy(&r, req, sizeof(r));
    uint8_t msg_key[32];
    derive_msg_key(secret, r.nonce, msg_key);

    oobe_payload_t payload;
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, msg_key, 256);
    int rc = mbedtls_gcm_auth_decrypt(&gcm, sizeof(payload), r.nonce, sizeof(r.nonce), NULL, 0,
                                       r.tag, sizeof(r.tag), r.ciphertext, (uint8_t *)&payload);
    mbedtls_gcm_free(&gcm);
    if (rc != 0) {
        ESP_LOGW(TAG, "remote oobe push: GCM auth failed (rc=%d)", rc);
        return true;
    }
    char username[USERAUTH_USERNAME_MAX];
    copy_req_username((const uint8_t *)payload.username, sizeof(payload.username), username);
    char password[sizeof(payload.password) + 1];
    memcpy(password, payload.password, sizeof(payload.password));
    password[sizeof(payload.password)] = 0;

    // Mirrors oobe_app.c's on_continue()/on_skip() exactly — see that
    // file for the local-UI version of this same logic.
    if (username[0] == '\0') {
        ESP_LOGI(TAG, "remote oobe: keeping default account '%s'", user_mgr_default_username());
        user_mgr_set_logged_in(user_mgr_default_username());
    } else {
        if (!user_mgr_valid_username(username)) {
            ESP_LOGW(TAG, "remote oobe: invalid username '%s'", username);
            return true;
        }
        const char *current_default = user_mgr_default_username();
        bool renaming = strcmp(username, current_default) != 0;
        if (renaming) {
            if (user_mgr_exists(username)) {
                ESP_LOGW(TAG, "remote oobe: '%s' already exists", username);
                return true;
            }
            if (!user_mgr_create(username, password)) {
                ESP_LOGW(TAG, "remote oobe: create('%s') failed", username);
                return true;
            }
            if (strcmp(current_default, USER_MGR_BOOTSTRAP_USER) == 0) {
                user_mgr_remove(USER_MGR_BOOTSTRAP_USER);
            }
        } else if (!user_mgr_set_password(username, password)) {
            ESP_LOGW(TAG, "remote oobe: set_password('%s') failed", username);
            return true;
        }
        user_mgr_set_logged_in(username);
    }

    user_mgr_set_oobe_completed();
    ESP_LOGI(TAG, "remote oobe: setup complete via pushed config, account '%s'",
             username[0] ? username : user_mgr_default_username());
    resp_out[0] = 1;
    return true;
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

        if (s_userauth_req_state != USERAUTH_REQ_NONE &&
            (uint32_t)purr_kernel_uptime_ms() - s_userauth_req_started_ms >= USERAUTH_REQ_TIMEOUT_MS) {
            ESP_LOGI(TAG, "pending user-access request for '%s' timed out", s_userauth_req_username);
            s_userauth_req_state = USERAUTH_REQ_NONE;
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

// ── Remote login: Phase B (server side — human approval surface) ───────────

bool pairing_get_pending_user_request(char *out_username, size_t username_sz,
                                       char *out_device_name, size_t device_name_sz) {
    if (s_userauth_req_state != USERAUTH_REQ_PENDING) return false;
    if (out_username) snprintf(out_username, username_sz, "%s", s_userauth_req_username);
    if (out_device_name) snprintf(out_device_name, device_name_sz, "%s", s_userauth_req_device_name);
    return true;
}

void pairing_confirm_user_access(void) {
    if (s_userauth_req_state != USERAUTH_REQ_PENDING) return;
    s_userauth_req_state = USERAUTH_REQ_APPROVED;
    // Fresh timeout window for the client to actually call REGISTER —
    // don't inherit however much of USERAUTH_REQ_TIMEOUT_MS the human
    // approval step itself already used up.
    s_userauth_req_started_ms = (uint32_t)purr_kernel_uptime_ms();
    ESP_LOGI(TAG, "userauth: approved access to '%s' for %s", s_userauth_req_username, s_userauth_req_device_name);
}

void pairing_reject_user_access(void) {
    if (s_userauth_req_state != USERAUTH_REQ_PENDING) return;
    ESP_LOGI(TAG, "userauth: denied access to '%s' for %s", s_userauth_req_username, s_userauth_req_device_name);
    s_userauth_req_state = USERAUTH_REQ_NONE;
}

// ── Remote login: Phase B (client side) ─────────────────────────────────────
// Every function below is a blocking proximity_rpc_call() (or several) —
// same "never call from cupcake_task or proximity_task" rule that header's
// own top comment documents. Callers (milkbar.c) must run these from their
// own background task, same as every existing proximity_rpc_call() site in
// this codebase already does.

bool pairing_request_user_access(const uint8_t mac[6], const char *username, const char *password) {
    if (!mac || !username || !password) return false;

    uint8_t resp[64]; size_t resp_len = 0;
    bool ok = proximity_rpc_call(mac, PAIRING_ACTION_USERAUTH_SALT,
                                  (const uint8_t *)username, strlen(username),
                                  resp, sizeof(resp), &resp_len, PAIRING_RPC_TIMEOUT_MS);
    if (!ok || resp_len != 16) return false;
    uint8_t salt[16];
    memcpy(salt, resp, 16);

    uint8_t pwhash[32];
    user_mgr_hash_password(salt, password, pwhash);

    uint8_t secret[32];
    if (!get_paired_secret(mac, secret)) return false;

    userauth_request_req_t req = {0};
    strncpy(req.username, username, sizeof(req.username) - 1);
    esp_fill_random(req.nonce, sizeof(req.nonce));
    uint8_t msg_key[32];
    derive_msg_key(secret, req.nonce, msg_key);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, msg_key, 256);
    mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, 32, req.nonce, sizeof(req.nonce), NULL, 0,
                               pwhash, req.ciphertext, sizeof(req.tag), req.tag);
    mbedtls_gcm_free(&gcm);

    ok = proximity_rpc_call(mac, PAIRING_ACTION_USERAUTH_REQUEST,
                             (const uint8_t *)&req, sizeof(req), resp, sizeof(resp), &resp_len,
                             PAIRING_RPC_TIMEOUT_MS);
    if (!ok || resp_len != 1) return false;
    return resp[0] == 1;   // true = password accepted, pending human approval; false = denied outright
}

pairing_user_access_status_t pairing_poll_user_access(const uint8_t mac[6], const char *username) {
    if (!mac || !username) return PAIRING_USERAUTH_NONE;
    uint8_t resp[4]; size_t resp_len = 0;
    bool ok = proximity_rpc_call(mac, PAIRING_ACTION_USERAUTH_STATUS,
                                  (const uint8_t *)username, strlen(username),
                                  resp, sizeof(resp), &resp_len, PAIRING_RPC_TIMEOUT_MS);
    if (!ok || resp_len != 1 || resp[0] > 2) return PAIRING_USERAUTH_NONE;
    return (pairing_user_access_status_t)resp[0];
}

bool pairing_register_user_key(const uint8_t mac[6], const char *username) {
    if (!mac || !username) return false;

    uint8_t key[32];
    esp_fill_random(key, sizeof(key));

    uint8_t secret[32];
    if (!get_paired_secret(mac, secret)) return false;

    userauth_register_req_t req = {0};
    strncpy(req.username, username, sizeof(req.username) - 1);
    esp_fill_random(req.nonce, sizeof(req.nonce));
    uint8_t msg_key[32];
    derive_msg_key(secret, req.nonce, msg_key);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, msg_key, 256);
    mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, 32, req.nonce, sizeof(req.nonce), NULL, 0,
                               key, req.ciphertext, sizeof(req.tag), req.tag);
    mbedtls_gcm_free(&gcm);

    uint8_t resp[4]; size_t resp_len = 0;
    bool ok = proximity_rpc_call(mac, PAIRING_ACTION_USERAUTH_REGISTER,
                                  (const uint8_t *)&req, sizeof(req), resp, sizeof(resp), &resp_len,
                                  PAIRING_RPC_TIMEOUT_MS);
    if (!ok || resp_len != 1 || resp[0] != 1) return false;

    // Our OWN copy, for pairing_verify_user() below — same table, same
    // helper the server side uses to store its copy (see this file's own
    // "one unified table serves both roles" comment).
    userauth_store_key(mac, username, key);
    return true;
}

// ── Remote login: Phase C (client side — the common, cheap reconnect path) ──

bool pairing_verify_user(const uint8_t mac[6], const char *username) {
    if (!mac || !username) return false;

    int idx = userauth_find_key(mac, username);
    if (idx < 0) return false;   // never registered on this device — caller should fall back to Phase B
    uint8_t stored_key[32];
    memcpy(stored_key, s_userauth_keys[idx].key, 32);

    uint8_t resp[32]; size_t resp_len = 0;
    bool ok = proximity_rpc_call(mac, PAIRING_ACTION_USERAUTH_CHALLENGE,
                                  (const uint8_t *)username, strlen(username),
                                  resp, sizeof(resp), &resp_len, PAIRING_RPC_TIMEOUT_MS);
    if (!ok || resp_len != 16) return false;   // server has no key for us (expired/never registered) or unreachable

    uint8_t proof[32];
    hmac_sha256(stored_key, sizeof(stored_key), resp, 16, proof);

    userauth_verify_req_t vreq = {0};
    strncpy(vreq.username, username, sizeof(vreq.username) - 1);
    memcpy(vreq.proof, proof, sizeof(proof));

    ok = proximity_rpc_call(mac, PAIRING_ACTION_USERAUTH_VERIFY,
                             (const uint8_t *)&vreq, sizeof(vreq), resp, sizeof(resp), &resp_len,
                             PAIRING_RPC_TIMEOUT_MS);
    if (!ok || resp_len != 2 || resp[0] != 1) return false;
    bool is_admin = resp[1] != 0;

    // Success — this device is now authenticated AS username on the
    // server at mac. Do the local user_mgr bookkeeping here rather than
    // pushing it onto every caller: user_mgr_create_remote() is a no-op
    // (besides syncing is_admin) if the REMOTE record already exists (see
    // its own doc comment), so this is safe to call on every successful
    // verify, not just the first.
    if (!user_mgr_create_remote(username, is_admin, mac)) {
        ESP_LOGW(TAG, "userauth: verified '%s' but user_mgr_create_remote() failed "
                      "(name already exists as a LOCAL account?)", username);
        return false;
    }
    user_mgr_set_logged_in(username);
    ESP_LOGI(TAG, "userauth: logged in as remote user '%s' (admin=%d)", username, (int)is_admin);
    return true;
}

bool pairing_get_shared_secret(const uint8_t mac[6], uint8_t out_secret[32]) {
    if (!mac || !out_secret) return false;
    return get_paired_secret(mac, out_secret);
}

// ── Remote OOBE (client side) ────────────────────────────────────────────
// See pairing.h's own "Remote OOBE" doc comment.

bool pairing_remote_oobe_needed(const uint8_t mac[6]) {
    if (!mac) return false;
    uint8_t resp[4]; size_t resp_len = 0;
    bool ok = proximity_rpc_call(mac, PAIRING_ACTION_OOBE_QUERY, NULL, 0,
                                  resp, sizeof(resp), &resp_len, PAIRING_RPC_TIMEOUT_MS);
    // Any failure — unreachable, wrong action, malformed response — reads
    // as "not needed" (see pairing.h's own doc comment: "false on any RPC
    // failure too — 'don't offer it' is the safe default"), not "needed".
    if (!ok || resp_len != 1) return false;
    return resp[0] == 1;
}

bool pairing_remote_oobe_push(const uint8_t mac[6], const char *username, const char *password) {
    if (!mac) return false;

    uint8_t secret[32];
    if (!get_paired_secret(mac, secret)) return false;

    oobe_payload_t payload = {0};
    if (username) strncpy(payload.username, username, sizeof(payload.username) - 1);
    if (password) strncpy(payload.password, password, sizeof(payload.password) - 1);

    oobe_push_req_t req = {0};
    esp_fill_random(req.nonce, sizeof(req.nonce));
    uint8_t msg_key[32];
    derive_msg_key(secret, req.nonce, msg_key);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, msg_key, 256);
    mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, sizeof(payload), req.nonce, sizeof(req.nonce), NULL, 0,
                               (const uint8_t *)&payload, req.ciphertext, sizeof(req.tag), req.tag);
    mbedtls_gcm_free(&gcm);
    memset(&payload, 0, sizeof(payload));   // done with the plaintext copy — no reason to keep it around

    uint8_t resp[4]; size_t resp_len = 0;
    bool ok = proximity_rpc_call(mac, PAIRING_ACTION_OOBE_PUSH,
                                  (const uint8_t *)&req, sizeof(req), resp, sizeof(resp), &resp_len,
                                  PAIRING_RPC_TIMEOUT_MS);
    return ok && resp_len == 1 && resp[0] == 1;
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

// Exercises the Phase B/C RPC handlers directly (not through
// proximity_rpc_call(), which needs two real devices) against a fake
// paired-device secret (Phase A's own selftest above already proved the
// ECDH math that would normally produce one) and a real throwaway
// user_mgr account (the handlers call into user_mgr for real, so this
// needs a real account to exercise against). Cleans up everything it
// creates either way.
void pairing_selftest_userauth(void) {
    static const char *user = "userauth_selftest";
    static const char *pass_word = "testpass123";
    static const uint8_t fake_mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    uint8_t fake_secret[32];
    esp_fill_random(fake_secret, sizeof(fake_secret));

    bool have_user = user_mgr_exists(user) || user_mgr_create(user, pass_word);
    ESP_LOGI(TAG, "userauth selftest: have_user=%d", have_user);
    if (!have_user) { ESP_LOGE(TAG, "userauth selftest: SELFTEST FAIL (user create)"); return; }

    add_or_update_paired(fake_mac, "SelftestPeer", fake_secret);

    bool pass = true;

    // ── Phase B ──────────────────────────────────────────────────────
    uint8_t salt[16]; size_t salt_len = 0;
    bool ok = handle_userauth_salt(fake_mac, PAIRING_ACTION_USERAUTH_SALT,
                                    (const uint8_t *)user, strlen(user), salt, sizeof(salt), &salt_len);
    ESP_LOGI(TAG, "userauth selftest: salt ok=%d len=%u", ok, (unsigned)salt_len);
    pass = pass && ok && salt_len == 16;

    uint8_t pwhash[32];
    user_mgr_hash_password(salt, pass_word, pwhash);

    userauth_request_req_t req = {0};
    strncpy(req.username, user, sizeof(req.username) - 1);
    esp_fill_random(req.nonce, sizeof(req.nonce));
    uint8_t msg_key[32];
    derive_msg_key(fake_secret, req.nonce, msg_key);
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, msg_key, 256);
    mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, 32, req.nonce, sizeof(req.nonce), NULL, 0,
                               pwhash, req.ciphertext, sizeof(req.tag), req.tag);
    mbedtls_gcm_free(&gcm);

    uint8_t resp1[4]; size_t resp1_len = 0;
    ok = handle_userauth_request(fake_mac, PAIRING_ACTION_USERAUTH_REQUEST,
                                  (const uint8_t *)&req, sizeof(req), resp1, sizeof(resp1), &resp1_len);
    ESP_LOGI(TAG, "userauth selftest: request ok=%d status=%u (expect 1=pending)",
             ok, resp1_len ? resp1[0] : 255);
    pass = pass && ok && resp1_len == 1 && resp1[0] == 1;

    pairing_confirm_user_access();
    ESP_LOGI(TAG, "userauth selftest: state after confirm = %d (expect APPROVED=%d)",
             (int)s_userauth_req_state, (int)USERAUTH_REQ_APPROVED);
    pass = pass && (s_userauth_req_state == USERAUTH_REQ_APPROVED);

    uint8_t client_key[32];
    esp_fill_random(client_key, sizeof(client_key));
    userauth_register_req_t reg = {0};
    strncpy(reg.username, user, sizeof(reg.username) - 1);
    esp_fill_random(reg.nonce, sizeof(reg.nonce));
    uint8_t msg_key2[32];
    derive_msg_key(fake_secret, reg.nonce, msg_key2);
    mbedtls_gcm_init(&gcm);
    mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, msg_key2, 256);
    mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, 32, reg.nonce, sizeof(reg.nonce), NULL, 0,
                               client_key, reg.ciphertext, sizeof(reg.tag), reg.tag);
    mbedtls_gcm_free(&gcm);

    uint8_t resp2[4]; size_t resp2_len = 0;
    ok = handle_userauth_register(fake_mac, PAIRING_ACTION_USERAUTH_REGISTER,
                                   (const uint8_t *)&reg, sizeof(reg), resp2, sizeof(resp2), &resp2_len);
    ESP_LOGI(TAG, "userauth selftest: register ok=%d status=%u (expect 1)", ok, resp2_len ? resp2[0] : 255);
    pass = pass && ok && resp2_len == 1 && resp2[0] == 1;

    // ── Phase C — correct proof ──────────────────────────────────────
    uint8_t challenge[16]; size_t challenge_len = 0;
    ok = handle_userauth_challenge(fake_mac, PAIRING_ACTION_USERAUTH_CHALLENGE,
                                    (const uint8_t *)user, strlen(user), challenge, sizeof(challenge), &challenge_len);
    ESP_LOGI(TAG, "userauth selftest: challenge ok=%d len=%u", ok, (unsigned)challenge_len);
    pass = pass && ok && challenge_len == 16;

    uint8_t proof[32];
    hmac_sha256(client_key, sizeof(client_key), challenge, sizeof(challenge), proof);
    userauth_verify_req_t vreq = {0};
    strncpy(vreq.username, user, sizeof(vreq.username) - 1);
    memcpy(vreq.proof, proof, sizeof(proof));
    uint8_t resp3[4]; size_t resp3_len = 0;
    ok = handle_userauth_verify(fake_mac, PAIRING_ACTION_USERAUTH_VERIFY,
                                 (const uint8_t *)&vreq, sizeof(vreq), resp3, sizeof(resp3), &resp3_len);
    // `user` is a plain user_mgr_create() LOCAL account — never admin — so
    // resp3[1] (is_admin) should come back 0 here.
    ESP_LOGI(TAG, "userauth selftest: verify (correct proof) ok=%d status=%u is_admin=%u (expect 1, 0)",
             ok, resp3_len ? resp3[0] : 255, resp3_len > 1 ? resp3[1] : 255);
    pass = pass && ok && resp3_len == 2 && resp3[0] == 1 && resp3[1] == 0;

    // ── Phase C — wrong proof must be rejected (negative case) ───────
    handle_userauth_challenge(fake_mac, PAIRING_ACTION_USERAUTH_CHALLENGE,
                               (const uint8_t *)user, strlen(user), challenge, sizeof(challenge), &challenge_len);
    userauth_verify_req_t bad_vreq = {0};
    strncpy(bad_vreq.username, user, sizeof(bad_vreq.username) - 1);
    esp_fill_random(bad_vreq.proof, sizeof(bad_vreq.proof));
    uint8_t resp4[4]; size_t resp4_len = 0;
    handle_userauth_verify(fake_mac, PAIRING_ACTION_USERAUTH_VERIFY,
                            (const uint8_t *)&bad_vreq, sizeof(bad_vreq), resp4, sizeof(resp4), &resp4_len);
    bool wrong_proof_rejected = (resp4_len == 2 && resp4[0] == 0);
    ESP_LOGI(TAG, "userauth selftest: wrong-proof correctly rejected = %d", wrong_proof_rejected);
    pass = pass && wrong_proof_rejected;

    // ── Cleanup — leave no test residue behind ────────────────────────
    remove_paired(fake_mac);
    int kidx = userauth_find_key(fake_mac, user);
    if (kidx >= 0) userauth_remove_key_at(kidx);
    user_mgr_remove(user);

    ESP_LOGI(TAG, "userauth selftest: %s", pass ? "SELFTEST PASS" : "SELFTEST FAIL");
}

// Exercises handle_oobe_query()/handle_oobe_push() directly, same
// fake-paired-peer shape as pairing_selftest_userauth() just above.
//
// The positive "correctly-encrypted push actually creates the account"
// case can only run on a device that hasn't completed its OWN OOBE yet —
// user_mgr_oobe_completed() is a real, one-way flag (see user_mgr.h's own
// doc comment: no reset-for-testing escape hatch by design), so a board
// that's already past its own setup skips that one assertion rather than
// faking a result — every gating/negative-path check below still runs and
// still means something regardless of this board's own OOBE state.
void pairing_selftest_remote_oobe(void) {
    static const uint8_t fake_mac[6]      = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFE};
    static const uint8_t untrusted_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint8_t fake_secret[32];
    esp_fill_random(fake_secret, sizeof(fake_secret));

    bool pass = true;
    bool oobe_already_done = user_mgr_oobe_completed();
    ESP_LOGI(TAG, "remote oobe selftest: this device's own oobe_completed=%d (test adapts either way)", oobe_already_done);

    // ── QUERY, untrusted mac — must read "not needed" regardless of state ──
    uint8_t qresp[4]; size_t qresp_len = 0;
    bool ok = handle_oobe_query(fake_mac, PAIRING_ACTION_OOBE_QUERY, NULL, 0, qresp, sizeof(qresp), &qresp_len);
    bool untrusted_query_denied = ok && qresp_len == 1 && qresp[0] == 0;
    ESP_LOGI(TAG, "remote oobe selftest: query (untrusted) = %u (expect 0)", qresp_len ? qresp[0] : 255);
    pass = pass && untrusted_query_denied;

    add_or_update_paired(fake_mac, "SelftestPeer", fake_secret);

    // ── QUERY, trusted — mirrors this device's REAL oobe state ──────────
    ok = handle_oobe_query(fake_mac, PAIRING_ACTION_OOBE_QUERY, NULL, 0, qresp, sizeof(qresp), &qresp_len);
    uint8_t expect_needed = oobe_already_done ? 0 : 1;
    bool trusted_query_matches = ok && qresp_len == 1 && qresp[0] == expect_needed;
    ESP_LOGI(TAG, "remote oobe selftest: query (trusted) = %u (expect %u)", qresp_len ? qresp[0] : 255, expect_needed);
    pass = pass && trusted_query_matches;

    // ── PUSH, untrusted mac — must fail before ever touching decryption ──
    oobe_payload_t junk_payload = {0};
    strncpy(junk_payload.username, "oobe_selftest_user", sizeof(junk_payload.username) - 1);
    oobe_push_req_t breq = {0};
    esp_fill_random(breq.nonce, sizeof(breq.nonce));
    uint8_t junk_key[32];
    esp_fill_random(junk_key, sizeof(junk_key));   // untrusted_mac has no real secret to encrypt under — any key proves the point
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, junk_key, 256);
    mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, sizeof(junk_payload), breq.nonce, sizeof(breq.nonce), NULL, 0,
                               (const uint8_t *)&junk_payload, breq.ciphertext, sizeof(breq.tag), breq.tag);
    mbedtls_gcm_free(&gcm);
    uint8_t presp[4]; size_t presp_len = 0;
    ok = handle_oobe_push(untrusted_mac, PAIRING_ACTION_OOBE_PUSH,
                           (const uint8_t *)&breq, sizeof(breq), presp, sizeof(presp), &presp_len);
    bool untrusted_push_denied = ok && presp_len == 1 && presp[0] == 0;
    ESP_LOGI(TAG, "remote oobe selftest: push (untrusted) = %u (expect 0)", presp_len ? presp[0] : 255);
    pass = pass && untrusted_push_denied;

    // ── PUSH, trusted mac, wrong key (tampered) — must fail either way:
    // either the oobe-already-done gate catches it first, or — on a
    // genuinely fresh device — the GCM auth-tag check does. Reuses the
    // untrusted request's bytes verbatim: wrong key for fake_mac's REAL
    // secret regardless.
    ok = handle_oobe_push(fake_mac, PAIRING_ACTION_OOBE_PUSH,
                           (const uint8_t *)&breq, sizeof(breq), presp, sizeof(presp), &presp_len);
    bool tampered_push_denied = ok && presp_len == 1 && presp[0] == 0;
    ESP_LOGI(TAG, "remote oobe selftest: push (trusted, tampered) = %u (expect 0)", presp_len ? presp[0] : 255);
    pass = pass && tampered_push_denied;

    // ── PUSH, trusted mac, correctly-encrypted — only meaningful pre-OOBE ──
    if (!oobe_already_done) {
        static const char *test_user = "oobe_selftest_user";
        oobe_payload_t good_payload = {0};
        strncpy(good_payload.username, test_user, sizeof(good_payload.username) - 1);
        strncpy(good_payload.password, "selftestpass", sizeof(good_payload.password) - 1);

        oobe_push_req_t greq = {0};
        esp_fill_random(greq.nonce, sizeof(greq.nonce));
        uint8_t msg_key[32];
        derive_msg_key(fake_secret, greq.nonce, msg_key);
        mbedtls_gcm_init(&gcm);
        mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, msg_key, 256);
        mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, sizeof(good_payload), greq.nonce, sizeof(greq.nonce), NULL, 0,
                                   (const uint8_t *)&good_payload, greq.ciphertext, sizeof(greq.tag), greq.tag);
        mbedtls_gcm_free(&gcm);

        ok = handle_oobe_push(fake_mac, PAIRING_ACTION_OOBE_PUSH,
                               (const uint8_t *)&greq, sizeof(greq), presp, sizeof(presp), &presp_len);
        bool good_push_accepted = ok && presp_len == 1 && presp[0] == 1;
        bool user_created       = user_mgr_exists(test_user);
        bool oobe_now_done      = user_mgr_oobe_completed();
        ESP_LOGI(TAG, "remote oobe selftest: push (trusted, correct) = %u user_created=%d oobe_completed=%d (expect 1, 1, 1)",
                 presp_len ? presp[0] : 255, user_created, oobe_now_done);
        pass = pass && good_push_accepted && user_created && oobe_now_done;

        // Cleanup the account — cannot un-set oobe_completed (see this
        // function's own doc comment), but that's fine: a fresh device
        // only ever runs this selftest once anyway, the same "temporary,
        // called from a boot hook, then reverted" discipline every
        // selftest in this file already follows.
        user_mgr_remove(test_user);
    } else {
        ESP_LOGW(TAG, "remote oobe selftest: SKIPPING positive push case — this device's own "
                      "OOBE is already complete (one-way flag) — gating checks above still ran");
    }

    remove_paired(fake_mac);
    ESP_LOGI(TAG, "remote oobe selftest: %s", pass ? "SELFTEST PASS" : "SELFTEST FAIL");
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

    // USERAUTH_* — Phase B/C of the remote-login work. Registered here
    // (always-on, module init) rather than scoped to any app's lifetime —
    // same reasoning app_manager_remote.c's REMOTEAPPS_ACTION_* handlers
    // already follow: the device answering a login request doesn't need
    // milkbar (or anything else) open to do it.
    proximity_rpc_register(PAIRING_ACTION_USERAUTH_SALT,      handle_userauth_salt);
    proximity_rpc_register(PAIRING_ACTION_USERAUTH_REQUEST,   handle_userauth_request);
    proximity_rpc_register(PAIRING_ACTION_USERAUTH_STATUS,    handle_userauth_status);
    proximity_rpc_register(PAIRING_ACTION_USERAUTH_REGISTER,  handle_userauth_register);
    proximity_rpc_register(PAIRING_ACTION_USERAUTH_CHALLENGE, handle_userauth_challenge);
    proximity_rpc_register(PAIRING_ACTION_USERAUTH_VERIFY,    handle_userauth_verify);

    // Remote OOBE — same "always-on, module init" reasoning as USERAUTH_*
    // just above: a fresh device needing setup has no app open at all yet,
    // so this can't be scoped to one.
    proximity_rpc_register(PAIRING_ACTION_OOBE_QUERY, handle_oobe_query);
    proximity_rpc_register(PAIRING_ACTION_OOBE_PUSH,  handle_oobe_push);

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
    proximity_rpc_register(PAIRING_ACTION_USERAUTH_SALT,      NULL);
    proximity_rpc_register(PAIRING_ACTION_USERAUTH_REQUEST,   NULL);
    proximity_rpc_register(PAIRING_ACTION_USERAUTH_STATUS,    NULL);
    proximity_rpc_register(PAIRING_ACTION_USERAUTH_REGISTER,  NULL);
    proximity_rpc_register(PAIRING_ACTION_USERAUTH_CHALLENGE, NULL);
    proximity_rpc_register(PAIRING_ACTION_USERAUTH_VERIFY,    NULL);
    proximity_rpc_register(PAIRING_ACTION_OOBE_QUERY, NULL);
    proximity_rpc_register(PAIRING_ACTION_OOBE_PUSH,  NULL);
    s_state = PAIRING_STATE_NONE;
    s_userauth_req_state = USERAUTH_REQ_NONE;
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
