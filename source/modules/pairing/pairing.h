#pragma once
// pairing.h — public API for PURR OS device pairing over ESP-NOW.
//
// General-purpose "trust this specific nearby device" mechanism — first
// use case is pairing a full-UI device (e.g. T-Deck Plus) with a headless
// LoRa "radio companion" (e.g. Heltec V3) so MSN can relay its mesh
// traffic through the companion's radio (see the "Remote radio companion"
// plan), but nothing here is Heltec-specific: either side can be the
// initiator or the responder, and any device with proximity_module.c's
// ESP-NOW subsystem can use this.
//
// Rides on proximity_module.c's shared ESP-NOW dispatch (PROXIMITY_FRAME_
// PAIRING) rather than owning its own radio callback — see proximity.h's
// own header comment for why that has to be true (ESP-NOW has one global
// recv-callback slot).
//
// Multi-device trust list — up to PAIRING_MAX_DEVICES remembered pairings,
// persisted (see pairing_module.c's mesh_router.c-style NVS blob pattern).
// The negotiation itself (REQUEST/ACCEPT/REJECT handshake, PENDING_* states
// below) is still only ever one-at-a-time — you can't have two pairing
// requests in flight simultaneously — but a successfully confirmed pairing
// is now APPENDED to the trust list rather than overwriting a single slot.
// Security (encryption/authentication of the link) is explicitly out of
// scope this pass; the pairing code only confirms *which physical device*
// you're pairing with (protects against drive-by pairing with a stranger's
// device merely by being in range), not confidentiality or integrity of
// the resulting link — every higher-level feature built on this trust list
// (e.g. the Remote Apps RPC layer) must re-check pairing_is_trusted() on
// every inbound frame itself; this module doesn't gate anything but its
// own handshake traffic.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int  pairing_init(void);
void pairing_deinit(void);

typedef enum {
    PAIRING_STATE_NONE = 0,
    PAIRING_STATE_PENDING_OUTGOING,   // we initiated, waiting for their confirm
    PAIRING_STATE_PENDING_INCOMING,   // they initiated, waiting on our local confirm
    PAIRING_STATE_PAIRED,
} pairing_state_t;

pairing_state_t pairing_get_state(void);

#define PAIRING_MAX_DEVICES 8

typedef struct {
    uint8_t mac[6];
    char    name[20];
} paired_device_t;

// ── Initiator side ──────────────────────────────────────────────────────
// e.g. nearby_app.c on a press-and-hold of a device flagged
// PROXIMITY_CAP_RADIO_COMPANION. Fails (returns false) if a pairing is
// already active (PAIRED or PENDING_*) — unpair/cancel first.
bool pairing_start(const uint8_t mac[6], const char *peer_name);
// Cancels our own outgoing pending request (no-op if not PENDING_OUTGOING).
void pairing_cancel(void);

// ── Responder side ───────────────────────────────────────────────────────
// e.g. a new oled_ui screen on Heltec, or T-Deck Plus's own nearby_app.c —
// the mechanism is symmetric, either device can be asked to confirm an
// incoming request. Poll pairing_get_state() == PAIRING_STATE_PENDING_
// INCOMING, then use these to render a confirm screen.
bool pairing_get_pending_code(char *out, size_t out_len);        // e.g. "4821"
bool pairing_get_pending_peer_name(char *out, size_t out_len);
void pairing_confirm(void);   // local user accepted
void pairing_reject(void);    // local user declined

// ── Durable pairing state ───────────────────────────────────────────────
// Either side of an established pairing uses these — this is what msn.c's
// "Remote" backend (Phase C) checks before offering itself as a chooser
// option.
//
// pairing_is_paired()/pairing_get_paired_mac()/pairing_get_paired_name()
// operate on trust-list index 0 (the first/oldest remembered device) —
// kept for the two existing single-device callers (nearby_app.c,
// oled_ui_module.c), neither of which needs to distinguish between
// multiple paired devices. New code that needs the full list uses the
// indexed API below instead.
bool pairing_is_paired(void);
bool pairing_get_paired_mac(uint8_t out_mac[6]);
bool pairing_get_paired_name(char *out, size_t out_len);
// Clears trust-list index 0 (the first/oldest paired device) and
// (best-effort) notifies the peer. Safe to call regardless of current
// state. New code should prefer pairing_forget(mac) instead, which is
// unambiguous about which device.
void pairing_unpair(void);

// ── Multi-device trust list ─────────────────────────────────────────────
int  pairing_device_count(void);
// Fills *out for a trust-list index. Returns false if idx is out of range.
bool pairing_device_at(int idx, paired_device_t *out);
// True if mac is in the trust list — the authorization check every
// higher-level feature (Remote Apps RPC, etc.) must apply to inbound
// frames itself; see this header's own top comment.
bool pairing_is_trusted(const uint8_t mac[6]);
// Removes mac from the trust list (best-effort UNPAIR notice to the peer,
// same as pairing_unpair()) if present. No-op otherwise.
void pairing_forget(const uint8_t mac[6]);

