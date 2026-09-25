// msn_backend_meshtastic.c — msn_backend_t implementation wrapping
// meshtastic.h. Contacts are addressed by 32-bit node id upstream, but the
// unified msn_backend_t interface addresses everything by table index —
// this file owns the index<->node-id translation, replicating meshchat.c's
// original find_buddy_idx()-by-scan approach (Meshtastic's own API has no
// id-to-index lookup either, so this isn't a regression, just relocated).

#include "msn_backend.h"
#include "meshtastic.h"
#include "meshtastic/portnums.pb.h"
#include "purr_kernel.h"
#include "pairing.h"
#include "proximity_rpc.h"
#include "homebase.h"
#include "msn_relay.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static bool backend_ready(void)    { return mesh_manager_ready(); }
static bool backend_is_alive(void) { return mesh_manager_is_alive(); }

// ── Home-base relay + fresh-slate remote view (Phase D2) ────────────────────
// See msn_backend.h's comment block and homebase.h. Automatic by default;
// s_force_local is a runtime-only (not persisted) manual override from
// msn.c's chooser UI. msn_mt_relay_is_active() gates BOTH the TX relay
// below and the read-side switch (contact/channel/history all come from
// the home base's own cache instead of this device's local mesh_manager_*
// calls while true) — declared forward since backend_contact_count() etc.
// need it before the relay dispatch code that also uses it appears below.
// msn_mt_relay_is_active() is declared in msn_backend.h (included above);
// defined further down, alongside relay_dispatch_hash().
static bool s_force_local = false;

// Remote-state cache, kept fresh by remote_poll_task() below (~5s cadence,
// idle no-op whenever msn_mt_relay_is_active() is false). Plain reads from
// backend_contact_at()/etc — no lock, same "background task writes, UI
// reads, benign eventual consistency" convention as nearby_app.c/
// milkbar_app.c's own refresh_task() + cache pattern.
static msn_relay_contact_t s_remote_contacts[MSN_RELAY_MAX_CONTACTS];
static int                 s_remote_contact_count = 0;
static msn_relay_channel_t s_remote_channels[MSN_RELAY_MAX_CHANNELS];
static int                 s_remote_channel_count = 0;
static uint32_t            s_history_since_seq = 0;

static int backend_contact_count(void) {
    if (msn_mt_relay_is_active()) return s_remote_contact_count;
    return mesh_manager_node_count();
}

