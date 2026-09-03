// server_manager_app.c — PURR OS Server Manager (.claw)
//
// Settings/transfer/approval surface for a connected server. Reachable
// two ways: app_manager.h's synthetic "Server Manager" remote-mode entry
// (a server admin, on the remote desktop) pre-selects the already-
// connected server; a normal local Start Menu icon launch starts with no
// target and offers "Select Server" instead — a device picker over
// pairing.h's own trust list (pairing_device_count()/at()). Earlier this
// app was hidden from the normal listing entirely (app_manager.c's own
// is_hidden_local_app()) on the assumption the synthetic entry was the
// only reasonable way in — real, reported consequence: with no local
// icon AND no picker, there was no way to reach it at all outside an
// active remote-mode session, and no Control Panel/Settings entry either.
// Milkbar itself stays a pure connection surface (Connect -> Dashboard ->
// Desktop, all unchanged); this app owns everything past that — WiFi
// setup and app transfer/approval, per this session's own "Server
// Manager" plan doc. See source/modules/server_mgr/server_mgr.h for the
// wire protocol both screens below talk over; this file only ever calls
// its public API, never proximity_rpc/mbedtls directly.
//
// Four windows: the root (server name + Select Server/WiFi/Apps
// buttons), and three lazy sub-windows, same "create once, show/hide
// after" idiom every other multi-screen app in this codebase (milkbar,
// settings, diagnostics) uses.

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR — see s_local_paths's own comment below
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "purr_win.h"
#include "purr_kernel.h"
#include "purr_module.h"
#include "app_manager.h"
#include "app_manager_remote.h"
#include "pairing.h"
#include "server_mgr.h"

static const char *TAG = "server_manager";

// Set once at init() from app_manager_remote_mac() — the server this app
// was launched to manage. Never changes for the life of this app instance
// (closing it and going back to Desktop, then reopening it via the
// synthetic entry again, re-reads it fresh — same shape milkbar's own
// s_selected_mac has for a single, unchanging target per open).
static uint8_t       s_target_mac[6];
static volatile bool s_have_target = false;

// ── Root window ──────────────────────────────────────────────────────────
static purr_win_t s_win        = 0;
static purr_wid_t s_server_lbl = 0;

static void on_subwin_back(purr_wid_t w, purr_event_t e, void *u) {
    (void)w; (void)e;
    purr_win_hide((purr_win_t)(uintptr_t)u);
}
static void add_back_button(purr_win_t win) {
    purr_win_button(win, "< Back", on_subwin_back, (void *)(uintptr_t)win);
}

// Re-derives s_server_lbl's text from s_have_target/s_target_mac — called
// after init() and after a picker selection, so both paths share one
// place that knows how to render "no target yet" vs. a real paired
// device's display name.
static void update_server_label(void) {
    char server_line[64];
    if (s_have_target) {
        char server_name[20] = "?";
        int n = pairing_device_count();
        for (int i = 0; i < n; i++) {
            paired_device_t pd;
            if (pairing_device_at(i, &pd) && memcmp(pd.mac, s_target_mac, 6) == 0) {
                snprintf(server_name, sizeof(server_name), "%s", pd.name);
                break;
            }
        }
        snprintf(server_line, sizeof(server_line), "Server: %s", server_name);
    } else {
        snprintf(server_line, sizeof(server_line), "Server: (none selected)");
    }
    if (s_server_lbl) purr_win_label_set(s_server_lbl, server_line);
}

// ── Target picker ────────────────────────────────────────────────────────
// Launched normally (a local Start Menu icon, not through app_manager.h's
// synthetic remote-mode entry — see this file's own top comment, updated:
// that entry still works and still pre-selects the already-connected
// server, but it's no longer the ONLY way in), this app starts with no
// target at all. "Select Server" lists every paired device (pairing_
// device_count()/at(), pairing.h — the same trust list milkbar's own
// Connection screen reads) and lets the user pick one, filling in
// s_target_mac/s_have_target exactly as app_manager_remote_mac() would
// have. WiFi/Apps both already handle !s_have_target gracefully (a clear
// status, no crash), so nothing downstream needs to change for this.
static purr_win_t s_picker_win  = 0;
static purr_wid_t s_picker_list = 0;
#define MAX_PICKER_ROWS 16
// EXT_RAM_BSS_ATTR — see s_local_paths's own comment below; same class of
// buffer (rebuilt-on-refresh display rows, never touched before PSRAM is
// up), same fix.
static EXT_RAM_BSS_ATTR char s_picker_names[MAX_PICKER_ROWS][20];
static const char *s_picker_name_ptrs[MAX_PICKER_ROWS];
static EXT_RAM_BSS_ATTR uint8_t s_picker_macs[MAX_PICKER_ROWS][6];
static int          s_picker_count = 0;

