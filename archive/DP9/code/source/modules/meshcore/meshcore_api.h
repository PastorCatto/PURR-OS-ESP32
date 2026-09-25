#pragma once
// meshcore_api.h — public API for PURR OS's MeshCore-compatible mesh module.
// Named meshcore_api.h rather than meshcore.h: on a case-insensitive
// filesystem (Windows), a same-directory-tree "meshcore.h" collides with
// vendor/MeshCore.h at the compiler's include-search level (angle-bracket
// lookups can't tell them apart), silently resolving <MeshCore.h> to the
// wrong file. Confirmed live — this is not a style preference.
//
// Deliberately parallel in shape to meshtastic.h (same init/deinit,
// send_text, channel, rx-callback, alive/ready pattern) so a future
// protocol-agnostic messaging app (MSN) can integrate against both with
// minimal glue — even though the two wire protocols are unrelated and
// this module's addressing is contact/pubkey-based rather than
// Meshtastic's 32-bit node-id scheme.
//
// Self-contained PURR_MOD_SYSTEM module. Talks to the radio exclusively
// through purr_kernel_radio()'s catcall_radio_t via a custom mesh::Radio
// adapter (mc_radio_adapter.h) — never touches a specific radio chip
// directly. Mutually exclusive with meshtastic at runtime (only one
// physical radio, one catcall_radio_t slot) — see meshcore_module.cpp's
// startup guard.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Callback fired on every successfully decrypted incoming text message.
// from_contact_idx is an index into mc_manager_contact_at() (-1 if the
// sender isn't a known contact yet). channel_idx is a valid
// mc_manager_channel_*() index for a channel/group message, or -1 for a
// direct message.
typedef void (*mc_rx_cb_t)(int from_contact_idx, int channel_idx, const char *text);

// PURR_MOD_SYSTEM init()/deinit() — called by the kernel module loader.
int  mc_manager_init(void);
void mc_manager_deinit(void);

// Sends a text message. If to_contact_idx >= 0, sends a direct
// (ECDH-encrypted) message to that contact. Otherwise sends a group text
// message on channel_idx. Same async-queue semantics as meshtastic.h's
// mesh_manager_send_text(): encoding happens on the caller's own task,
// actual radio transmit happens later on the mesh task. Returns false if
// not ready / encode failed / packet pool exhausted.
bool mc_manager_send_text(int to_contact_idx, int channel_idx, const char *text);

// ── Channels ─────────────────────────────────────────────────────────────
#define MC_MAX_CHANNELS 8

int  mc_manager_channel_count(void);
bool mc_manager_channel_name(int idx, char *name_out, size_t name_max);

// Adds a group channel with a 16 or 32-byte pre-shared key. Returns the
// new channel's index, or -1 if all slots are full.
int  mc_manager_channel_add(const char *name, const uint8_t *psk, size_t psk_len);

// A channel's on-air hash byte (SHA-256(secret) truncated to 1 byte) —
// stable identity for a channel independent of its table index (which can
// shift when another channel is removed). Same shape/purpose as
// meshtastic.h's mesh_manager_channel_hash(). Returns false if idx is out
// of range.
bool mc_manager_channel_hash(int idx, uint8_t *hash_out);

// Removes a channel — "Forget" in MSN's Manage screen. No-op if idx is out
// of range.
void mc_manager_channel_remove(int idx);

// ── Contacts ─────────────────────────────────────────────────────────────
typedef struct {
    uint8_t  pub_key[32];
    char     name[32];
    bool     has_path;
    uint32_t last_advert_timestamp;
} mc_contact_info_t;

int  mc_manager_contact_count(void);
// Fills *out for a known contact index. Returns false if idx is out of range.
bool mc_manager_contact_at(int idx, mc_contact_info_t *out);

// Removes a contact — "Forget" in MSN's Manage screen. No-op if idx is out
// of range.
void mc_manager_contact_forget(int idx);

// ── Callbacks / status ───────────────────────────────────────────────────
#define MC_MAX_RX_CB 4
void mc_manager_add_rx_callback(mc_rx_cb_t cb);
void mc_manager_remove_rx_callback(mc_rx_cb_t cb);

bool mc_manager_ready(void);

// Current value of the same clock mc_contact_info_t.last_advert_timestamp
// is stamped from (PurrRTCClock — uptime-seconds plus an optional offset,
// see mc_radio_adapter.h; no real wall-clock source is wired in yet). Lets
// a caller compute "how long ago" without assuming that clock's internals.
uint32_t mc_manager_now_seconds(void);

// True if the mesh task's own loop has stamped its heartbeat within the
// last few seconds — registered with purr_kernel_health_register() so the
// kernel's shared watchdog can catch a hung/crashed mesh task, and read
// directly by the Services app to show MeshCore's live status.
bool mc_manager_is_alive(void);

#ifdef __cplusplus
}
#endif