static bool backend_contact_at(int idx, msn_contact_t *out) {
    if (msn_mt_relay_is_active()) {
        if (idx < 0 || idx >= s_remote_contact_count) return false;
        const msn_relay_contact_t *c = &s_remote_contacts[idx];
        snprintf(out->id_str, sizeof(out->id_str), "%08lX", (unsigned long)c->node_id);
        strncpy(out->name, c->name, sizeof(out->name) - 1);
        out->name[sizeof(out->name) - 1] = 0;
        out->channel_idx      = -1;   // not a local index — remote sends key off channel_hash instead, see relay_dispatch_hash()
        out->rssi_dbm          = c->rssi_dbm;
        out->hops_away         = c->hops_away;
        out->battery_pct       = c->battery_pct;
        out->last_seen_ms_ago  = c->last_seen_ms_ago;
        return true;
    }

    mesh_node_info_t info;
    if (mesh_manager_node_at(idx, &info) != 0) return false;

    snprintf(out->id_str, sizeof(out->id_str), "%08lX", (unsigned long)info.id);
    strncpy(out->name, info.long_name, sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = 0;
    out->channel_idx = info.channel_idx;
    out->rssi_dbm    = info.rssi;
    out->hops_away   = info.hops_away;
    out->battery_pct = info.battery_pct;   // already -1-if-unknown, same convention as MSN_BATTERY_UNKNOWN

    if (info.last_ms == 0) {
        out->last_seen_ms_ago = MSN_LAST_SEEN_UNKNOWN;
    } else {
        uint32_t now_ms = (uint32_t)purr_kernel_uptime_ms();
        out->last_seen_ms_ago = now_ms - info.last_ms;
    }
    return true;
}

static void backend_contact_forget(int idx) {
    if (msn_mt_relay_is_active()) return;   // remote cache isn't something "forget" makes sense on locally
    mesh_node_info_t info;
    if (mesh_manager_node_at(idx, &info) != 0) return;
    mesh_manager_node_forget(info.id);
}

static int backend_channel_count(void) {
    if (msn_mt_relay_is_active()) return s_remote_channel_count;
    return mesh_manager_channel_count();
}

static bool backend_channel_name(int idx, char *out, size_t max) {
    if (msn_mt_relay_is_active()) {
        if (idx < 0 || idx >= s_remote_channel_count) return false;
        snprintf(out, max, "%s", s_remote_channels[idx].name);
        return true;
    }
    return mesh_manager_channel_name(idx, out, max);
}

static int backend_channel_add(const char *name, const uint8_t *psk, size_t psk_len) {
    if (msn_mt_relay_is_active()) return -1;   // adding rooms remotely isn't in scope this pass
    if (psk_len != 16) return -1;   // mesh_manager_add_channel() requires exactly 16 bytes
    return mesh_manager_add_channel(name, psk);
}

static void backend_channel_remove(int idx) {
    if (msn_mt_relay_is_active()) return;
    mesh_manager_remove_channel(idx);
}

static bool backend_channel_hash(int idx, uint8_t *hash_out) {
    if (msn_mt_relay_is_active()) {
        if (idx < 0 || idx >= s_remote_channel_count) return false;
        *hash_out = s_remote_channels[idx].hash;
        return true;
    }
    return mesh_manager_channel_hash(idx, hash_out);
}

typedef struct {
    uint8_t  home_base_mac[6];
    uint32_t to_node_id;
    uint8_t  channel_hash;
    bool     allow_local_fallback;   // false in remote mode — see relay_dispatch_hash()'s comment
    int      local_channel_idx;      // only meaningful/used when allow_local_fallback
    char     text[MSN_RELAY_MAX_TEXT];
} relay_ctx_t;

static void relay_send_task(void *arg) {
    relay_ctx_t *ctx = (relay_ctx_t *)arg;

    size_t text_len = strlen(ctx->text);
    uint8_t payload[MSN_RELAY_HEADER_LEN + MSN_RELAY_MAX_TEXT];
    memcpy(payload, &ctx->to_node_id, sizeof(ctx->to_node_id));
    payload[4] = ctx->channel_hash;
    memcpy(payload + MSN_RELAY_HEADER_LEN, ctx->text, text_len);

    uint8_t resp[4]; size_t resp_len = 0;
    bool ok = proximity_rpc_call(ctx->home_base_mac, MSN_RELAY_ACTION_SEND_TEXT,
                                  payload, MSN_RELAY_HEADER_LEN + text_len,
                                  resp, sizeof(resp), &resp_len, 2500);
    if (!ok && ctx->allow_local_fallback) {
        // Home base didn't answer in time (out of range mid-send, unknown
        // channel on its side, ...) — fall back to the local radio so the
        // message still goes out instead of silently vanishing. Only valid
        // in LOCAL mode, where the contact/channel is known to exist on
        // this device's own tables too — a remote-mode send has no local
        // equivalent to fall back to (see relay_dispatch_hash()).
        mesh_manager_send_text(ctx->to_node_id, ctx->local_channel_idx, ctx->text);
    }
    free(ctx);
    vTaskDeleteWithCaps(NULL);
}

// Common dispatch, given a channel HASH directly (not a local index) —
// used by both local mode (hash derived from a local channel_idx just
// before calling this) and remote mode (hash comes straight from the
// remote cache, which may not correspond to anything in this device's own
// channel table at all). allow_local_fallback must be false for a
// remote-mode send: falling back to this device's own radio would be
// semantically wrong (the contact/channel may not exist locally), so a
// remote-mode send that the home base doesn't answer just fails — matches
// this pass's "MSN ignores local when a remote host is there" scope.
// Returns false only if the background task itself couldn't be queued —
// NOT a signal of the RPC's own eventual success/failure.
static bool relay_dispatch_hash(const uint8_t home_base_mac[6], uint32_t to_node_id,
                                 uint8_t channel_hash, const char *text,
                                 bool allow_local_fallback, int local_channel_idx) {
    size_t text_len = strlen(text);
    if (text_len >= MSN_RELAY_MAX_TEXT) text_len = MSN_RELAY_MAX_TEXT - 1;   // truncate rather than reject

    relay_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) return false;
    memcpy(ctx->home_base_mac, home_base_mac, 6);
    ctx->to_node_id            = to_node_id;
    ctx->channel_hash          = channel_hash;
    ctx->allow_local_fallback  = allow_local_fallback;
    ctx->local_channel_idx     = local_channel_idx;
    memcpy(ctx->text, text, text_len);
    ctx->text[text_len] = 0;

    // proximity_rpc_call() must never run on cupcake_task — dedicated
    // background task per send, same rule as Milkbar's refresh_task().
    // PSRAM-backed stack: no NVS/flash touched anywhere in this task's body.
    TaskHandle_t task = NULL;
    BaseType_t ok = xTaskCreateWithCaps(relay_send_task, "msn_relay_tx", 4096, ctx, 3, &task, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) { free(ctx); return false; }
    return true;
}

