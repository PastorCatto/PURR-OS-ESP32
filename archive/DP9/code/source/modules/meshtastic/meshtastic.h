#pragma once
// meshtastic.h — public API for PURR OS's Meshtastic-compatible mesh module.
//
// Self-contained PURR_MOD_SYSTEM module: owns radio preset config, AES
// encryption, packet encode/decode, flood routing, dedup, and the node
// table internally. Talks to the radio exclusively through
// purr_kernel_radio()'s catcall_radio_t — never touches a specific radio
// chip directly, so it works with any registered radio driver.
//
// Ported from PURR-OS-0.11/CoreOS/system/kernel/modules/purr_mesh.{cpp,h}.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Callback fired on every successfully decoded + decrypted incoming packet.
// portnum matches meshtastic_PortNum values (1=TEXT, 3=POSITION, 4=NODEINFO, ...).
// channel_idx is which known channel (mesh_manager_channel_name()) this
// packet arrived on. to_node is the packet's destination — MESH_BROADCAST
// for a room/channel message, or a specific node id (normally our own) for
// a direct message; callers that care about routing a message to the
// right room vs. the right DM window need this to tell the two apart.
typedef void (*mesh_rx_cb_t)(uint32_t from_node, uint32_t to_node, int channel_idx, int portnum,
                              const uint8_t *payload, size_t len);

// PURR_MOD_SYSTEM init()/deinit() — called by the kernel module loader.
// init() just creates the mesh task and returns immediately; the task
// itself waits for the radio catcall to be registered before doing anything.
int  mesh_manager_init(void);
void mesh_manager_deinit(void);

// Destination for mesh_manager_send_text() meaning "the shared channel
// everyone hears" rather than a specific node — part of the public API
// (unlike the rest of mesh_radio.h, which is internal to this module).
#define MESH_BROADCAST 0xFFFFFFFFUL

// Encrypts a TEXT_MESSAGE_APP packet and queues it for transmission. `to`
// is a destination node ID for a private direct message, or MESH_BROADCAST
// for the shared channel everyone hears. `channel_idx` selects which
// channel (room) to encode with — for a DM this should be the target
// node's own mesh_node_info_t.channel_idx (the channel it was last heard
// on), not necessarily the primary. Safe to call from any task, including
// directly from a UI button press — encoding happens on the caller's own
// task (cheap), but the actual radio transmit happens later, asynchronously,
// on the mesh module's own task (RadioLib's transmit() is a blocking call
// that can take hundreds of ms of real LoRa airtime, too slow to run inline
// on whatever thread called this). Returns false if the radio isn't ready
// yet, encoding failed, or the outgoing queue is full — true means
// "queued", not "confirmed transmitted".
bool mesh_manager_send_text(uint32_t to, int channel_idx, const char *text);

// ── Channels (rooms) ──────────────────────────────────────────────────────────
// Index 0 is always the fixed "LongFast" default channel — every device
// speaks it out of the box, matching real Meshtastic. Additional channels
// (up to MESH_MAX_CHANNELS, currently 8) are user-added rooms with their
// own name+PSK, persisted across reboots.
#define MESH_MAX_CHANNELS 8

int  mesh_manager_channel_count(void);

// Fills *name_out (NUL-terminated, truncated to name_max) for a known
// channel index. Returns false if idx is out of range.
bool mesh_manager_channel_name(int idx, char *name_out, size_t name_max);

// Adds a new channel (room) with a 16-byte PSK. Returns the new channel's
// index, or -1 if all slots are full or the computed on-air hash collides
// with an existing channel (two channels would be indistinguishable on
// decode — pick a different name or PSK).
int mesh_manager_add_channel(const char *name, const uint8_t psk16[16]);

// Removes a channel (room) — "Forget" in MeshChat's Manage screen. Refuses
// idx==0 (the fixed LongFast primary). No-op if idx is out of range.
void mesh_manager_remove_channel(int idx);

// A channel's on-air hash byte — stable identity for a channel independent
// of its table index (which can shift when another channel is removed).
// Use this, not idx, as a persistent key (e.g. a history file name).
// Returns false if idx is out of range.
bool mesh_manager_channel_hash(int idx, uint8_t *hash_out);

// Register/unregister a callback for every incoming decoded packet (any
// portnum) — up to MESH_MAX_RX_CB subscribers at once (e.g. MeshChat and a
// Bluetooth companion-app bridge can both observe RX independently).
#define MESH_MAX_RX_CB 4
void mesh_manager_add_rx_callback(mesh_rx_cb_t cb);
void mesh_manager_remove_rx_callback(mesh_rx_cb_t cb);

uint32_t mesh_manager_node_id(void);
int      mesh_manager_node_count(void);
bool     mesh_manager_ready(void);

// True if the mesh task's own loop has stamped its heartbeat within the
// last few seconds — registered with purr_kernel_health_register() so the
// kernel's shared watchdog can catch a hung/crashed mesh task, and read
// directly by the Services app to show Meshtastic's live status.
bool     mesh_manager_is_alive(void);

// A known mesh node's display name (once heard via NodeInfo) + signal info.
typedef struct {
    uint32_t id;
    char     long_name[40];   // "!%08lX"-formatted node ID if never heard
    char     short_name[8];
    int8_t   rssi;
    uint32_t last_ms;         // esp_timer_get_time()/1000 at last packet heard
    int      channel_idx;     // channel this node was last heard on — pass
                               // to mesh_manager_send_text() to DM them
    int      hops_away;       // approximated from the most recent packet's
                               // remaining hop_limit vs. this codebase's
                               // fixed MESH_HOP_LIMIT — not a real wire
                               // field, see mesh_router.h's doc comment.
    int8_t   battery_pct;     // 0-100 from the node's last TELEMETRY_APP
                               // DeviceMetrics, or -1 if never heard.
} mesh_node_info_t;

// Enumerate known nodes by index (0..mesh_manager_node_count()-1). Returns
// 0 and fills *out on success, -1 if idx is out of range.
int mesh_manager_node_at(int idx, mesh_node_info_t *out);

// Removes a node entirely — "Forget" in MeshChat's Manage screen. A future
// message from the same node id re-adds it as unknown/new again.
void mesh_manager_node_forget(uint32_t id);

#ifdef __cplusplus
}
#endif
