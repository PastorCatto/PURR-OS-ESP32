#pragma once
// msn_relay.h — wire protocol shared between the home-base-side responder
// (msn_relay.c) and MSN's sender-side auto-routing
// (source/apps/system/msn/msn_backend_meshtastic.c), via proximity_rpc_
// call(). See msn_relay.c's own top comment for why this is a separate
// module from msn.c.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MSN_RELAY_ACTION_SEND_TEXT 0x2000

// Longest text payload this relay will carry — comfortably above any
// realistic Meshtastic TEXT_MESSAGE_APP payload (the mesh itself caps
// packets well under this), well inside proximity_rpc's own
// PROXIMITY_RPC_MAX_MSG=2048 budget once the 5-byte header is added.
#define MSN_RELAY_MAX_TEXT 512

// Raw wire format for MSN_RELAY_ACTION_SEND_TEXT requests — no fixed
// struct (mirrors app_manager_remote.h's bare-name convention for LAUNCH/
// STOP): a request buffer is
//   [uint32_t to_node_id (memcpy, not aligned)][uint8_t channel_hash][text bytes, NOT null-terminated]
// with req_len == MSN_RELAY_HEADER_LEN + strlen(text). to_node_id is
// MESH_BROADCAST (meshtastic.h) or a specific node id, and channel_hash is
// mesh_manager_channel_hash()'s single on-air byte — resolved to a LOCAL
// channel_idx by the responder, since channel index isn't guaranteed to
// match between two devices, only the hash is.
#define MSN_RELAY_HEADER_LEN 5

// ── Fresh-slate remote view (Phase D2) ──────────────────────────────────────
// LIST_CONTACTS/LIST_CHANNELS/GET_HISTORY let a paired caller see the home
// base's own mesh state instead of its own — see the home-base relay plan's
// "Phase D2" section. Contacts/channels are live pass-throughs of the home
// base's own mesh_manager_node_at()/channel_*() (no new storage); history is
// served from msn_relay.c's own bounded ring buffer, since nothing below
// meshtastic_module.c retains message text anywhere (confirmed — only
// telemetry is durable).

#define MSN_RELAY_ACTION_LIST_CONTACTS 0x2001
#define MSN_RELAY_ACTION_LIST_CHANNELS 0x2002
#define MSN_RELAY_ACTION_GET_HISTORY   0x2003

#define MSN_RELAY_MAX_CONTACTS 16   // matches msn.c's own MAX_CHATS
#define MSN_RELAY_MAX_CHANNELS 8    // matches MESH_MAX_CHANNELS / msn.c's MAX_ROOMS

#define MSN_LAST_SEEN_UNKNOWN_WIRE 0xFFFFFFFFu   // mirrors msn_backend.h's MSN_LAST_SEEN_UNKNOWN

// LIST_CONTACTS response: array of these. channel_hash (not a local
// channel_idx) so a DM sent back through this contact addresses the right
// channel regardless of the caller's own local channel table — same
// cross-device-stable-identity reasoning as SEND_TEXT's own wire format.
// last_seen_ms_ago is computed by the RESPONDER using its own clock (the
// two devices' uptimes aren't comparable), matching msn_contact_t's own
// field semantics so the caller can pass it straight through.
typedef struct __attribute__((packed)) {
    uint32_t node_id;
    char     name[40];
    uint8_t  channel_hash;
    int8_t   rssi_dbm;
    int32_t  hops_away;
    int8_t   battery_pct;
    uint32_t last_seen_ms_ago;
} msn_relay_contact_t;

// LIST_CHANNELS response: array of these.
typedef struct __attribute__((packed)) {
    char    name[12];   // matches msn.c's own s_room_names[][12] sizing
    uint8_t hash;
} msn_relay_channel_t;

// GET_HISTORY request: a bare little-endian uint32_t since_seq (req_len==4)
// — the caller's last-seen entry, 0 to fetch from the start of whatever the
// ring buffer currently holds. Response: an array of these, oldest first,
// capped per response (see msn_relay.c) to stay within proximity_rpc's
// PROXIMITY_RPC_MAX_MSG budget — a caller behind by more than one response's
// worth just needs another poll round, same "call again" shape as any other
// paginated RPC in this codebase. to_node is MESH_BROADCAST (meshtastic.h)
// for a channel/room message (channel_hash valid) or a specific node id for
// a DM addressed to the home base itself (channel_hash unused).
typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint32_t from_node;
    uint32_t to_node;
    uint8_t  channel_hash;
    char     text[200];
} msn_relay_history_entry_t;

// Called once from this module's init() to register the responder side.
// A no-op from the caller's perspective if proximity_rpc never actually
// receives a request — this just makes the device answer if one arrives
// from a trusted peer.
void msn_relay_register(void);

#ifdef __cplusplus
}
#endif