// ── Home base ────────────────────────────────────────────────────────────
// At most one paired device can be designated "home base" — the target of
// MSN's automatic radio-relay handoff (see source/modules/homebase/). Kept
// as its own NVS key rather than a field on paired_device_t: growing that
// struct would change its NVS blob size with no format-version key to
// detect old blobs, silently dropping every existing pairing on upgrade
// (see pairing_module.c's load_paired()/save_paired()). Storing this
// separately avoids that risk entirely.
bool pairing_set_home_base(const uint8_t mac[6]);   // false if mac isn't in the trust list
bool pairing_get_home_base(uint8_t out_mac[6]);      // false if none set
bool pairing_is_home_base(const uint8_t mac[6]);
void pairing_clear_home_base(void);

// ── Remote login ─────────────────────────────────────────────────────────
// Logging in as a specific user_mgr account on a PAIRED device — separate
// from (and layered on top of) the device-level trust above. Two paths:
//
//   Phase B (rare — first time this device asks to act as `username` on a
//   given paired server): pairing_request_user_access() sends the
//   password (protected under the pairing.h Phase A shared secret, never
//   sent in reusable/replayable form — see pairing_module.c's own doc
//   comment for the exact construction) and returns whether it was
//   accepted; if so, a human on the SERVER must then approve/deny via
//   pairing_get_pending_user_request()/pairing_confirm_user_access()/
//   pairing_reject_user_access() — poll pairing_poll_user_access() from
//   the CLIENT side until it reports something other than PENDING, then
//   call pairing_register_user_key() to actually receive a credential.
//
//   Phase C (every later login as the same user on the same server):
//   pairing_verify_user() alone — proves possession of the key Phase B
//   registered via challenge-response (the key itself is never resent),
//   and on success does the user_mgr_create_remote()+set_logged_in()
//   bookkeeping itself. Returns false (with no side effects) if this
//   device never completed Phase B for that (mac, username) pair — the
//   caller's fallback is simply to start Phase B.
//
// A server-held key that goes unused for a long stretch is discarded
// (checked lazily, next time it would be used to answer a Phase C
// challenge) — see pairing_module.c's own USERAUTH_KEY_EXPIRY_S comment.
//
// Every function below is a blocking proximity_rpc_call() (one or more) —
// same "never call from cupcake_task or proximity_task" rule
// proximity_rpc.h's own top comment documents. Call these from a
// dedicated background task, same convention every existing
// proximity_rpc_call() site in this codebase already follows.

typedef enum {
    PAIRING_USERAUTH_NONE     = 0,   // no matching request on the server (never sent, denied, or expired)
    PAIRING_USERAUTH_PENDING  = 1,   // password accepted, waiting on the server's own human to approve/deny
    PAIRING_USERAUTH_APPROVED = 2,   // approved — call pairing_register_user_key() next
} pairing_user_access_status_t;

// ── Server side (this device owns the user_mgr account being requested) ──
// Poll pairing_get_pending_user_request() (e.g. from the same background
// task/timer a UI already uses to refresh other pairing state), then
// render a confirm screen and call one of the two below.
bool pairing_get_pending_user_request(char *out_username, size_t username_sz,
                                       char *out_device_name, size_t device_name_sz);
void pairing_confirm_user_access(void);   // local human approved
void pairing_reject_user_access(void);    // local human declined

// ── Client side ────────────────────────────────────────────────────────
// Phase B, step 1: verify the password and (if correct) put the request in
// front of the server's human. Returns false immediately for a wrong
// password OR if another request is already pending anywhere on the
// server — true means "wait and poll", not "approved".
bool pairing_request_user_access(const uint8_t mac[6], const char *username, const char *password);
// Phase B, step 2: poll until this stops returning PENDING.
pairing_user_access_status_t pairing_poll_user_access(const uint8_t mac[6], const char *username);
// Phase B, step 3: once APPROVED, actually receive a credential. Persists
// it locally (this device's own copy, for pairing_verify_user() below) —
// nothing further needed after this returns true.
bool pairing_register_user_key(const uint8_t mac[6], const char *username);

// Phase C: the ordinary "log in again" path. False if this device never
// completed Phase B for (mac, username) — caller should fall back to
// pairing_request_user_access(). On true, this device is now
// user_mgr_current_user() == username (user_mgr_set_logged_in() already
// called).
bool pairing_verify_user(const uint8_t mac[6], const char *username);

// ── Forced logout (server -> client) ─────────────────────────────────────
// The one action in this file that runs in reverse — a SERVER calls these
// to tell an already-trusted CLIENT "you're being disconnected", rather
// than a client asking a server for something. Real use: freeing this
// device's own pairing/proximity/WiFi memory for something mutually
// exclusive with acting as a login server (RNode mode — source/modules/
// rnode/rnode_module.c) needs any currently-connected client told first,
// while proximity_rpc itself is still up to carry the call. On the
// receiving device, this fires user_mgr_logout() and purr_kernel_notify_
// remote_logout() (purr_kernel.h) — whichever app owns the visible
// "connected" UI (milkbar, today) reacts via purr_kernel_set_remote_
// logout_cb(), never a direct call into this file.
//
// False only on an RPC-level failure (unreachable/timeout/untrusted) —
// same "best-effort" contract every other caller-side function here has.
bool pairing_force_logout(const uint8_t mac[6]);
// Best-effort broadcast to every device in this file's own trust list —
// see pairing_force_logout_all()'s own implementation comment
// (pairing_module.c) for why "every trusted device" rather than "every
// currently-connected device" (this codebase has no live session tracking
// today) is the right, safe scope for this.
void pairing_force_logout_all(void);

