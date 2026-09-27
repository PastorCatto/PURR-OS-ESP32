#pragma once
// proximity.h — public API for PURR OS's shared ESP-NOW subsystem.
//
// Started as beacon-only "which other PurrOS devices are physically near
// me right now" discovery — independent of the LoRa mesh (Meshtastic/
// MeshCore), works whether either is active or not. Now also the single
// owner of ESP-NOW's one global recv-callback slot for the whole OS: since
// esp_now_register_recv_cb() cannot have multiple subscribers, every other
// PurrOS ESP-NOW frame type (pairing handshake, remote-radio-companion
// relay RPC — see the "Remote radio companion" plan) rides on this module's
// dispatch via proximity_register_handler() instead of registering its own
// callback. Beacon discovery itself is unchanged by this: unencrypted
// broadcast beacons (PMK/LMK encryption deferred, per the original scoping
// note), and the known-devices table is deliberately NOT persisted across
// reboots — "nearby right now" is inherently live; a stale entry surviving
// a reboot would be actively misleading, not just harmless staleness
// (unlike a long-term mesh contact list, or the durable pairing state that
// will live elsewhere).

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int  proximity_init(void);
void proximity_deinit(void);

bool proximity_ready(void);
bool proximity_is_alive(void);

#define PROXIMITY_MAX_DEVICES 16

// Beacon capability flags — advertises what a device offers beyond plain
// discovery. CAP_RADIO_COMPANION: has a LoRa radio and little/no UI of its
// own, and offers to be paired with + have its radio relayed to (see the
// "Remote radio companion" plan) — set via proximity_set_own_caps() on
// devices like Heltec V3 that want to advertise this.
#define PROXIMITY_CAP_RADIO_COMPANION (1u << 0)

// CAP_HAS_DISPLAY: this device has a real display driver registered (not
// necessarily purr_win-capable — oled_ui counts too) — set at boot
// wherever purr_kernel_display() (or the equivalent host-specific check)
// comes back non-NULL. CAP_STRONG_COMPUTE: this device has PSRAM
// (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) — the same signal
// this codebase already uses elsewhere (app_manager.c's own task-stack
// allocator, proximity_module.c's own PSRAM-vs-internal-RAM task-stack
// choice) as its "meaningfully stronger board" heuristic. Both feed
// app_manager.c's decide_placement() for a HYBRID-placed app — see its
// own doc comment for the actual rule.
#define PROXIMITY_CAP_HAS_DISPLAY     (1u << 1)
#define PROXIMITY_CAP_STRONG_COMPUTE  (1u << 2)

typedef struct {
    uint8_t  mac[6];
    char     name[20];        // matches the beacon payload's own name field size
    uint8_t  caps;            // PROXIMITY_CAP_* bitflags from the peer's beacon
    int8_t   rssi;
    uint32_t first_seen_ms;   // purr_kernel_uptime_ms() at first beacon heard
    uint32_t last_seen_ms;    // purr_kernel_uptime_ms() at most recent beacon heard
} proximity_device_t;

int  proximity_device_count(void);
// Fills *out for a known device index. Returns false if idx is out of range.
bool proximity_device_at(int idx, proximity_device_t *out);

// Same, keyed by mac instead of table index — false if `mac` hasn't sent
// a beacon within STALE_MS (proximity_module.c), same pruning
// proximity_device_at()'s own indexed enumeration is already subject to.
// A caller that needs a currently-connected remote peer's caps (e.g.
// app_manager_decide_placement()'s own caller) and gets false back
// should treat the peer as having none — safe/conservative, not an error
// to surface.
bool proximity_find_device_by_mac(const uint8_t mac[6], proximity_device_t *out);

// ORs `caps` into this device's own capability flags (accumulates, does
// NOT replace) — reflected in every beacon sent from here on (next
// BEACON_INTERVAL_MS tick — no immediate re-send). Accumulates rather
// than replaces specifically so multiple independent callers (oled_ui_
// module.c's own PROXIMITY_CAP_RADIO_COMPANION call, proximity_module.c's
// own HAS_DISPLAY/STRONG_COMPUTE detection at init time) can each set
// their own bit without needing to know about or coordinate with the
// others, regardless of init order.
void proximity_set_own_caps(uint8_t caps);

// This device's own current capability flags — for a caller comparing
// itself against a discovered peer's proximity_device_t.caps (e.g.
// app_manager.h's app_manager_decide_placement()). 0 before proximity_
// init() has run.
uint8_t proximity_get_own_caps(void);

// Copies this device's own beacon name into out (NUL-terminated, truncated
// to out_len). Safe any time after proximity_init() has run.
void proximity_get_own_name(char *out, size_t out_len);

// ── Shared ESP-NOW frame dispatch ───────────────────────────────────────
// Every frame this module sends/receives leads with a one-byte type —
// mirrors meshtastic's own portnum-switch, just one layer up, so multiple
// unrelated protocols can multiplex over ESP-NOW's single callback slot.
typedef enum {
    PROXIMITY_FRAME_BEACON  = 1,   // handled internally, not registrable
    PROXIMITY_FRAME_PAIRING = 2,
    PROXIMITY_FRAME_RELAY   = 3,
} proximity_frame_type_t;

// 250 bytes is ESP-NOW's hard per-packet cap in this ESP-IDF version; 1
// byte is reserved for the leading type byte.
#define PROXIMITY_MAX_PAYLOAD 249

// Invoked from proximity_task() (not ISR/recv-callback context) for every
// received frame of a registered type. payload/len exclude the leading
// type byte — callers only see their own frame body.
typedef void (*proximity_frame_handler_t)(const uint8_t *mac, int8_t rssi,
                                           const uint8_t *payload, size_t len);

// Registers the handler for a frame type. At most one handler per type; a
// second registration for the same type replaces the first. Only
// PROXIMITY_FRAME_PAIRING/_RELAY are meant to be registered externally —
// PROXIMITY_FRAME_BEACON is reserved for this module's own discovery logic.
void proximity_register_handler(proximity_frame_type_t type, proximity_frame_handler_t handler);

// Ensures a unicast ESP-NOW peer exists for mac (idempotent, safe to call
// even if it already does). proximity_send_unicast() also calls this
// itself, so most callers never need to call it directly.
bool proximity_add_peer(const uint8_t *mac);

// Sends a typed unicast frame to mac. payload_len must be <=
// PROXIMITY_MAX_PAYLOAD. Fire-and-forget like all of ESP-NOW — callers
// needing a reply/timeout (e.g. relay RPC) build that on top via their own
// registered handler.
bool proximity_send_unicast(const uint8_t *mac, proximity_frame_type_t type,
                             const uint8_t *payload, size_t payload_len);

#ifdef __cplusplus
}
#endif