bool msn_mt_relay_is_active(void) {
    if (s_force_local) return false;
    if (!homebase_is_present()) return false;
    uint8_t hb_mac[6];
    return pairing_get_home_base(hb_mac);
}

void msn_mt_relay_set_force_local(bool force) { s_force_local = force; }
bool msn_mt_relay_get_force_local(void)        { return s_force_local; }

static bool backend_send_text(int to_contact_idx, int channel_idx, const char *text) {
    uint8_t hb_mac[6];

    if (msn_mt_relay_is_active() && pairing_get_home_base(hb_mac)) {
        // Remote mode: to_contact_idx/channel_idx index the REMOTE cache
        // (s_remote_contacts[]/s_remote_channels[]) — resolve node id +
        // channel hash directly from there, no local mesh_manager_* lookup
        // at all (the identity may not exist on this device's own tables).
        uint32_t to_node_id; uint8_t hash;
        if (to_contact_idx >= 0) {
            if (to_contact_idx >= s_remote_contact_count) return false;
            to_node_id = s_remote_contacts[to_contact_idx].node_id;
            hash       = s_remote_contacts[to_contact_idx].channel_hash;
        } else {
            if (channel_idx < 0 || channel_idx >= s_remote_channel_count) return false;
            to_node_id = MESH_BROADCAST;
            hash       = s_remote_channels[channel_idx].hash;
        }
        return relay_dispatch_hash(hb_mac, to_node_id, hash, text, /*allow_local_fallback=*/false, 0);
    }

    // Local mode (unchanged behavior from the first relay pass).
    uint32_t to_node_id;
    int      resolved_channel_idx;
    if (to_contact_idx >= 0) {
        // DM: channel_idx is ignored — the contact's own last-heard channel
        // picks which PSK to encode with, not whatever the caller passed.
        mesh_node_info_t info;
        if (mesh_manager_node_at(to_contact_idx, &info) != 0) return false;
        to_node_id = info.id;
        resolved_channel_idx = info.channel_idx;
    } else {
        if (channel_idx < 0) return false;
        to_node_id = MESH_BROADCAST;
        resolved_channel_idx = channel_idx;
    }

    return mesh_manager_send_text(to_node_id, resolved_channel_idx, text);
}

// ── RX fan-out ───────────────────────────────────────────────────────────
// meshtastic.h's own callback delivers raw (from_node, to_node, channel_idx,
// portnum, payload, len) — this shim filters to text messages, resolves
// from_node to a contact index, and fans out to every locally-registered
// msn_rx_cb_t. Only one shim is ever registered with mesh_manager_add_rx_
// callback() regardless of how many msn_rx_cb_t subscribers MSN itself adds.

#define MAX_LOCAL_RX_CB 4
static msn_rx_cb_t s_local_cbs[MAX_LOCAL_RX_CB];
static bool        s_shim_registered = false;

