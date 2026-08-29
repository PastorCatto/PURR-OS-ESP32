// server_manager_app.c — PURR OS Server Manager (.claw)
//
// Settings/transfer/approval surface for a connected server — reached
// ONLY via app_manager.h's synthetic "Server Manager" remote-mode entry
// (a server admin, on the remote desktop — see that header's own doc
// comment), never launched any other way. Milkbar itself stays a pure
// connection surface (Connect -> Dashboard -> Desktop, all unchanged);
// this app owns everything past that — WiFi setup and app transfer/
// approval, per this session's own "Server Manager" plan doc. See
// source/modules/server_mgr/server_mgr.h for the wire protocol both
// screens below talk over; this file only ever calls its public API,
// never proximity_rpc/mbedtls directly.
//
// Three windows: the root (server name + WiFi/Apps buttons), and two lazy
// sub-windows, same "create once, show/hide after" idiom every other
// multi-screen app in this codebase (milkbar, settings, diagnostics) uses.

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "purr_win.h"
#include "purr_kernel.h"
#include "purr_module.h"
#include "app_manager.h"
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

// ── Apps sub-window ──────────────────────────────────────────────────────
// Two lists: "On this server" is just app_manager_count()/get() directly —
// this app only ever runs while app_manager is already in remote mode
// (that's the only way to reach it, see this file's top comment), so
// there's no second caller-side listing call needed here at all, skipping
// the synthetic "Server Manager" row itself (see refresh_remote_apps()).
// "Push from this device" is a small local readdir() over this device's
// OWN /flash/apps + /sdcard/apps (the exact paths app_manager.c's own
// s_scan_paths[] already uses) — not a second app_manager instance.

static purr_win_t    s_apps_win        = 0;
static purr_wid_t    s_remote_list     = 0;
static purr_wid_t    s_local_list      = 0;
static purr_wid_t    s_apps_status_lbl = 0;
static volatile bool s_push_busy       = false;

#define MAX_REMOTE_ROWS 32
static char        s_remote_row_bufs[MAX_REMOTE_ROWS][64];
static const char *s_remote_row_ptrs[MAX_REMOTE_ROWS];

#define MAX_LOCAL_ROWS 32
static char        s_local_row_bufs[MAX_LOCAL_ROWS][48];
static const char *s_local_row_ptrs[MAX_LOCAL_ROWS];
static char         s_local_paths[MAX_LOCAL_ROWS][300];
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
static void on_open_apps_click(purr_wid_t w, purr_event_t e, void *user)  { (void)w; (void)e; (void)user; open_apps(); }

static int server_manager_init(void) {
    s_have_target = app_manager_remote_mac(s_target_mac);

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
        // Shouldn't happen in practice — this app only ever launches
        // through app_manager.h's synthetic entry, which only exists
        // while remote mode is on — but fails toward an honest message
        // rather than a blank label if it somehow does.
        snprintf(server_line, sizeof(server_line), "Server: (not connected)");
        ESP_LOGW(TAG, "launched with no remote target — app_manager_remote_mac() returned false");
    }

    s_win = purr_win_create("Server Manager");
    s_server_lbl = purr_win_label(s_win, server_line);

    purr_wid_t row = purr_win_row(s_win, 2);
    purr_win_button(s_win, "WiFi", on_open_wifi_click, NULL);
    purr_win_button(s_win, "Apps", on_open_apps_click, NULL);
    purr_win_layout_end(row);

    purr_win_show(s_win);
    return 0;
}

static void server_manager_deinit(void) {
    if (s_wifi_win) {
        purr_win_destroy(s_wifi_win);
        s_wifi_win = 0; s_wifi_status_lbl = 0; s_wifi_ssid_input = 0; s_wifi_pass_input = 0;
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