static void refresh_target_picker(void) {
    if (!s_picker_list) return;
    s_picker_count = 0;
    int n = pairing_device_count();
    for (int i = 0; i < n && s_picker_count < MAX_PICKER_ROWS; i++) {
        paired_device_t pd;
        if (!pairing_device_at(i, &pd)) continue;
        snprintf(s_picker_names[s_picker_count], sizeof(s_picker_names[s_picker_count]), "%s", pd.name);
        s_picker_name_ptrs[s_picker_count] = s_picker_names[s_picker_count];
        memcpy(s_picker_macs[s_picker_count], pd.mac, 6);
        s_picker_count++;
    }
    purr_win_list_set_items(s_picker_list, s_picker_name_ptrs, s_picker_count);
}

static void on_picker_select(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)user;
    if (e != PURR_EVENT_ACTIVATED) return;
    int idx = purr_win_list_get_selected(s_picker_list);
    if (idx < 0 || idx >= s_picker_count) return;
    memcpy(s_target_mac, s_picker_macs[idx], 6);
    s_have_target = true;
    // Also points app_manager.h's own remote mode at the selection — the
    // Apps tab's "On this server" list reads app_manager_count()/get()
    // directly (see refresh_remote_apps()'s own comment), which reflects
    // app_manager's remote-mode state, NOT this app's own s_target_mac.
    // Without this, WiFi/push would correctly target the picked device
    // (server_mgr_wifi_status()/_set()/app_upload() all take s_target_mac
    // directly) while "On this server" silently showed the wrong thing —
    // same class of bug this file's own top comment already documents
    // for the pre-picker "launched with no target" case. Best-effort:
    // WiFi/push still work from s_target_mac alone even if this fails.
    app_manager_remote_connect(s_target_mac);
    update_server_label();
    if (s_picker_win) purr_win_hide(s_picker_win);
}

static void open_target_picker(void) {
    if (!s_picker_win) {
        s_picker_win = purr_win_create("Select Server");
        add_back_button(s_picker_win);
        purr_win_label(s_picker_win, "Paired devices:");
        s_picker_list = purr_win_list(s_picker_win, 100, 60);
        purr_win_list_on_select(s_picker_list, on_picker_select, NULL);
    }
    refresh_target_picker();
    purr_win_show(s_picker_win);
}

static void on_open_picker_click(purr_wid_t w, purr_event_t e, void *user) { (void)w; (void)e; (void)user; open_target_picker(); }

// ── WiFi sub-window ──────────────────────────────────────────────────────
static purr_win_t     s_wifi_win        = 0;
static purr_wid_t     s_wifi_status_lbl = 0;
static purr_wid_t     s_wifi_ssid_input = 0;
static purr_wid_t     s_wifi_pass_input = 0;
static volatile bool  s_wifi_busy       = false;

// Fire-and-forget one-shot tasks, same shape as this whole session's other
// proximity_rpc_call()-driven UI actions (milkbar's retired setup_push_
// task(), app_manager.c's remote_op_task()) — never call server_mgr_wifi_
// status()/_set() (both blocking proximity_rpc_call()s) from cupcake_task.
// Safe to touch s_wifi_status_lbl directly from here despite that: this is
// the portable purr_win_* API, which already defers internally.

