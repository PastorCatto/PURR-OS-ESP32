#pragma once
// msn_backend.h — protocol-agnostic vtable MSN calls through, so its UI
// code (window layout, chat-log append/scroll/render, Add Room, Manage)
// never touches meshtastic.h or meshcore_api.h directly. Two
// implementations: msn_backend_meshtastic.c and msn_backend_meshcore.c —
// see each for how it maps its protocol's actual API onto this shape.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MSN_LAST_SEEN_UNKNOWN 0xFFFFFFFFu

// Sentinels for the three fields below — MeshCore's protocol doesn't carry
// any of them (mc_contact_info_t has no rssi/hop/telemetry fields at all,
// confirmed by reading meshcore_api.h), so its backend always reports
// "unknown" via these; meshtastic's backend reports real values whenever
// mesh_node_info_t has them.
#define MSN_RSSI_UNKNOWN     (-128)   // out of any real dBm range (~-30 to -120)
#define MSN_HOPS_UNKNOWN     (-1)
#define MSN_BATTERY_UNKNOWN  (-1)

typedef struct {
    char     id_str[24];   // stable string key (SD history path component) —
                            // hex node id (meshtastic) or hex pubkey prefix (meshcore)
    char     name[40];     // raw display name, no freshness annotation
    int      channel_idx;  // meshtastic: last-heard channel (used to pick DM key).
                            // meshcore: unused, always -1 (per-contact ECDH, not channel-keyed).
    uint32_t last_seen_ms_ago;  // MSN_LAST_SEEN_UNKNOWN if never heard — each
                                 // backend computes this from its own clock
                                 // (uptime-ms for meshtastic, RTC-seconds*1000
                                 // for meshcore) so msn.c can format an
                                 // "(online)"/"(Nm ago)" suffix generically
                                 // without knowing either clock's semantics.
    int8_t   rssi_dbm;     // MSN_RSSI_UNKNOWN if not available.
    int      hops_away;    // MSN_HOPS_UNKNOWN if not available (see
                            // mesh_router.h's doc comment on how meshtastic
                            // approximates this — it's not a real wire field).
    int8_t   battery_pct;  // 0-100, MSN_BATTERY_UNKNOWN if not available.
} msn_contact_t;

// channel_idx == -1 means the message was a direct message (from/to a contact),
// not a channel/room message.
typedef void (*msn_rx_cb_t)(int contact_idx, int channel_idx, const char *text);

typedef struct {
    const char *name;   // "Meshtastic" / "MeshCore" — for the chooser/status line

    bool (*ready)(void);
    bool (*is_alive)(void);

    int  (*contact_count)(void);
    bool (*contact_at)(int idx, msn_contact_t *out);
    void (*contact_forget)(int idx);

    int  (*channel_count)(void);
    bool (*channel_name)(int idx, char *out, size_t max);
    int  (*channel_add)(const char *name, const uint8_t *psk, size_t psk_len);
    void (*channel_remove)(int idx);
    // Stable on-air identity byte for a channel, independent of table index
    // (which shifts when another channel is removed) — used as the SD
    // history filename key, mirrors msn_contact_t.id_str's purpose.
    bool (*channel_hash)(int idx, uint8_t *hash_out);

    // to_contact_idx >= 0 sends a DM to that contact (channel_idx ignored).
    // Otherwise sends a channel/group text on channel_idx.
    bool (*send_text)(int to_contact_idx, int channel_idx, const char *text);

    void (*add_rx_cb)(msn_rx_cb_t cb);
    void (*remove_rx_cb)(msn_rx_cb_t cb);
} msn_backend_t;

const msn_backend_t *msn_backend_meshtastic(void);  // msn_backend_meshtastic.c
const msn_backend_t *msn_backend_meshcore(void);    // msn_backend_meshcore.c

// ── Home-base relay status/control (msn_backend_meshtastic.c only, this
// pass — see homebase.h and the home-base relay plan) ──────────────────────
// backend_send_text() in msn_backend_meshtastic.c automatically relays
// through the paired home base (when in range) instead of sending on the
// local radio; these let msn.c's chooser UI show that state and offer a
// manual "Force local radio" override. Free functions rather than vtable
// entries because this is scoped to the Meshtastic backend only this pass
// (see the plan) — MeshCore relay would extend msn_backend_meshcore.c and
// this same small API later, not the vtable itself.
bool msn_mt_relay_is_active(void);         // true if the NEXT send would go through the home base right now
void msn_mt_relay_set_force_local(bool force);
bool msn_mt_relay_get_force_local(void);

#ifdef __cplusplus
}
#endif
