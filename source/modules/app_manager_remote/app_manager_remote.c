// app_manager_remote.c — responder side of the Remote Apps (Milkbar)
// protocol: answers proximity_rpc requests from a paired device wanting to
// list/launch/stop THIS device's apps. Pure wrapper around app_manager.h's
// existing public API — no new local behavior, just a new caller.
//
// A separate module (not folded into app_manager.c itself) specifically so
// devices without WiFi/proximity_rpc (most non-radio-companion targets)
// never pull this in — app_manager.c is universal across every device,
// this isn't. Auto-added by purrstrap.py's apply_radio_companion_
// defaults() alongside proximity/pairing/proximity_rpc, same condition
// (radio.wifi=true). Both this module and proximity_rpc register at
// P3/OPTIONAL — same-tier load order between them isn't guaranteed, which
// is why proximity_rpc_register() is deliberately safe to call before
// proximity_rpc_init() has actually run (see that function's own comment).

#include "../app_manager/app_manager.h"
#include "app_manager_remote.h"
#include "../proximity_rpc/proximity_rpc.h"
#include "../proximity/proximity.h"
#include "../claw_loader/claw_loader.h"
#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_kernel.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

static const char *TAG = "app_manager_remote";

// remote_app_entry_t is deliberately NOT app_entry_t itself (that struct
// carries local-only fields like purr_win_t window and a 256-byte path no
// remote caller needs) — see app_manager_remote.h for the wire shape both
// this responder and Milkbar's caller side share.

// True for a path with nothing real behind it to fopen() — a pre-linked
// app (compiled into firmware, app_manager.c's "prelinked:/<name>" path
// convention) or a personal-space app (claw_loader's own "personal:/
// <user>/<name>" convention, a real file but not at that literal path —
// see claw_loader.c's personal_file_path()). Neither is DOWNLOAD-eligible
// this pass — see REMOTEAPPS_ACTION_DOWNLOAD_INFO's own doc comment in
// app_manager_remote.h. Everything scan_dir() finds (app_manager.c) —
// ordinary /flash/apps or /sdcard/apps .claw files — has a real,
// directly fopen()-able path and IS eligible.
static bool path_not_downloadable(const char *path) {
    return strncmp(path, "prelinked:/", 11) == 0 || strncmp(path, "personal:/", 10) == 0;
}

static bool handle_list(const uint8_t mac[6], uint16_t action_id,
                         const uint8_t *req, size_t req_len,
                         uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out) {
    (void)mac; (void)action_id; (void)req; (void)req_len;
    int n = app_manager_count();
    size_t max_entries = resp_cap / sizeof(remote_app_entry_t);
    if ((size_t)n > max_entries) n = (int)max_entries;   // truncate rather than overflow the caller's buffer

    remote_app_entry_t *out = (remote_app_entry_t *)resp_out;
    int written = 0;
    for (int i = 0; i < n; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (!app) continue;
        memset(&out[written], 0, sizeof(out[written]));
        strncpy(out[written].name, app->name, sizeof(out[written].name) - 1);
        out[written].tier         = (uint8_t)app->tier;
        out[written].state        = (uint8_t)app->state;
        out[written].placement    = (uint8_t)app->placement;
        out[written].downloadable = path_not_downloadable(app->path) ? 0 : 1;
        written++;
    }
    *resp_len_out = (size_t)written * sizeof(remote_app_entry_t);
    return true;
}

// Request payload for LAUNCH/STOP — a bare app name, matching
// remote_app_entry_t.name's size. Indices aren't stable across devices
// (see proximity_rpc.h's Phase C notes), so name is the only reliable key
// a remote caller has after a LIST response.
static int find_by_name(const char *name) {
    int n = app_manager_count();
    for (int i = 0; i < n; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (app && strncmp(app->name, name, sizeof(app->name)) == 0) return i;
    }
    return -1;
}

