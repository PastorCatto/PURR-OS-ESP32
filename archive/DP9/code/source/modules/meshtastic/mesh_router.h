#pragma once
// mesh_router.h — packet encode/decode, flood routing, dedup, node table.
// Internal to the meshtastic module — not a public API, see meshtastic.h.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Must be called once (with this node's ID) before any other function here.
void mesh_router_init(uint32_t node_id);

// Encodes a TEXT_MESSAGE_APP packet, encrypted, ready to hand to
// catcall_radio_t.send(). `to` is a destination node ID, or MESH_BROADCAST
// (mesh_radio.h) for the shared channel. `channel_idx` selects which
// channel's PSK/hash to encode with (mesh_radio.h's channel table).
// Returns wire byte count, or 0 on failure.
size_t mesh_router_encode_text(uint8_t *wire, size_t wire_max, uint32_t to, int channel_idx, const char *text);

// Encodes a broadcast NODEINFO_APP packet (this node's User info).
size_t mesh_router_encode_nodeinfo(uint8_t *wire, size_t wire_max);

// Encodes an implicit-ACK ROUTING_APP packet (error_reason=NONE) addressed
// back to `to` on `channel_idx` (must be the same channel the original
// packet arrived on — the sender only knows how to decode with that
// channel's PSK), with `request_id` set to the original packet's id —
// this is what real Meshtastic firmware auto-sends whenever it receives a
// unicast packet with want_ack set, and what every real client (the phone
// app, another node's OLED) is waiting on before it'll show a message as
// delivered instead of erroring out. Returns wire byte count, or 0 on
// failure.
size_t mesh_router_encode_ack(uint8_t *wire, size_t wire_max, uint32_t to, int channel_idx, uint32_t request_id);

// Re-broadcasts a received raw packet with hop_limit decremented, after a
// random 0-500ms backoff. Does NOT decrypt/re-encrypt — the encrypted
// payload is preserved verbatim, only the plaintext header changes.
void mesh_router_relay(const uint8_t *raw, size_t raw_len);

// Decodes + decrypts raw LoRa bytes. Returns false on parse/crypto failure,
// including "the packet's channel-hash byte doesn't match any channel we
// know" — that's an expected, silent case (someone else's private room),
// not logged as a warning the way real decode failures are.
// *want_ack reflects the header's WANT_ACK flag bit — mesh_task() uses this
// to decide whether to send an implicit ACK back (see
// mesh_router_encode_ack()) once the packet is confirmed addressed to us.
// *channel_idx is which of our configured channels matched.
bool mesh_router_decode(const uint8_t *raw, size_t raw_len,
                         uint32_t *from, uint32_t *to, uint32_t *pkt_id,
                         uint8_t *hop_limit, bool *want_ack, int *channel_idx, int *portnum,
                         uint8_t *payload, size_t *payload_len, size_t payload_max);

// Dedup ring buffer (32 slots, keyed by from+packet_id).
bool mesh_router_dedup_seen(uint32_t from, uint32_t id);
void mesh_router_dedup_add(uint32_t from, uint32_t id);

// Node table (16 nodes, tracks RSSI, last-seen time, and — once a NodeInfo
// packet has been heard from that node — its display name).
typedef struct {
    uint32_t id;
    char     long_name[40];
    char     short_name[8];
    int8_t   rssi;
    uint32_t last_ms;
    int      channel_idx;      // fallback channel for DMing this node when
                                // PKI isn't available (see has_public_key)
    uint8_t  public_key[32];
    bool     has_public_key;   // once true, DMs to/from this node use real
                                // Meshtastic's PKI encryption, not channel_idx
    // hop_limit *remaining* on the most recent packet heard from this node
    // (mesh_router_decode()'s own *hop_limit output) — not persisted (reset
    // to MESH_HOP_LIMIT on load, same rationale as rssi/last_ms below).
    // hops-away = MESH_HOP_LIMIT - hop_limit_at_last_heard, an approximation
    // since every sender in this codebase always originates at the same
    // fixed MESH_HOP_LIMIT (mesh_radio.h) — there's no hop_start field on
    // the wire to read directly (see mesh_router.c's PacketHeader comment).
    uint8_t  hop_limit_at_last_heard;
    // 0-100 from a decoded TELEMETRY_APP DeviceMetrics.battery_level, or -1
    // if never heard. Not persisted — same staleness rationale as rssi.
    int8_t   battery_pct;
} mesh_node_t;

// channel_idx: which channel this packet was heard on (from
// mesh_router_decode()'s output) — updates the node's "home channel" for
// DMing, since a node heard on a private room's channel can't be reached
// on the primary channel's PSK. hop_limit: the packet's *remaining*
// hop_limit (mesh_router_decode()'s own output) — stored for the hops-away
// approximation (see mesh_node_t.hop_limit_at_last_heard above).
void mesh_router_node_touch(uint32_t id, int8_t rssi, int channel_idx, uint8_t hop_limit);

// Called when a decoded TELEMETRY_APP packet's DeviceMetrics carries
// has_battery_level — no-op if the node isn't tracked yet (same "touch()
// always runs first" precondition as node_set_name()/node_set_pubkey()).
void mesh_router_node_set_battery(uint32_t id, uint8_t pct);

// Called when a NODEINFO_APP packet is decoded (see mesh_task()'s RX loop in
// meshtastic_module.c) — fills in the display name for a node already
// present via mesh_router_node_touch(). No-op if the node isn't tracked yet.
void mesh_router_node_set_name(uint32_t id, const char *long_name, const char *short_name);

// Called alongside mesh_router_node_set_name() when a NodeInfo's User.public_key
// is present (32 bytes) — once set, this node becomes reachable via PKI.
// No-op if the node isn't tracked yet.
void mesh_router_node_set_pubkey(uint32_t id, const uint8_t pubkey[32]);

// Removes a node entirely (shift-down + re-save to NVS) — used by
// MeshChat's "Forget" action. No-op if the node isn't tracked.
void mesh_router_node_forget(uint32_t id);

int  mesh_router_node_count(void);

// Enumerate the node table by index (0..mesh_router_node_count()-1). Returns
// NULL out of range. The returned pointer is only valid until the next
// mesh_router_node_touch()/mesh_router_node_set_name() call.
const mesh_node_t *mesh_router_node_at(int idx);

// Loads the persisted node table from NVS (namespace "purr_mesh", keys
// "nodes"/"node_count" — same pattern and namespace as mesh_radio.c's
// channel table, just different keys). Only identity fields are restored
// (id/names/channel_idx/public_key) — rssi/last_ms start fresh, repopulated
// as soon as each node is heard again. MUST be called once from
// mesh_manager_init(), before mesh_task() is created — never from
// mesh_task() itself or anything it calls (PSRAM-stack-vs-NVS crash class,
// see mesh_radio_init()'s doc comment for the full story).
void mesh_router_load_nodes(void);

// Every node-table mutator above (_touch/_set_name/_set_pubkey/_forget) is
// reachable from a PSRAM-backed stack (mesh_task()'s RX loop, or MeshChat's
// own app task) and so never touches NVS directly — they just mark the
// table dirty. This performs the actual deferred NVS write, and MUST be
// called periodically from a task with an internal-RAM stack (see
// meshtastic_module.c's mesh_persist_task()). No-op if nothing changed
// since the last call.
void mesh_router_flush_nodes_if_dirty(void);
