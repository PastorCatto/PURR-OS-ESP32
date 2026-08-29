#pragma once
// server_mgr.h — PURR OS Server Manager wire protocol.
//
// A "settings surface" for a connected server, reachable from a client
// only via the synthetic "Server Manager" entry app_manager.h's remote
// mode injects for a server admin (see that header's own doc comment) —
// milkbar itself is untouched by this; this is a second, separate app
// (source/apps/system/server_manager/) that owns settings/transfer/
// approval, same "milkbar stays a connection surface, settings live
// elsewhere" split pairing.h's own remote-login work already established
// relative to milkbar.
//
// Responder AND caller live together in server_mgr.c, same shape as
// pairing_module.c (USERAUTH_*)/app_manager_remote.c (REMOTEAPPS_*) —
// both sides of one wire protocol in one module, not split across two.
// Always-on (registered from module init, not scoped to any app's
// lifetime) — a server answers these whether or not anything client-side
// is currently open, same reasoning app_manager_remote.c's own header
// comment gives for REMOTEAPPS_*.
//
// Gated into purrstrap.py's apply_radio_companion_defaults() auto-added
// set (server = true / radio.wifi = true), same family as proximity/
// pairing/proximity_rpc/app_manager_remote/homebase — no new purrstrap
// flag of its own.
//
// ── WiFi ──────────────────────────────────────────────────────────────────
// Calls into wifi_mgr.h ONLY if that module is actually loaded on this
// device (purr_kernel_get_module("wifi_mgr") != NULL) — server_mgr's own
// CMakeLists.txt REQUIRES wifi_mgr unconditionally (same "linkable
// everywhere this module is, initialized only where device.pcat also
// lists it" shape user_mgr/homebase already have on some devices), but a
// server that never lists wifi_mgr in its own [modules] section (Heltec,
// for now — see this header's own doc comment further down on why) simply
// answers every WiFi action as unsupported, never touching wifi_mgr's own
// state. This is a deliberate, DOCUMENTED gap, not an oversight: Heltec's
// proximity_module.c brings up the WiFi radio itself, lazily, in STA mode,
// specifically because nothing else on that device needed it before now
// (see heltec/device.pcat's own comment) — wifi_mgr_init() hard-assumes
// esp_wifi_init()/esp_netif_init()/esp_wifi_set_mode() already ran (see
// wifi_mgr.c's own header comment: true on tdeck_plus-class devices via
// kernel_tdp_boot.c, NOT true on Heltec's generic boot.c path), and racing
// wifi_mgr_init() against proximity's own on-demand bring-up is a real
// initialization-ordering hazard that needs its own dedicated pass to
// resolve correctly, not a fix bolted onto this one. Every OTHER
// server_mgr feature (app transfer/approval) works on Heltec today
// regardless — only the WiFi actions report unsupported there until that
// follow-up lands.
//
// ── App transfer ─────────────────────────────────────────────────────────
// Chunked (PROXIMITY_RPC_MAX_MSG caps a single proximity_rpc_call() at
// 2048 bytes — see proximity_rpc.h — nowhere near enough for a whole .claw
// binary in one call), landing in claw_loader.h's personal-space storage
// (SD if available, else /flash/personal — see that header's own doc
// comment on the flash fallback this session's work added). A pushed file
// is NOT immediately visible/scanned: it stays in <personal_root>/pending/
// until a human on THIS device approves it (server_mgr_get_pending_app()/
// _approve_app()/_reject_app() below — Heltec's own consumer is a new
// oled_ui_module.c screen, same hold=approve/tap=reject shape as that
// file's SCREEN_LOGIN_REQ). Approving moves it into the real, scanned
// <personal_root>/<username>/ location and calls app_manager_scan() so
// it's immediately visible over REMOTEAPPS_ACTION_LIST (app_manager_
// remote.h) to any other connected client — unchanged, existing mechanism,
// nothing new needed there. See the "Server Manager" plan doc's own
// "Files served, not run locally" section for what "served to clients"
// does and doesn't mean here.
//
// Every action below requires pairing_is_trusted(mac) — same baseline
// every remote-login/remoteapps action already uses (pairing.h's own top
// comment: "every higher-level feature built on this trust list must
// re-check pairing_is_trusted() on every inbound frame itself"). ACCEPTED
// SIMPLIFICATION, stated plainly: this does not re-verify the CALLING
// client's own admin status per call the way userauth's challenge-
// response does — pass-1 access control is "only an admin's client ever
// shows the Server Manager entry point at all" (client-side gating, see
// app_manager.h), not a per-request server-side re-proof. Threading a
// real challenge-response proof through every server_mgr action (mirroring
// pairing_verify_user()'s own Phase C) is real, valuable hardening,
// explicitly deferred, not silently pretended to be solved here.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int  server_mgr_init(void);
void server_mgr_deinit(void);

// ── WiFi — client (caller) side ─────────────────────────────────────────
// Both are blocking proximity_rpc_call()s — same "background task only,
// never cupcake_task" rule as every other function of this shape in this
// codebase. server_manager (the app) is the intended caller.

typedef enum {
    SRVMGR_WIFI_UNSUPPORTED = 0,   // no wifi_mgr on that device at all
    SRVMGR_WIFI_IDLE        = 1,
    SRVMGR_WIFI_CONNECTING  = 2,
    SRVMGR_WIFI_CONNECTED   = 3,
    SRVMGR_WIFI_FAILED      = 4,
} server_mgr_wifi_status_t;

// out_ssid/out_ip may be "" (never NULL-terminated garbage) if not
// connected or unsupported. False only on an RPC-level failure
// (unreachable/timeout) — a real "unsupported" answer from a reachable
// peer still returns true with SRVMGR_WIFI_UNSUPPORTED.
bool server_mgr_wifi_status(const uint8_t mac[6], server_mgr_wifi_status_t *out_status,
                             char *out_ssid, size_t ssid_sz, char *out_ip, size_t ip_sz);

// password may be NULL/"" for an open network. Encrypted (AES-256-GCM
// under the Phase A pairing secret — pairing.h) before it ever goes over
// the air, same protection pairing_request_user_access()'s own password
// hash already gets. False if the target has no wifi_mgr at all, or on
// an RPC-level failure.
bool server_mgr_wifi_set(const uint8_t mac[6], const char *ssid, const char *password);

// ── App transfer — client (caller) side ─────────────────────────────────
// One call does the whole begin/chunk-loop/end sequence — the transfer
// itself has no human-approval wait built in (that happens locally, later,
// on the server — this function only proves the BYTES arrived intact).
// Blocking (a multi-second network operation for anything but a tiny app);
// same "own background task" rule as everything else in this header.
bool server_mgr_app_upload(const uint8_t mac[6], const char *name,
                            const uint8_t *data, size_t len);

// ── App approval — server (local) side ──────────────────────────────────
// For whichever UI a given server hosts its own approval surface in
// (Heltec: a new oled_ui_module.c screen; a bigger-screen server: settings
// or a dedicated section — same API either way, see this header's own top
// comment). Mirrors pairing.h's pairing_get_pending_user_request()/
// _confirm_user_access()/_reject_user_access() shape exactly.
bool server_mgr_get_pending_app(char *out_name, size_t name_sz,
                                 char *out_device_name, size_t device_name_sz);
void server_mgr_approve_app(void);   // moves pending -> the real scanned personal dir, calls app_manager_scan()
void server_mgr_reject_app(void);    // discards the pending file

#ifdef __cplusplus
}
#endif