static bool handle_launch(const uint8_t mac[6], uint16_t action_id,
                           const uint8_t *req, size_t req_len,
                           uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out) {
    (void)mac; (void)action_id;
    char name[48] = {0};
    size_t n = req_len < sizeof(name) - 1 ? req_len : sizeof(name) - 1;
    memcpy(name, req, n);

    bool ok = app_manager_launch_by_name(name) == 0;
    if (resp_cap >= 1) {
        // See REMOTEAPPS_LAUNCH_STATUS_* doc comment (app_manager_
        // remote.h) — this device's OWN caps decide OK_DISPLAY vs
        // OK_HEADLESS, nothing about the launched app itself.
        resp_out[0] = !ok ? REMOTEAPPS_LAUNCH_STATUS_FAILED
                    : (proximity_get_own_caps() & PROXIMITY_CAP_HAS_DISPLAY) ? REMOTEAPPS_LAUNCH_STATUS_OK_DISPLAY
                    : REMOTEAPPS_LAUNCH_STATUS_OK_HEADLESS;
        *resp_len_out = 1;
    } else {
        *resp_len_out = 0;
    }
    return ok;
}

static bool handle_stop(const uint8_t mac[6], uint16_t action_id,
                         const uint8_t *req, size_t req_len,
                         uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out) {
    (void)mac; (void)action_id; (void)resp_out; (void)resp_cap;
    char name[48] = {0};
    size_t n = req_len < sizeof(name) - 1 ? req_len : sizeof(name) - 1;
    memcpy(name, req, n);

    int idx = find_by_name(name);
    *resp_len_out = 0;
    if (idx < 0) return false;
    app_manager_stop(idx);
    return true;
}

// See REMOTEAPPS_ACTION_DOWNLOAD_INFO's own doc comment (app_manager_
// remote.h) for why this deliberately does NOT check pairing_is_trusted()
// the way server_mgr.c's own upload handlers do: app_manager_remote.c
// has never gated LIST/LAUNCH/STOP on trust either (this module has no
// pairing.h dependency at all — trust-gating remote APP access was never
// this file's job, matching its own top comment on why it's a separate
// module from app_manager.c in the first place). DOWNLOAD stays
// consistent with its three siblings rather than introducing a new,
// inconsistent access-control axis just for itself.
static bool handle_download_info(const uint8_t mac[6], uint16_t action_id,
                                  const uint8_t *req, size_t req_len,
                                  uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out) {
    (void)mac; (void)action_id;
    *resp_len_out = 0;
    if (resp_cap < sizeof(remote_download_info_t)) return false;

    char name[48] = {0};
    size_t n = req_len < sizeof(name) - 1 ? req_len : sizeof(name) - 1;
    memcpy(name, req, n);

    int idx = find_by_name(name);
    if (idx < 0) return true;
    const app_entry_t *app = app_manager_get(idx);
    if (!app || path_not_downloadable(app->path)) return true;

    struct stat st;
    if (stat(app->path, &st) != 0) return true;

    remote_download_info_t info = { .size = (uint32_t)st.st_size, .tier = (uint8_t)app->tier };
    memcpy(resp_out, &info, sizeof(info));
    *resp_len_out = sizeof(info);
    return true;
}

static bool handle_download_chunk(const uint8_t mac[6], uint16_t action_id,
                                   const uint8_t *req, size_t req_len,
                                   uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out) {
    (void)mac; (void)action_id;
    *resp_len_out = 0;
    if (req_len != sizeof(remote_download_chunk_req_t)) return false;

    remote_download_chunk_req_t r;
    memcpy(&r, req, sizeof(r));
    char name[49] = {0};
    memcpy(name, r.name, sizeof(r.name));

    int idx = find_by_name(name);
    if (idx < 0) return true;
    const app_entry_t *app = app_manager_get(idx);
    if (!app || path_not_downloadable(app->path)) return true;

    FILE *f = fopen(app->path, "rb");
    if (!f) return true;
    if (fseek(f, (long)r.offset, SEEK_SET) != 0) { fclose(f); return true; }

    size_t want = r.want;
    if (want > REMOTEAPPS_DOWNLOAD_CHUNK_MAX) want = REMOTEAPPS_DOWNLOAD_CHUNK_MAX;
    if (want > resp_cap) want = resp_cap;
    *resp_len_out = fread(resp_out, 1, want, f);
    fclose(f);
    return true;
}