static int find_contact_idx_by_node_id(uint32_t node_id) {
    int n = mesh_manager_node_count();
    for (int i = 0; i < n; i++) {
        mesh_node_info_t info;
        if (mesh_manager_node_at(i, &info) != 0) break;
        if (info.id == node_id) return i;
    }
    return -1;
}

static void rx_shim(uint32_t from_node, uint32_t to_node, int channel_idx, int portnum,
                     const uint8_t *payload, size_t len) {
    // "MSN ignores local when a remote host is there" — the local radio
    // keeps running (mesh_ble.c/oled_ui_module.c have their own independent
    // registrations, unaffected), MSN just stops forwarding what it hears
    // into its own UI while remote_poll_task() below is feeding it instead.
    if (msn_mt_relay_is_active()) return;
    if (portnum != (int)meshtastic_PortNum_TEXT_MESSAGE_APP) return;

    char text[241];
    size_t n = len < sizeof(text) - 1 ? len : sizeof(text) - 1;
    memcpy(text, payload, n);
    text[n] = 0;

    int contact_idx = find_contact_idx_by_node_id(from_node);
    int out_channel_idx = (to_node == (uint32_t)MESH_BROADCAST) ? channel_idx : -1;

    for (int i = 0; i < MAX_LOCAL_RX_CB; i++) {
        if (s_local_cbs[i]) s_local_cbs[i](contact_idx, out_channel_idx, text);
    }
}

static void backend_add_rx_cb(msn_rx_cb_t cb) {
    if (!cb) return;
    if (!s_shim_registered) {
        mesh_manager_add_rx_callback(rx_shim);
        s_shim_registered = true;
    }
    for (int i = 0; i < MAX_LOCAL_RX_CB; i++) {
        if (s_local_cbs[i] == cb) return;
        if (!s_local_cbs[i]) { s_local_cbs[i] = cb; return; }
    }
}

static void backend_remove_rx_cb(msn_rx_cb_t cb) {
    for (int i = 0; i < MAX_LOCAL_RX_CB; i++) {
        if (s_local_cbs[i] == cb) { s_local_cbs[i] = NULL; return; }
    }
}

// ── Remote poll task (Phase D2) ─────────────────────────────────────────────
// Keeps s_remote_contacts[]/s_remote_channels[] fresh and forwards new
// home-base history into the same s_local_cbs[] fan-out rx_shim() uses for
// local RX — msn.c's on_mesh_rx() can't tell the two apart. Always running
// once lazily started (see msn_backend_meshtastic()'s ensure_poll_task_
// started() below); idles at the vTaskDelay when msn_mt_relay_is_active()
// is false, same "cheap no-op when inactive" shape as homebase.c's own poll.

static int find_remote_contact_idx_by_node_id(uint32_t node_id) {
    for (int i = 0; i < s_remote_contact_count; i++) {
        if (s_remote_contacts[i].node_id == node_id) return i;
    }
    return -1;
}

static int find_remote_channel_idx_by_hash(uint8_t hash) {
    for (int i = 0; i < s_remote_channel_count; i++) {
        if (s_remote_channels[i].hash == hash) return i;
    }
    return -1;
}

static void poll_remote_contacts(const uint8_t hb_mac[6]) {
    uint8_t resp[MSN_RELAY_MAX_CONTACTS * sizeof(msn_relay_contact_t)];
    size_t resp_len = 0;
    if (!proximity_rpc_call(hb_mac, MSN_RELAY_ACTION_LIST_CONTACTS, NULL, 0,
                             resp, sizeof(resp), &resp_len, 2500)) {
        return;   // leave the existing cache in place — a transient miss shouldn't blank the UI
    }
    int n = (int)(resp_len / sizeof(msn_relay_contact_t));
    if (n > MSN_RELAY_MAX_CONTACTS) n = MSN_RELAY_MAX_CONTACTS;
    memcpy(s_remote_contacts, resp, (size_t)n * sizeof(msn_relay_contact_t));
    s_remote_contact_count = n;
}

