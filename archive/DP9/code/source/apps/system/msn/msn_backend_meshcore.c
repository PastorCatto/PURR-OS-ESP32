// msn_backend_meshcore.c — msn_backend_t implementation wrapping
// meshcore_api.h. Close to a direct passthrough: mc_manager_*'s shapes
// already resemble the unified interface (contacts and channels are both
// already index-addressed, and mc_rx_cb_t's signature is structurally
// identical to msn_rx_cb_t — no translation shim needed, unlike the
// meshtastic backend's node-id-to-index lookup).

#include "msn_backend.h"
#include "meshcore_api.h"
#include <string.h>
#include <stdio.h>

static bool backend_ready(void)    { return mc_manager_ready(); }
static bool backend_is_alive(void) { return mc_manager_is_alive(); }

static int backend_contact_count(void) { return mc_manager_contact_count(); }

static bool backend_contact_at(int idx, msn_contact_t *out) {
    mc_contact_info_t info;
    if (!mc_manager_contact_at(idx, &info)) return false;

    // Hex prefix of the pubkey — MeshCore contacts have no node-id-shaped
    // identity, so id_str is shorter than meshtastic's full 8-hex-digit id.
    snprintf(out->id_str, sizeof(out->id_str), "%02X%02X%02X%02X",
             info.pub_key[0], info.pub_key[1], info.pub_key[2], info.pub_key[3]);
    strncpy(out->name, info.name[0] ? info.name : out->id_str, sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = 0;
    out->channel_idx = -1;   // no per-contact channel concept — DMs use per-contact ECDH
    // mc_contact_info_t (meshcore_api.h) has no rssi/hop/telemetry fields at
    // all — confirmed by reading it, not assumed. MeshCore's protocol simply
    // doesn't carry any of this today.
    out->rssi_dbm    = MSN_RSSI_UNKNOWN;
    out->hops_away   = MSN_HOPS_UNKNOWN;
    out->battery_pct = MSN_BATTERY_UNKNOWN;

    if (info.last_advert_timestamp == 0) {
        out->last_seen_ms_ago = MSN_LAST_SEEN_UNKNOWN;
    } else {
        uint32_t now_s = mc_manager_now_seconds();
        uint32_t ago_s = now_s - info.last_advert_timestamp;
        out->last_seen_ms_ago = ago_s * 1000u;
    }
    return true;
}

static void backend_contact_forget(int idx) {
    mc_manager_contact_forget(idx);
}

static int backend_channel_count(void) { return mc_manager_channel_count(); }

static bool backend_channel_name(int idx, char *out, size_t max) {
    return mc_manager_channel_name(idx, out, max);
}

static int backend_channel_add(const char *name, const uint8_t *psk, size_t psk_len) {
    return mc_manager_channel_add(name, psk, psk_len);
}

static void backend_channel_remove(int idx) {
    mc_manager_channel_remove(idx);
}

static bool backend_channel_hash(int idx, uint8_t *hash_out) {
    return mc_manager_channel_hash(idx, hash_out);
}

static bool backend_send_text(int to_contact_idx, int channel_idx, const char *text) {
    // channel_idx is passed through as-is (ignored by mc_manager_send_text()
    // when to_contact_idx >= 0, same "DM ignores channel" contract as the
    // meshtastic backend — MeshCore just doesn't need a substitution step,
    // since it has no per-contact channel to look up).
    return mc_manager_send_text(to_contact_idx, channel_idx, text);
}

static void backend_add_rx_cb(msn_rx_cb_t cb) {
    // mc_rx_cb_t and msn_rx_cb_t are structurally identical
    // (void(*)(int, int, const char*)) — cast, no shim needed.
    mc_manager_add_rx_callback((mc_rx_cb_t)cb);
}

static void backend_remove_rx_cb(msn_rx_cb_t cb) {
    mc_manager_remove_rx_callback((mc_rx_cb_t)cb);
}

static const msn_backend_t s_backend = {
    .name           = "MeshCore",
    .ready          = backend_ready,
    .is_alive       = backend_is_alive,
    .contact_count  = backend_contact_count,
    .contact_at     = backend_contact_at,
    .contact_forget = backend_contact_forget,
    .channel_count  = backend_channel_count,
    .channel_name   = backend_channel_name,
    .channel_add    = backend_channel_add,
    .channel_remove = backend_channel_remove,
    .channel_hash   = backend_channel_hash,
    .send_text      = backend_send_text,
    .add_rx_cb      = backend_add_rx_cb,
    .remove_rx_cb   = backend_remove_rx_cb,
};

const msn_backend_t *msn_backend_meshcore(void) { return &s_backend; }
