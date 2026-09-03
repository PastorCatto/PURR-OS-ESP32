#pragma once
// app_manager_remote.h — Remote Apps (Milkbar) protocol shared between the
// responder (app_manager_remote.c, runs on the device being controlled)
// and any caller (Milkbar's UI, runs on the controlling device) via
// proximity_rpc_call(). See app_manager_remote.c's own top comment.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REMOTEAPPS_ACTION_LIST            0x1000
// req: app name -> resp: 1 status byte (REMOTEAPPS_LAUNCH_STATUS_*), or
// 0-length from an older responder that predates this field (proximity_
// rpc_call() itself already reports the plain success/fail as its own
// bool return regardless — this byte is ADDITIONAL detail on top of
// that, a caller that only checks the bool return loses nothing new).
#define REMOTEAPPS_ACTION_LAUNCH          0x1001
#define REMOTEAPPS_ACTION_STOP            0x1002

// A launch can genuinely succeed with nothing visible to the CALLER —
// the app now runs on the responder's own screen (OK_DISPLAY, the
// common case), or the responder has no screen of its own at all
// (OK_HEADLESS, e.g. Heltec) and there was never going to be anything
// to see anywhere. Distinguishing the two is the whole fix: previously
// both looked identical to a caller (nothing happened client-side either
// way), which read as "the app isn't installed" when it was actually
// "it launched, just not somewhere you can see it."
#define REMOTEAPPS_LAUNCH_STATUS_FAILED      0
#define REMOTEAPPS_LAUNCH_STATUS_OK_DISPLAY  1
#define REMOTEAPPS_LAUNCH_STATUS_OK_HEADLESS 2
// req: app name (bare bytes, like LAUNCH/STOP) -> resp: remote_download_
// info_t, 0-length if the app doesn't exist or isn't downloadable (a
// pre-linked or personal-space app has no plain fopen()-able file behind
// it — see handle_download_info()'s own comment in app_manager_remote.c).
#define REMOTEAPPS_ACTION_DOWNLOAD_INFO   0x1003
// req: remote_download_chunk_req_t -> resp: up to `want` raw bytes,
// 0-length on any failure (not found, not downloadable, seek/read
// error). Deliberately stateless/pull-style on the responder — unlike
// server_mgr.c's own chunked UPLOAD (where the responder incrementally
// builds a file that doesn't exist yet, so it must hold an open FILE*
// and in-flight state across calls), a DOWNLOAD's source is a complete,
// already-on-disk file: each chunk request independently opens, seeks,
// reads, and closes, with nothing to track between calls and no
// per-transfer concurrency guard needed.
#define REMOTEAPPS_ACTION_DOWNLOAD_CHUNK  0x1004

#define REMOTEAPPS_DOWNLOAD_CHUNK_MAX 1900   // comfortably under PROXIMITY_RPC_MAX_MSG (2048, proximity_rpc.h), same margin server_mgr.c's own SRVMGR_UPLOAD_CHUNK_MAX uses
#define REMOTEAPPS_MAX_DOWNLOAD_SIZE  (2u * 1024u * 1024u)   // same sanity ceiling as server_mgr.c's SRVMGR_MAX_UPLOAD_SIZE

typedef struct __attribute__((packed)) {
    uint32_t size;
    uint8_t  tier;   // app_tier_t
} remote_download_info_t;

typedef struct __attribute__((packed)) {
    char     name[48];
    uint32_t offset;
    uint16_t want;   // requested length — responder caps to REMOTEAPPS_DOWNLOAD_CHUNK_MAX regardless
} remote_download_chunk_req_t;

// One entry per app in a LIST response. tier/state are app_tier_t/
// app_state_t's raw values (app_manager.h) — Milkbar interprets them the
// same way the local Cat Apps launcher does. placement is app_placement_t
// (app_manager.h) — REMOTE (0) for every app that predates this field.
// downloadable mirrors app_entry_t.downloadable (app_manager.h) — see
// its own doc comment; computed server-side by handle_list() the same
// way handle_download_info()/_chunk() already gate the real thing, so
// the two can never disagree.
typedef struct __attribute__((packed)) {
    char    name[48];
    uint8_t tier;
    uint8_t state;
    uint8_t placement;
    uint8_t downloadable;
} remote_app_entry_t;

// Called once from app_manager_init() to register this device as a Remote
// Apps responder. A no-op from the caller's perspective if proximity_rpc
// never actually receives a request — this just makes the device answer
// if one arrives from a trusted peer.
void app_manager_remote_register(void);

// ── Caller side ──────────────────────────────────────────────────────────
// Milkbar's "Desktop" button, in one call: turns THIS device's own
// app_manager_count()/get()/launch_idx()/stop() over to `mac`'s app list,
// using proximity_rpc_call()/REMOTEAPPS_ACTION_* as the transport —
// app_manager.c itself never links proximity_rpc (see app_manager.h's own
// doc comment on app_manager_set_remote() for why: this module is the
// provider app_manager_set_remote() takes, not something app_manager
// depends on directly).
//
// False if app_manager_set_remote() itself refuses (bad args, background
// task couldn't start) — does NOT mean the remote device is unreachable;
// that shows up later as remote_refresh_task()'s list() calls quietly
// failing (app_manager_count() just reads 0 until one succeeds), not as a
// return value here.
bool app_manager_remote_connect(const uint8_t mac[6]);

// Turns remote mode back off — app_manager_count()/get()/launch_idx()/
// stop() revert to this device's own local registry. A no-op if remote
// mode wasn't on (or was pointed elsewhere by a different caller entirely —
// app_manager only tracks ONE remote session at a time, see app_manager.h).
void app_manager_remote_disconnect(void);

// Downloads `name` from `mac` into THIS device's own personal-space
// storage under `username` (claw_loader_personal_add(), claw_loader.h) —
// blocking, same "own background task, never cupcake_task/LVGL" rule
// every proximity_rpc_call() site in this codebase already follows.
// False on any failure (not found/not downloadable per
// REMOTEAPPS_ACTION_DOWNLOAD_INFO, too large, a chunk request failed, no
// personal-space storage on this device, or claw_loader_personal_add()
// itself failed) — the app is not left partially written either way
// (buffered whole in RAM, only handed to claw_loader_personal_add() once
// every chunk has arrived intact). On success, the app is immediately a
// real APP_TIER_PERSONAL entry — launch it the same way any other
// personal app launches (app_manager_launch_path()/app_manager_scan()).
bool app_manager_remote_download(const uint8_t mac[6], const char *name, const char *username);

#ifdef __cplusplus
}
#endif