static void poll_remote_channels(const uint8_t hb_mac[6]) {
    uint8_t resp[MSN_RELAY_MAX_CHANNELS * sizeof(msn_relay_channel_t)];
    size_t resp_len = 0;
    if (!proximity_rpc_call(hb_mac, MSN_RELAY_ACTION_LIST_CHANNELS, NULL, 0,
                             resp, sizeof(resp), &resp_len, 2500)) {
        return;
    }
    int n = (int)(resp_len / sizeof(msn_relay_channel_t));
    if (n > MSN_RELAY_MAX_CHANNELS) n = MSN_RELAY_MAX_CHANNELS;
    memcpy(s_remote_channels, resp, (size_t)n * sizeof(msn_relay_channel_t));
    s_remote_channel_count = n;
}

static void poll_remote_history(const uint8_t hb_mac[6]) {
    uint8_t req[sizeof(uint32_t)];
    memcpy(req, &s_history_since_seq, sizeof(req));

    uint8_t resp[4 * sizeof(msn_relay_history_entry_t)];   // matches msn_relay.c's MAX_HISTORY_PER_RESPONSE=4
    size_t resp_len = 0;
    if (!proximity_rpc_call(hb_mac, MSN_RELAY_ACTION_GET_HISTORY, req, sizeof(req),
                             resp, sizeof(resp), &resp_len, 2500)) {
        return;
    }
    int n = (int)(resp_len / sizeof(msn_relay_history_entry_t));
    for (int i = 0; i < n; i++) {
        msn_relay_history_entry_t e;
        memcpy(&e, resp + i * sizeof(e), sizeof(e));
        s_history_since_seq = e.seq;

        int contact_idx, channel_idx;
        if (e.to_node == (uint32_t)MESH_BROADCAST) {
            contact_idx = -1;
            channel_idx = find_remote_channel_idx_by_hash(e.channel_hash);
            if (channel_idx < 0) continue;   // a room we don't have in the just-fetched channel cache
        } else {
            contact_idx = find_remote_contact_idx_by_node_id(e.from_node);
            channel_idx = -1;
            if (contact_idx < 0) continue;
        }
        for (int cb = 0; cb < MAX_LOCAL_RX_CB; cb++) {
            if (s_local_cbs[cb]) s_local_cbs[cb](contact_idx, channel_idx, e.text);
        }
    }
}

static void remote_poll_task(void *arg) {
    (void)arg;
    for (;;) {
        if (msn_mt_relay_is_active()) {
            uint8_t hb_mac[6];
            if (pairing_get_home_base(hb_mac)) {
                poll_remote_contacts(hb_mac);
                poll_remote_channels(hb_mac);
                poll_remote_history(hb_mac);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static const msn_backend_t s_backend = {
    .name           = "Meshtastic",
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

// Started once, ever — remote_poll_task() idles cheaply via its own
// msn_mt_relay_is_active() check when there's nothing to do, so there's no
// need to tear it down between chooser switches/relaunches (same
// always-on-but-idle shape as homebase.c's own presence-watcher task).
static bool s_poll_task_started = false;

const msn_backend_t *msn_backend_meshtastic(void) {
    if (!s_poll_task_started) {
        s_poll_task_started = true;
        TaskHandle_t task = NULL;
        // 8192, not a smaller size — poll_remote_history() fans new
        // messages out through the SAME s_local_cbs[] callbacks local RX
        // uses, which reach into msn.c's chat_log_append()/room_log_
        // append() and their append_to_sd() SD-file writes. That's real
        // depth (VFS/FATFS/SDMMC stack frames), not just this task's own
        // ~1KB of local response buffers. mesh_task() in meshtastic_
        // module.c hits this exact same on_mesh_rx() call chain today on
        // an 8192-byte PSRAM stack — matching that proven size here after
        // a live stack-overflow crash (vApplicationStackOverflowHook,
        // confirmed via backtrace symbolization) at the smaller 6144.
        xTaskCreateWithCaps(remote_poll_task, "msn_remote_poll", 8192, NULL, 3, &task, MALLOC_CAP_SPIRAM);
    }
    return &s_backend;
}