void app_manager_remote_register(void) {
    proximity_rpc_register(REMOTEAPPS_ACTION_LIST,            handle_list);
    proximity_rpc_register(REMOTEAPPS_ACTION_LAUNCH,          handle_launch);
    proximity_rpc_register(REMOTEAPPS_ACTION_STOP,            handle_stop);
    proximity_rpc_register(REMOTEAPPS_ACTION_DOWNLOAD_INFO,   handle_download_info);
    proximity_rpc_register(REMOTEAPPS_ACTION_DOWNLOAD_CHUNK,  handle_download_chunk);
}

// ── Caller side ──────────────────────────────────────────────────────────
// The other end of handle_list()/handle_launch()/handle_stop() above — this
// device asking a PAIRED device to list/launch/stop ITS apps. Wired into
// app_manager.c as an app_manager_remote_provider_t (app_manager.h) via
// app_manager_remote_connect() below, so app_manager.c itself never links
// proximity_rpc — see that struct's own doc comment.
//
// Every proximity_rpc_call() here blocks (up to RPC_TIMEOUT_MS) — safe in
// this file only because app_manager.c's own remote-mode task
// (remote_refresh_task()) is the sole caller of ->list(), and a dedicated
// one-shot task (remote_op_task()) is the sole caller of ->launch()/->stop()
// — never the LVGL render task. See app_manager.h's provider-contract
// comment for why that split exists.
#define RPC_TIMEOUT_MS 3000

static int provider_list(const uint8_t mac[6], app_entry_t *out, int max) {
    uint8_t resp[PROXIMITY_RPC_MAX_MSG];
    size_t  resp_len = 0;
    bool ok = proximity_rpc_call(mac, REMOTEAPPS_ACTION_LIST, NULL, 0,
                                  resp, sizeof(resp), &resp_len, RPC_TIMEOUT_MS);
    if (!ok) return -1;

    int n = (int)(resp_len / sizeof(remote_app_entry_t));
    if (n > max) n = max;
    const remote_app_entry_t *entries = (const remote_app_entry_t *)resp;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        strncpy(out[i].name, entries[i].name, sizeof(out[i].name) - 1);
        out[i].tier         = (app_tier_t)entries[i].tier;
        out[i].state        = (app_state_t)entries[i].state;
        out[i].placement    = (app_placement_t)entries[i].placement;
        out[i].downloadable = entries[i].downloadable != 0;
        // path/window/error/speed_demon/mem_free_at_launch stay zeroed —
        // nothing downstream of app_manager's remote-mode dereferences them
        // (app_manager_launch_idx()/_stop()'s remote branches key off
        // ->name alone, same as the UI callers' own build_icon()/label
        // code only ever reads ->name/->tier/->state/->window).
    }
    return n;
}

static bool provider_launch(const uint8_t mac[6], const char *name) {
    uint8_t resp[16]; size_t resp_len = 0;
    bool ok = proximity_rpc_call(mac, REMOTEAPPS_ACTION_LAUNCH,
                                  (const uint8_t *)name, strlen(name),
                                  resp, sizeof(resp), &resp_len, RPC_TIMEOUT_MS);
    // Surfaced via purr_kernel_notify(), not threaded back through
    // app_manager_remote_provider_t.launch()'s own bool-only return —
    // this whole call already runs fire-and-forget on its own one-shot
    // task (app_manager.c's remote_op_task(), "return the instant the
    // work is handed off, real state observed later"), so a synchronous
    // return value was never going to reach the UI either way. The
    // notification IS the mechanism this codebase already uses for
    // exactly this shape of "something happened in the background, tell
    // the user" (proximity's own "Nearby device"/"Service recovered",
    // the memory watchdog's "App stopped"). OK_DISPLAY needs nothing —
    // the app shows on the responder's OWN screen, no false-nothing-
    // happened confusion to correct. FAILED already gets a false return
    // from proximity_rpc_call() itself (a genuine RPC/handler failure);
    // this is specifically the launched-but-can't-be-seen-anywhere case.
    if (ok && resp_len >= 1 && resp[0] == REMOTEAPPS_LAUNCH_STATUS_OK_HEADLESS) {
        purr_kernel_notify("Launched on server", name, "app_manager_remote");
    }
    return ok;
}