static void wifi_status_task(void *arg) {
    (void)arg;
    server_mgr_wifi_status_t status = SRVMGR_WIFI_UNSUPPORTED;
    char ssid[33] = ""; char ip[16] = "";
    bool ok = server_mgr_wifi_status(s_target_mac, &status, ssid, sizeof(ssid), ip, sizeof(ip));

    char buf[80];
    if (!ok) {
        snprintf(buf, sizeof(buf), "Unreachable");
    } else {
        static const char *names[] = { "Unsupported on this server", "Idle", "Connecting...", "Connected", "Failed" };
        const char *name = (status <= SRVMGR_WIFI_FAILED) ? names[status] : "?";
        if (status == SRVMGR_WIFI_CONNECTED && ip[0]) snprintf(buf, sizeof(buf), "%s (%s)", name, ip);
        else snprintf(buf, sizeof(buf), "%s", name);
    }
    if (s_wifi_status_lbl) purr_win_label_set(s_wifi_status_lbl, buf);
    s_wifi_busy = false;
    vTaskDeleteWithCaps(NULL);
}

static void on_wifi_refresh_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    if (!s_have_target || s_wifi_busy) return;
    if (s_wifi_status_lbl) purr_win_label_set(s_wifi_status_lbl, "Checking...");
    s_wifi_busy = true;
    TaskHandle_t task = NULL;
    BaseType_t ok = xTaskCreateWithCaps(wifi_status_task, "srvmgr_wifi_st", 4096, NULL, 3, &task, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) { s_wifi_busy = false; if (s_wifi_status_lbl) purr_win_label_set(s_wifi_status_lbl, "Could not start - try again"); }
}

typedef struct {
    uint8_t mac[6];
    char    ssid[33];
    char    password[64];
} wifi_set_ctx_t;

static void wifi_set_task(void *arg) {
    wifi_set_ctx_t *ctx = (wifi_set_ctx_t *)arg;
    bool ok = server_mgr_wifi_set(ctx->mac, ctx->ssid, ctx->password);
    if (s_wifi_status_lbl) {
        purr_win_label_set(s_wifi_status_lbl,
            ok ? "Sent - connecting on the server" : "Failed (unsupported on that server, or unreachable)");
    }
    memset(ctx->password, 0, sizeof(ctx->password));   // done with it — no reason to keep a plaintext password in RAM longer than necessary
    free(ctx);
    s_wifi_busy = false;
    vTaskDeleteWithCaps(NULL);
}

static void on_wifi_connect_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    if (!s_have_target || s_wifi_busy) return;

    const char *ssid = s_wifi_ssid_input ? purr_win_textarea_get(s_wifi_ssid_input) : NULL;
    if (!ssid || !ssid[0]) {
        if (s_wifi_status_lbl) purr_win_label_set(s_wifi_status_lbl, "Enter an SSID");
        return;
    }
    const char *pass = s_wifi_pass_input ? purr_win_textarea_get(s_wifi_pass_input) : NULL;

    wifi_set_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) return;
    memcpy(ctx->mac, s_target_mac, 6);
    snprintf(ctx->ssid, sizeof(ctx->ssid), "%s", ssid);
    snprintf(ctx->password, sizeof(ctx->password), "%s", pass ? pass : "");

    if (s_wifi_status_lbl) purr_win_label_set(s_wifi_status_lbl, "Sending...");
    s_wifi_busy = true;
    TaskHandle_t task = NULL;
    BaseType_t ok = xTaskCreateWithCaps(wifi_set_task, "srvmgr_wifi_set", 4096, ctx, 3, &task, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        s_wifi_busy = false;
        free(ctx);
        if (s_wifi_status_lbl) purr_win_label_set(s_wifi_status_lbl, "Could not start - try again");
    }
}

static void open_wifi(void) {
    if (s_wifi_win) { purr_win_show(s_wifi_win); return; }

    s_wifi_win = purr_win_create("WiFi");
    add_back_button(s_wifi_win);
    s_wifi_status_lbl = purr_win_label(s_wifi_win,
        s_have_target ? "Tap Refresh to check status" : "Not connected to a server");

    purr_win_label(s_wifi_win, "SSID:");
    s_wifi_ssid_input = purr_win_textarea(s_wifi_win, 100, 16);
    purr_win_label(s_wifi_win, "Password:");
    s_wifi_pass_input = purr_win_textarea(s_wifi_win, 100, 16);

    purr_wid_t row = purr_win_row(s_wifi_win, 2);
    purr_win_button(s_wifi_win, "Refresh", on_wifi_refresh_click, NULL);
    purr_win_button(s_wifi_win, "Connect", on_wifi_connect_click, NULL);
    purr_win_layout_end(row);

    purr_win_show(s_wifi_win);
}

