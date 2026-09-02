// msn_relay.c — home-base-side responder for MSN's automatic radio relay.
// Pure wrapper over meshtastic.h's existing public API (mesh_manager_send_
// text(), mesh_manager_channel_count()/_hash()) — no new local send/
// channel behavior, just a new (remote) caller for what already exists.
//
// A separate module from msn.c/msn_backend_meshtastic.c specifically so a
// headless, non-purr_win device (e.g. Heltec V3) can answer relay requests
// even though MSN's own GUI can never run there — same reasoning
// app_manager_remote.c documents for why it isn't folded into
// app_manager.c. Auto-added by purrstrap.py's apply_radio_companion_
// defaults() alongside proximity_rpc/app_manager_remote/homebase, same
// condition (radio.wifi=true).

#include "msn_relay.h"
#include "meshtastic.h"
#include "meshtastic/portnums.pb.h"
#include "proximity_rpc.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/core/purr_module.h"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR — see s_history's own comment below
#include <string.h>

// ── Fresh-slate remote view (Phase D2) — history ring buffer ───────────────
// Nothing below meshtastic_module.c retains message TEXT anywhere (only
// telemetry survives in mesh_router.c's node table) — this module has to be
// the one keeping it, or GET_HISTORY has nothing to serve. Own rx callback
// registration, independent of MSN (which never runs on the same headless
// device this module targets) — a 4th consumer alongside MSN/mesh_ble/
// oled_ui, still within meshtastic.h's MESH_MAX_RX_CB=4 cap since MSN and
// msn_relay never both run on one device.
#define HISTORY_RING_SIZE 32
#define MAX_HISTORY_PER_RESPONSE 4   // keeps a GET_HISTORY response (~852 bytes) inside a shared 1024-byte caller-side buffer

// EXT_RAM_BSS_ATTR (PSRAM, not internal DRAM) — a passive ring buffer, never
// touched before PSRAM is up (this module registers like any other kernel
// module, well after boot's PSRAM init). Safe no-op on a PSRAM-less board
// (CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY unset there — see esp_attr.h)
// — relevant since this module also targets headless devices like Heltec
// V3 per this file's own header comment. Same class MiniWin's own control/
// list arrays already use this for — real, confirmed via purr_os.map: a
// genuine link-time DRAM overflow once the current app set was all
// compiled in together (see msn.c's matching comment, 2026-09-01).
static EXT_RAM_BSS_ATTR msn_relay_history_entry_t s_history[HISTORY_RING_SIZE];
static uint32_t s_history_next_seq = 1;   // 0 is never assigned, so since_seq=0 always means "from the start"

static void on_mesh_rx_for_history(uint32_t from_node, uint32_t to_node, int channel_idx, int portnum,
                                    const uint8_t *payload, size_t len) {
    if (portnum != (int)meshtastic_PortNum_TEXT_MESSAGE_APP) return;

    uint8_t hash = 0;
    if (to_node == (uint32_t)MESH_BROADCAST) {
        if (!mesh_manager_channel_hash(channel_idx, &hash)) return;
    }

    msn_relay_history_entry_t *e = &s_history[s_history_next_seq % HISTORY_RING_SIZE];
    e->seq          = s_history_next_seq++;
    e->from_node    = from_node;
    e->to_node      = to_node;
    e->channel_hash = hash;
    size_t n = len < sizeof(e->text) - 1 ? len : sizeof(e->text) - 1;
    memcpy(e->text, payload, n);
    e->text[n] = 0;
}

static bool handle_list_contacts(const uint8_t mac[6], uint16_t action_id,
                                  const uint8_t *req, size_t req_len,
                                  uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out) {
    (void)mac; (void)action_id; (void)req; (void)req_len;
    int n = mesh_manager_node_count();
    size_t max_entries = resp_cap / sizeof(msn_relay_contact_t);
    if ((size_t)n > max_entries) n = (int)max_entries;
    if (n > MSN_RELAY_MAX_CONTACTS) n = MSN_RELAY_MAX_CONTACTS;

    msn_relay_contact_t *out = (msn_relay_contact_t *)resp_out;
    int written = 0;
    uint32_t now_ms = (uint32_t)purr_kernel_uptime_ms();
    for (int i = 0; i < n; i++) {
        mesh_node_info_t info;
        if (mesh_manager_node_at(i, &info) != 0) continue;
        msn_relay_contact_t *c = &out[written];
        c->node_id = info.id;
        strncpy(c->name, info.long_name, sizeof(c->name) - 1);
        c->name[sizeof(c->name) - 1] = 0;
        if (!mesh_manager_channel_hash(info.channel_idx, &c->channel_hash)) c->channel_hash = 0;
        c->rssi_dbm     = info.rssi;
        c->hops_away    = info.hops_away;
        c->battery_pct  = info.battery_pct;
        c->last_seen_ms_ago = info.last_ms == 0 ? MSN_LAST_SEEN_UNKNOWN_WIRE : now_ms - info.last_ms;
        written++;
    }
    *resp_len_out = (size_t)written * sizeof(msn_relay_contact_t);
    return true;
}