static bool provider_stop(const uint8_t mac[6], const char *name) {
    uint8_t resp[16]; size_t resp_len = 0;
    return proximity_rpc_call(mac, REMOTEAPPS_ACTION_STOP,
                               (const uint8_t *)name, strlen(name),
                               resp, sizeof(resp), &resp_len, RPC_TIMEOUT_MS);
}

static const app_manager_remote_provider_t s_provider = {
    .list   = provider_list,
    .launch = provider_launch,
    .stop   = provider_stop,
};

bool app_manager_remote_connect(const uint8_t mac[6]) {
    return app_manager_set_remote(mac, &s_provider);
}

void app_manager_remote_disconnect(void) {
    app_manager_clear_remote();
}

bool app_manager_remote_download(const uint8_t mac[6], const char *name, const char *username) {
    if (!mac || !name || !name[0] || !username) return false;

    uint8_t info_resp[sizeof(remote_download_info_t)];
    size_t  info_len = 0;
    if (!proximity_rpc_call(mac, REMOTEAPPS_ACTION_DOWNLOAD_INFO,
                             (const uint8_t *)name, strlen(name),
                             info_resp, sizeof(info_resp), &info_len, RPC_TIMEOUT_MS) ||
        info_len != sizeof(remote_download_info_t)) {
        ESP_LOGW(TAG, "download '%s': DOWNLOAD_INFO failed or app not downloadable", name);
        return false;
    }
    remote_download_info_t info;
    memcpy(&info, info_resp, sizeof(info));
    if (info.size == 0 || info.size > REMOTEAPPS_MAX_DOWNLOAD_SIZE) {
        ESP_LOGW(TAG, "download '%s': size %u out of range", name, (unsigned)info.size);
        return false;
    }

    // Buffered whole in PSRAM, not streamed to a partial file the way
    // server_mgr.c's own UPLOAD responder does — this side is the
    // CALLER, not something another module's chunk-by-chunk requests
    // land on incrementally, and claw_loader_personal_add() itself only
    // ever takes a complete in-memory blob anyway (claw_loader.h). Only
    // ever handed to claw_loader_personal_add() once every byte has
    // arrived, so a failed/aborted download never leaves a corrupt or
    // partial personal-space file behind.
    uint8_t *buf = heap_caps_malloc(info.size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "download '%s': PSRAM alloc failed (%u bytes)", name, (unsigned)info.size);
        return false;
    }

    uint32_t received = 0;
    bool ok = true;
    while (received < info.size) {
        remote_download_chunk_req_t req = {0};
        strncpy(req.name, name, sizeof(req.name) - 1);
        req.offset = received;
        uint32_t remaining = info.size - received;
        req.want = (uint16_t)(remaining < REMOTEAPPS_DOWNLOAD_CHUNK_MAX ? remaining : REMOTEAPPS_DOWNLOAD_CHUNK_MAX);

        size_t got_len = 0;
        if (!proximity_rpc_call(mac, REMOTEAPPS_ACTION_DOWNLOAD_CHUNK,
                                 (const uint8_t *)&req, sizeof(req),
                                 buf + received, req.want, &got_len, RPC_TIMEOUT_MS) ||
            got_len == 0) {
            ESP_LOGW(TAG, "download '%s': chunk at offset %u failed", name, (unsigned)received);
            ok = false;
            break;
        }
        received += (uint32_t)got_len;
    }

    if (ok) {
        ok = claw_loader_personal_add(username, name, buf, info.size);
        if (!ok) ESP_LOGE(TAG, "download '%s': claw_loader_personal_add() failed", name);
    }
    heap_caps_free(buf);
    if (ok) ESP_LOGI(TAG, "download '%s': complete (%u bytes) -> personal space for '%s'",
                      name, (unsigned)info.size, username);
    return ok;
}

// ── Module lifecycle ──────────────────────────────────────────────────────

static int module_init(void) {
    app_manager_remote_register();
    return 0;
}

// ── Module header ─────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(app_manager_remote) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "app_manager_remote",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = module_init,
    .deinit            = NULL,
};