// ── Mesh backend sub-window ──────────────────────────────────────────────
// Same shape as the WiFi sub-window above. The real point of this one:
// Heltec's own oled_ui has no purr_win UI at all, so its local Settings
// screen's "Use RNode Mode" button (settings.c) can never actually be
// tapped there — this is the ONLY way to start/stop RNode mode (or
// switch to any other mesh backend) on a device like that. Unencrypted
// wire (server_mgr.h's own doc comment) — a backend selection isn't a
// secret the way a WiFi password is.

static purr_win_t    s_mesh_win        = 0;
static purr_wid_t    s_mesh_status_lbl = 0;
static volatile bool s_mesh_busy       = false;

// Mirrors purr_mesh_backend_t (purr_kernel.h) exactly — kept as raw
// indices here rather than pulling that header in, same "small local
// mirror" shape server_mgr.h's own wire values already use.
static const char *s_mesh_backend_names[] = { "Meshtastic", "MeshCore", "Reticulum", "RNode" };
#define MESH_BACKEND_COUNT 4

static void mesh_status_task(void *arg) {
    (void)arg;
    uint8_t backend = 0;
    bool ok = server_mgr_mesh_status(s_target_mac, &backend);

    char buf[48];
    if (!ok) {
        snprintf(buf, sizeof(buf), "Unreachable");
    } else if (backend < MESH_BACKEND_COUNT) {
        snprintf(buf, sizeof(buf), "Active: %s", s_mesh_backend_names[backend]);
    } else {
        snprintf(buf, sizeof(buf), "Active: unknown (%u)", (unsigned)backend);
    }
    if (s_mesh_status_lbl) purr_win_label_set(s_mesh_status_lbl, buf);
    s_mesh_busy = false;
    vTaskDeleteWithCaps(NULL);
}

static void on_mesh_refresh_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    if (!s_have_target || s_mesh_busy) return;
    if (s_mesh_status_lbl) purr_win_label_set(s_mesh_status_lbl, "Checking...");
    s_mesh_busy = true;
    TaskHandle_t task = NULL;
    BaseType_t ok = xTaskCreateWithCaps(mesh_status_task, "srvmgr_mesh_st", 4096, NULL, 3, &task, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) { s_mesh_busy = false; if (s_mesh_status_lbl) purr_win_label_set(s_mesh_status_lbl, "Could not start - try again"); }
}

typedef struct {
    uint8_t mac[6];
    uint8_t target;
} mesh_set_ctx_t;

static void mesh_set_task(void *arg) {
    mesh_set_ctx_t *ctx = (mesh_set_ctx_t *)arg;
    bool ok = server_mgr_mesh_set(ctx->mac, ctx->target);
    if (s_mesh_status_lbl) {
        purr_win_label_set(s_mesh_status_lbl,
            ok ? "Switched - may take a moment to come up" : "Failed (unreachable, or refused on the server)");
    }
    free(ctx);
    s_mesh_busy = false;
    vTaskDeleteWithCaps(NULL);
}

static void on_mesh_switch_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e;
    if (!s_have_target || s_mesh_busy) return;

    mesh_set_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) return;
    memcpy(ctx->mac, s_target_mac, 6);
    ctx->target = (uint8_t)(uintptr_t)user;

    if (s_mesh_status_lbl) purr_win_label_set(s_mesh_status_lbl, "Switching...");
    s_mesh_busy = true;
    TaskHandle_t task = NULL;
    BaseType_t ok = xTaskCreateWithCaps(mesh_set_task, "srvmgr_mesh_set", 4096, ctx, 3, &task, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        s_mesh_busy = false;
        free(ctx);
        if (s_mesh_status_lbl) purr_win_label_set(s_mesh_status_lbl, "Could not start - try again");
    }
}