static bool handle_list_channels(const uint8_t mac[6], uint16_t action_id,
                                  const uint8_t *req, size_t req_len,
                                  uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out) {
    (void)mac; (void)action_id; (void)req; (void)req_len;
    int n = mesh_manager_channel_count();
    size_t max_entries = resp_cap / sizeof(msn_relay_channel_t);
    if ((size_t)n > max_entries) n = (int)max_entries;
    if (n > MSN_RELAY_MAX_CHANNELS) n = MSN_RELAY_MAX_CHANNELS;

    msn_relay_channel_t *out = (msn_relay_channel_t *)resp_out;
    int written = 0;
    for (int i = 0; i < n; i++) {
        msn_relay_channel_t *c = &out[written];
        if (!mesh_manager_channel_name(i, c->name, sizeof(c->name))) continue;
        if (!mesh_manager_channel_hash(i, &c->hash)) continue;
        written++;
    }
    *resp_len_out = (size_t)written * sizeof(msn_relay_channel_t);
    return true;
}

static bool handle_get_history(const uint8_t mac[6], uint16_t action_id,
                                const uint8_t *req, size_t req_len,
                                uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out) {
    (void)mac; (void)action_id;
    *resp_len_out = 0;
    if (req_len < sizeof(uint32_t)) return false;
    uint32_t since_seq;
    memcpy(&since_seq, req, sizeof(since_seq));

    // Oldest entry still held, given the ring may have wrapped.
    uint32_t oldest_seq = s_history_next_seq > HISTORY_RING_SIZE ? s_history_next_seq - HISTORY_RING_SIZE : 1;
    uint32_t start_seq = since_seq + 1;
    if (start_seq < oldest_seq) start_seq = oldest_seq;   // caller fell behind the ring — skip to what's left

    size_t max_entries = resp_cap / sizeof(msn_relay_history_entry_t);
    if (max_entries > MAX_HISTORY_PER_RESPONSE) max_entries = MAX_HISTORY_PER_RESPONSE;

    msn_relay_history_entry_t *out = (msn_relay_history_entry_t *)resp_out;
    size_t written = 0;
    for (uint32_t seq = start_seq; seq < s_history_next_seq && written < max_entries; seq++) {
        out[written++] = s_history[seq % HISTORY_RING_SIZE];
    }
    *resp_len_out = written * sizeof(msn_relay_history_entry_t);
    return true;
}

static bool handle_send_text(const uint8_t mac[6], uint16_t action_id,
                              const uint8_t *req, size_t req_len,
                              uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out) {
    (void)mac; (void)action_id; (void)resp_out; (void)resp_cap;
    *resp_len_out = 0;
    if (req_len < MSN_RELAY_HEADER_LEN) return false;

    uint32_t to_node_id;
    memcpy(&to_node_id, req, sizeof(to_node_id));
    uint8_t ch_hash = req[4];

    size_t text_len = req_len - MSN_RELAY_HEADER_LEN;
    if (text_len == 0 || text_len >= MSN_RELAY_MAX_TEXT) return false;
    char text[MSN_RELAY_MAX_TEXT];
    memcpy(text, req + MSN_RELAY_HEADER_LEN, text_len);
    text[text_len] = 0;

    // Channel index isn't stable across devices — resolve by the on-air
    // hash byte instead (see msn_relay.h's wire-format comment).
    int channel_idx = -1;
    int n = mesh_manager_channel_count();
    for (int i = 0; i < n; i++) {
        uint8_t h;
        if (mesh_manager_channel_hash(i, &h) && h == ch_hash) { channel_idx = i; break; }
    }
    if (channel_idx < 0) return false;   // unknown channel on this device

    return mesh_manager_send_text(to_node_id, channel_idx, text);
}

void msn_relay_register(void) {
    proximity_rpc_register(MSN_RELAY_ACTION_SEND_TEXT, handle_send_text);
    proximity_rpc_register(MSN_RELAY_ACTION_LIST_CONTACTS, handle_list_contacts);
    proximity_rpc_register(MSN_RELAY_ACTION_LIST_CHANNELS, handle_list_channels);
    proximity_rpc_register(MSN_RELAY_ACTION_GET_HISTORY, handle_get_history);
}

// ── Module lifecycle ──────────────────────────────────────────────────────

static int module_init(void) {
    msn_relay_register();
    mesh_manager_add_rx_callback(on_mesh_rx_for_history);
    return 0;
}

// ── Module header ─────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(msn_relay) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "msn_relay",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = module_init,
    .deinit            = NULL,
};