// ── Remote OOBE (first-run setup, pushed from a client) ─────────────────
// For a paired device with no screen/keyboard of its own to run the local
// oobe app (source/apps/system/oobe/oobe_app.c) — e.g. Heltec's oled_ui —
// first-run setup is instead PUSHED here from an already-trusted client,
// once device pairing (above) has already succeeded. Gated on
// pairing_is_trusted() (this device must already trust the peer — same
// rule every higher-level feature built on the trust list applies to its
// own inbound frames, per this header's own top comment) AND
// !user_mgr_oobe_completed() on the RESPONDER side — once a device's own
// OOBE is complete, a push here always fails, PERMANENTLY: this is a
// first-setup mechanism, not a "remotely reconfigure the admin account
// later" one. Redoing setup later is a local, deliberate action only,
// same as the local oobe app itself only auto-launches once (a manual
// re-launch, e.g. from Terminal, still works locally — there is no remote
// equivalent).
//
// Both are blocking proximity_rpc_call()s — same "never call from
// cupcake_task or proximity_task" rule as every other function in this
// header.

// False on ANY failure — unreachable peer, not paired, or genuinely
// already configured — "don't offer it" is the safe default for a caller
// like milkbar's Nearby section deciding whether to show a Setup option.
bool pairing_remote_oobe_needed(const uint8_t mac[6]);

// username=="" (or NULL) means "keep the peer's own bootstrap default
// account, no password" — the exact same zero-friction path oobe_app.c's
// own on_skip() takes locally, just pushed remotely. password may be NULL/
// "" for no password either way. Both strings are AES-256-GCM-encrypted
// under the Phase A pairing secret before going over the air — a fresh
// admin password deserves the same protection pairing_request_user_
// access()'s own password hash already gets, not less.
bool pairing_remote_oobe_push(const uint8_t mac[6], const char *username, const char *password);

// ── Remote user creation (ongoing, admin action — distinct from OOBE) ────
// Unlike Remote OOBE above, this is NOT a one-time mechanism: it creates
// an ADDITIONAL named LOCAL account on the responder, any number of times,
// for as long as USER_MGR_MAX_USERS allows — "give someone else their own
// login on my server" after first-run setup is already done, not "set the
// server up for the first time" (that's still Remote OOBE's job; this
// function makes no attempt to touch the bootstrap account). Gated the
// same way every other ongoing server-management action already is
// (server_mgr.h's own WiFi-set/app-push handlers): pairing_is_trusted(mac)
// alone, deliberately NOT user_mgr_oobe_completed() — a paired device is
// already trusted to administer the server at that level, and this action
// is meant to keep working indefinitely, not close itself off after one
// use the way Remote OOBE intentionally does.
typedef enum {
    PAIRING_USER_CREATE_FAIL = 0,        // untrusted peer, unreachable, or RPC failure
    PAIRING_USER_CREATE_OK,
    PAIRING_USER_CREATE_ALREADY_EXISTS,  // a genuine name collision — not OOBE's blanket "already set up"
    PAIRING_USER_CREATE_INVALID_NAME,
    PAIRING_USER_CREATE_SERVER_FULL,     // USER_MGR_MAX_USERS reached
} pairing_user_create_status_t;

// Blocking proximity_rpc_call() — same "never call from cupcake_task or
// proximity_task" rule as every other function in this header. `password`
// may be NULL/"" for no password (mirrors user_mgr_create()'s own
// contract — user_mgr_verify()'s "no password = auto-login identity" rule
// then applies to this new account the same as any other). Both strings
// are AES-256-GCM-encrypted under the Phase A pairing secret before going
// over the air, same protection Remote OOBE's own payload gets.
pairing_user_create_status_t pairing_remote_user_create(const uint8_t mac[6], const char *username, const char *password);

// ── Shared secret (for other trusted-peer wire protocols) ────────────────
// The same Phase A ECDH shared secret this file's own USERAUTH/OOBE
// messages are encrypted under, for a module built ON TOP of this trust
// list that needs to protect its OWN payloads the same way (e.g.
// server_mgr.h's WiFi-credential push) rather than either sending them in
// the clear or standing up a second handshake. Derive a purpose-specific
// subkey from it per message (SHA256(secret || nonce || a distinct label
// string) — see pairing_module.c's own derive_msg_key() for the exact
// construction to mirror, with a DIFFERENT label than this file's own
// "purr_pairing_access", for domain separation between the two
// protocols sharing one secret) — never use the raw secret directly as
// an AES key. False if mac isn't a trusted peer.
bool pairing_get_shared_secret(const uint8_t mac[6], uint8_t out_secret[32]);

#ifdef __cplusplus
}
#endif