static void open_mesh(void) {
    if (s_mesh_win) { purr_win_show(s_mesh_win); return; }

    s_mesh_win = purr_win_create("Mesh Backend");
    add_back_button(s_mesh_win);
    s_mesh_status_lbl = purr_win_label(s_mesh_win,
        s_have_target ? "Tap Refresh to check status" : "Not connected to a server");

    purr_wid_t row1 = purr_win_row(s_mesh_win, 2);
    purr_win_button(s_mesh_win, "Meshtastic", on_mesh_switch_click, (void *)(uintptr_t)0);
    purr_win_button(s_mesh_win, "MeshCore",   on_mesh_switch_click, (void *)(uintptr_t)1);
    purr_win_layout_end(row1);
    purr_wid_t row2 = purr_win_row(s_mesh_win, 2);
    purr_win_button(s_mesh_win, "Reticulum",  on_mesh_switch_click, (void *)(uintptr_t)2);
    purr_win_button(s_mesh_win, "RNode",      on_mesh_switch_click, (void *)(uintptr_t)3);
    purr_win_layout_end(row2);

    purr_win_button(s_mesh_win, "Refresh", on_mesh_refresh_click, NULL);

    purr_win_show(s_mesh_win);
}

// ── Apps sub-window ──────────────────────────────────────────────────────
// Two lists: "On this server" is just app_manager_count()/get() directly
// when app_manager IS already in remote mode (the synthetic-entry launch
// path, or a normal local launch that also happens to be in remote mode
// for some other reason) — no second caller-side listing call needed at
// all for that case, skipping the synthetic "Server Manager" row itself
// (see refresh_remote_apps()). A normal local launch typically ISN'T in
// remote mode though (that's what Select Server/s_have_target is for
// instead) — refresh_remote_apps() checks s_have_target first and shows
// an honest empty list rather than this device's own local registry in
// that case (a real, previously-reported bug: "On this server" silently
// showing local apps instead).
// "Push from this device" is a small local readdir() over this device's
// OWN /flash/apps + /sdcard/apps (the exact paths app_manager.c's own
// s_scan_paths[] already uses) — not a second app_manager instance.

static purr_win_t    s_apps_win        = 0;
static purr_wid_t    s_remote_list     = 0;
static purr_wid_t    s_local_list      = 0;
static purr_wid_t    s_apps_status_lbl = 0;
static volatile bool s_push_busy       = false;

#define MAX_REMOTE_ROWS 32
static EXT_RAM_BSS_ATTR char s_remote_row_bufs[MAX_REMOTE_ROWS][64];
static const char *s_remote_row_ptrs[MAX_REMOTE_ROWS];

#define MAX_LOCAL_ROWS 32
static EXT_RAM_BSS_ATTR char s_local_row_bufs[MAX_LOCAL_ROWS][48];
static const char *s_local_row_ptrs[MAX_LOCAL_ROWS];
// EXT_RAM_BSS_ATTR (PSRAM, not internal DRAM) — this file is new enough that
// its DRAM cost was never budgeted: these five row/picker buffers alone were
// ~13.9KB of static internal DRAM (confirmed via purr_os.map), a real
// contributor to a genuine link-time `region dram0_0_seg overflowed` once
// combined with msn.c's own 16KB s_chat_logs (see that file's matching fix,
// 2026-09-01). Safe: pure display-row/path text, rebuilt on every refresh,
// never touched before PSRAM is up (this app is a normal launched .claw, not
// early-boot code) — same class MiniWin's own control/list arrays already
// use this attribute for.
static EXT_RAM_BSS_ATTR char s_local_paths[MAX_LOCAL_ROWS][300];
static int           s_local_count = 0;

static void refresh_remote_apps(void) {
    if (!s_remote_list) return;
    // s_have_target false means this instance has no remote target at
    // all (see init()'s own comment on why that "shouldn't happen in
    // practice" but is handled anyway) — app_manager_count()/get() would
    // still return SOMETHING in that case (this device's own LOCAL
    // registry, not the server's), which is exactly the reported bug:
    // "On this server" silently showing local apps instead. Empty list +
    // a clear status beats a wrong one.
    if (!s_have_target) {
        // s_remote_row_ptrs (a real, already-allocated static array), not
        // NULL — count=0 is what actually makes this an empty list; no
        // reason to also hand a backend a NULL items pointer it was never
        // written to expect.
        purr_win_list_set_items(s_remote_list, s_remote_row_ptrs, 0);
        if (s_apps_status_lbl) purr_win_label_set(s_apps_status_lbl, "Not connected to a server");
        return;
    }
    int n = app_manager_count();
    int shown = 0;
    for (int i = 0; i < n && shown < MAX_REMOTE_ROWS; i++) {
        const app_entry_t *app = app_manager_get(i);
        // Skip the synthetic entry this app was itself launched through —
        // see app_manager.h's own doc comment on why it exists.
        if (!app || strcmp(app->name, "Server Manager") == 0) continue;
        snprintf(s_remote_row_bufs[shown], sizeof(s_remote_row_bufs[shown]), "%s%s",
                 app->name, app->state == APP_STATE_RUNNING ? "  (running)" : "");
        s_remote_row_ptrs[shown] = s_remote_row_bufs[shown];
        shown++;
    }
    purr_win_list_set_items(s_remote_list, s_remote_row_ptrs, shown);
}

static void refresh_local_apps(void) {
    s_local_count = 0;
    static const char *paths[] = { "/flash/apps", "/sdcard/apps", NULL };
    for (int p = 0; paths[p] && s_local_count < MAX_LOCAL_ROWS; p++) {
        DIR *d = opendir(paths[p]);
        if (!d) continue;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && s_local_count < MAX_LOCAL_ROWS) {
            const char *ext = strrchr(ent->d_name, '.');
            if (!ext || strcmp(ext, ".claw") != 0) continue;

            size_t base_len = (size_t)(ext - ent->d_name);
            if (base_len >= sizeof(s_local_row_bufs[0])) base_len = sizeof(s_local_row_bufs[0]) - 1;
            memcpy(s_local_row_bufs[s_local_count], ent->d_name, base_len);
            s_local_row_bufs[s_local_count][base_len] = '\0';
            s_local_row_ptrs[s_local_count] = s_local_row_bufs[s_local_count];

            snprintf(s_local_paths[s_local_count], sizeof(s_local_paths[s_local_count]), "%s/%s", paths[p], ent->d_name);
            s_local_count++;
        }
        closedir(d);
    }
    if (s_local_list) purr_win_list_set_items(s_local_list, s_local_row_ptrs, s_local_count);
}

typedef struct {
    uint8_t mac[6];
    char    name[48];
    char    path[300];
} push_ctx_t;

static void push_task(void *arg) {
    push_ctx_t *ctx = (push_ctx_t *)arg;
    bool ok = false;

    FILE *f = fopen(ctx->path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0) {
            // PSRAM — this can be a genuinely large buffer (up to
            // SRVMGR_MAX_UPLOAD_SIZE, server_mgr.h), same reasoning every
            // other big-buffer allocation in this codebase already follows.
            uint8_t *buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
            if (buf) {
                size_t nread = fread(buf, 1, (size_t)sz, f);
                if (nread == (size_t)sz) {
                    ok = server_mgr_app_upload(ctx->mac, ctx->name, buf, (size_t)sz);
                } else {
                    ESP_LOGE(TAG, "push: short read on %s (%u/%ld)", ctx->path, (unsigned)nread, sz);
                }
                heap_caps_free(buf);
            } else {
                ESP_LOGE(TAG, "push: PSRAM alloc failed for %ld bytes", sz);
            }
        }
        fclose(f);
    } else {
        ESP_LOGE(TAG, "push: fopen %s failed", ctx->path);
    }

    if (s_apps_status_lbl) {
        purr_win_label_set(s_apps_status_lbl, ok ? "Pushed - awaiting approval on the server" : "Push failed");
    }
    free(ctx);
    s_push_busy = false;
    vTaskDeleteWithCaps(NULL);
}

static void on_push_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    if (!s_have_target || s_push_busy) return;
    int idx = purr_win_list_get_selected(s_local_list);
    if (idx < 0 || idx >= s_local_count) return;

    push_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) return;
    memcpy(ctx->mac, s_target_mac, 6);
    snprintf(ctx->name, sizeof(ctx->name), "%s", s_local_row_bufs[idx]);
    snprintf(ctx->path, sizeof(ctx->path), "%s", s_local_paths[idx]);

    if (s_apps_status_lbl) purr_win_label_set(s_apps_status_lbl, "Pushing...");
    s_push_busy = true;
    TaskHandle_t task = NULL;
    BaseType_t ok = xTaskCreateWithCaps(push_task, "srvmgr_push", 8192, ctx, 3, &task, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        s_push_busy = false;
        free(ctx);
        if (s_apps_status_lbl) purr_win_label_set(s_apps_status_lbl, "Could not start - try again");
    }
}

static void on_apps_refresh_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    refresh_remote_apps();
    refresh_local_apps();
}

static void open_apps(void) {
    if (s_apps_win) { purr_win_show(s_apps_win); refresh_remote_apps(); refresh_local_apps(); return; }

    s_apps_win = purr_win_create("Apps");
    add_back_button(s_apps_win);

    purr_win_label(s_apps_win, "On this server:");
    s_remote_list = purr_win_list(s_apps_win, 100, 25);

    purr_win_label(s_apps_win, "Push from this device:");
    s_local_list = purr_win_list(s_apps_win, 100, 25);

    purr_wid_t row = purr_win_row(s_apps_win, 2);
    purr_win_button(s_apps_win, "Refresh", on_apps_refresh_click, NULL);
    purr_win_button(s_apps_win, "Push", on_push_click, NULL);
    purr_win_layout_end(row);

    s_apps_status_lbl = purr_win_label(s_apps_win, "");

    purr_win_show(s_apps_win);
    refresh_remote_apps();
    refresh_local_apps();
}

// ── Root window ──────────────────────────────────────────────────────────

static void on_open_wifi_click(purr_wid_t w, purr_event_t e, void *user)  { (void)w; (void)e; (void)user; open_wifi(); }
static void on_open_mesh_click(purr_wid_t w, purr_event_t e, void *user)  { (void)w; (void)e; (void)user; open_mesh(); }
static void on_open_apps_click(purr_wid_t w, purr_event_t e, void *user)  { (void)w; (void)e; (void)user; open_apps(); }

static int server_manager_init(void) {
    // Pre-fills from the synthetic remote-mode entry if that's how this
    // launch happened (app_manager_remote_mac() true) — a normal local
    // Start Menu launch starts with no target instead, resolved via
    // Select Server below.
    s_have_target = app_manager_remote_mac(s_target_mac);
    if (!s_have_target) {
        ESP_LOGI(TAG, "launched with no remote target — use Select Server to pick a paired device");
    }

    s_win = purr_win_create("Server Manager");
    s_server_lbl = purr_win_label(s_win, "");
    update_server_label();

    purr_wid_t row = purr_win_row(s_win, 4);
    purr_win_button(s_win, "Select Server", on_open_picker_click, NULL);
    purr_win_button(s_win, "WiFi",          on_open_wifi_click,   NULL);
    purr_win_button(s_win, "Mesh",          on_open_mesh_click,   NULL);
    purr_win_button(s_win, "Apps",          on_open_apps_click,   NULL);
    purr_win_layout_end(row);

    purr_win_show(s_win);
    return 0;
}

static void server_manager_deinit(void) {
    if (s_picker_win) {
        purr_win_destroy(s_picker_win);
        s_picker_win = 0; s_picker_list = 0;
    }
    if (s_wifi_win) {
        purr_win_destroy(s_wifi_win);
        s_wifi_win = 0; s_wifi_status_lbl = 0; s_wifi_ssid_input = 0; s_wifi_pass_input = 0;
    }
    if (s_mesh_win) {
        purr_win_destroy(s_mesh_win);
        s_mesh_win = 0; s_mesh_status_lbl = 0;
    }
    if (s_apps_win) {
        purr_win_destroy(s_apps_win);
        s_apps_win = 0; s_remote_list = 0; s_local_list = 0; s_apps_status_lbl = 0;
    }
    if (s_win) { purr_win_destroy(s_win); s_win = 0; s_server_lbl = 0; }
    s_have_target = false;
}

// ── Module header ─────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(server_manager) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_APP,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "server_manager",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = server_manager_init,
    .deinit            = server_manager_deinit,
};
