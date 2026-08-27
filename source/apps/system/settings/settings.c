// settings.c — PURR OS system settings app (.claw)
// Uses purr_win.h — compatible with KittenUI (LVGL) and MiniWin.
//
// Sections: Theme  |  Display  |  Storage  |  Input
// Settings are persisted to NVS under namespace "purr_settings".

#include <string.h>
#include <stdio.h>
#include <stdlib.h>   // strtoul() — accent colour hex parsing
#include <stdint.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "purr_win.h"
#include "purr_kernel.h"
#include "purr_module.h"
#include "wifi_mgr.h"
#include "bt_mgr.h"
#include "mesh_ble.h"
#include "ota_mgr.h"
#include "systemui.h"   // purr_systemui_fx_refresh() — see on_effects_toggle()
#include "sdkconfig.h"

// LV_SYMBOL_* glyphs for the category tile grid below — meaningless (and
// their backing font absent) under MiniWin/Pounce, same as msn.c's own
// tile grid.
#ifdef CONFIG_PURR_UI_LVGL
#include "lvgl.h"
#endif

#define NVS_NS  "purr_settings"

#define MAX_WALLPAPERS      16
// Generous enough to comfortably hold "/sdcard/wallpapers/" + a max-length
// filesystem filename with room to spare — GCC's -Wformat-truncation (an
// error under this build's -Werror) can otherwise flag the directory-listing
// snprintf() below as a possible truncation.
#define WALLPAPER_PATH_LEN  300

// ── State ─────────────────────────────────────────────────────────────────────

static purr_win_t  s_win       = 0;   // top-level window — just the category picker
// One menu replaces the old tile-grid-plus-list-fallback pair. purr_win_menu()
// renders natively on every backend (iOS grouped table under Mochi, a plain
// list elsewhere), so there is no longer a second code path to keep working —
// nor the two #ifdef'd symbol tables the tile grid needed.
static purr_wid_t  s_cat_menu     = 0;
static purr_wid_t  s_general_menu = 0;

// Category sub-windows, each built lazily on first tap and cached/reused
// afterward — same pattern the WiFi/BT windows below already established.
// Every one of these (and WiFi/BT) gets an explicit "< Back" button as its
// first widget (see on_subwin_back()) since none of them have any window
// chrome — under Lollipop, purr_win_on_close()'s close-icon hook never
// fires (see cupcake_win.c's s_close_hooks doc comment), and the nav bar's
// own Back button only ever reaches the *app's* original window
// (app_manager.c's app->window, set once from settings_init()'s own
// purr_win_create() call) — without their own Back button, any of these
// would be permanently unreachable/undismissable once opened.
static purr_win_t  s_general_win        = 0;
static purr_wid_t  s_general_status_lbl = 0;
static purr_win_t  s_display_win        = 0;
static purr_wid_t  s_display_status_lbl = 0;
static purr_win_t  s_customization_win        = 0;
static purr_wid_t  s_customization_status_lbl = 0;
static purr_win_t  s_connectivity_win = 0;
static purr_wid_t  s_mesh_backend_status_lbl = 0;
static purr_win_t  s_mesh_switch_confirm_win = 0;
static purr_mesh_backend_t s_mesh_switch_target;
static purr_win_t  s_about_win = 0;

// Updates category — see on_open_updates(). ota_mgr_check()/apply() are
// documented-blocking network calls, same class of problem bt_scan_task()
// below already solved for Bluetooth scanning: never run them on cupcake_task,
// always on their own background task, guarded by s_ota_busy the same way
// s_bt_scanning guards a concurrent scan.
static purr_win_t  s_updates_win        = 0;
static purr_wid_t  s_updates_status_lbl = 0;
static purr_win_t  s_ota_url_dlg_win    = 0;
static purr_wid_t  s_ota_url_dlg_input  = 0;
static volatile bool s_ota_busy = false;

static purr_wid_t  s_brightness_lbl = 0;
static purr_wid_t  s_screen_timeout_lbl = 0;

static uint8_t     s_brightness = 255;
static uint8_t     s_screen_timeout_min = 1;   // must match purr_kernel.h's own default
static char        s_theme[16]  = "wce";

static uint8_t     s_dev_mode     = 0;   // 0/1 — see purr_kernel.h's doc comment

static purr_wid_t  s_navbar_visible_lbl     = 0;
static uint8_t     s_navbar_always_visible  = 0;   // 0/1 — see purr_kernel.h's doc comment
static purr_wid_t  s_lock_notifs_lbl        = 0;
// 1 = lock screen shows only a count. Defaults to 1 to match the kernel's own
// privacy default, so a device with no stored preference hides them.
static uint8_t     s_lock_hide_notifs       = 1;
// UI effects (translucency) + the accent colour that replaces it when off.
// Defaults match purr_kernel.c's own — effects on, dark blue-grey accent — so a
// device with nothing stored behaves identically to one that never had this
// setting. Accent is kept as a u32 and persisted as a hex STRING rather than a
// blob, because that is exactly what the user types and what they would see if
// they ever dumped NVS.
static uint8_t     s_ui_effects             = 1;
static uint32_t    s_accent_color           = 0x1C1C2E;
static purr_wid_t  s_effects_lbl            = 0;
static purr_wid_t  s_accent_lbl             = 0;
static purr_wid_t  s_accent_input           = 0;

static purr_wid_t  s_about_lbl = 0;

#define MAX_WIFI_RESULTS 24
static purr_win_t  s_wifi_win        = 0;   // separate "WiFi Settings" window, built on demand
static purr_wid_t  s_wifi_status_lbl = 0;
static purr_wid_t  s_wifi_list       = 0;
static char        s_wifi_labels[MAX_WIFI_RESULTS][64];
static const char *s_wifi_label_ptrs[MAX_WIFI_RESULTS];
static char        s_wifi_ssids[MAX_WIFI_RESULTS][33];   // parallel to the list above
static bool        s_wifi_secured[MAX_WIFI_RESULTS];
static int         s_wifi_count = 0;

static purr_win_t  s_wifi_dlg_win   = 0;
static purr_wid_t  s_wifi_dlg_input = 0;
static char        s_wifi_dlg_ssid[33] = "";

// Bluetooth is gated behind CONFIG_BT_NIMBLE_ENABLED (off by default — see
// bt_mgr.c/Kconfig.projbuild) — the whole section, state included, compiles
// out when it's off rather than being deleted, so a future device that
// enables it gets the full working UI back with zero further changes.
#ifdef CONFIG_BT_NIMBLE_ENABLED
#define MAX_BT_RESULTS 24
static purr_win_t  s_bt_win        = 0;   // separate "Bluetooth Settings" window, built on demand
static purr_wid_t  s_bt_status_lbl = 0;
static purr_wid_t  s_bt_list       = 0;
static char        s_bt_labels[MAX_BT_RESULTS][48];
static const char *s_bt_label_ptrs[MAX_BT_RESULTS];
static uint8_t     s_bt_addrs[MAX_BT_RESULTS][6];
static int         s_bt_count = 0;
// Set while a scan task (below) is in flight — blocks a second concurrent
// scan rather than racing s_bt_count/s_bt_labels between two background
// tasks. Cleared by bt_scan_task() itself right before it exits.
static volatile bool s_bt_scanning = false;
// Given by bt_scan_task() right before it self-deletes, waited on by
// settings_deinit() before it destroys s_bt_win/s_bt_list/s_bt_status_lbl —
// otherwise closing Settings mid-scan lets that task touch widgets out from
// under a window that's already gone. Same use-after-free shape as
// nearby_app.c/msn.c/milkbar_app.c's own s_refresh_done.
static SemaphoreHandle_t s_bt_scan_done = NULL;
#endif  // CONFIG_BT_NIMBLE_ENABLED

static purr_wid_t  s_wallpaper_list = 0;
static char        s_wallpaper_paths[MAX_WALLPAPERS][WALLPAPER_PATH_LEN];
static char        s_wallpaper_labels[MAX_WALLPAPERS][40];
static const char *s_wallpaper_label_ptrs[MAX_WALLPAPERS];
static int         s_wallpaper_count = 0;

// ── NVS helpers ───────────────────────────────────────────────────────────────

static void nvs_save_str(const char *key, const char *val) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_save_u8(const char *key, uint8_t val) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_theme);
    nvs_get_str(h, "theme", s_theme, &len);
    nvs_get_u8(h, "brightness", &s_brightness);
    nvs_get_u8(h, "screen_timeout", &s_screen_timeout_min);
    nvs_get_u8(h, "dev_mode", &s_dev_mode);
    nvs_get_u8(h, "navbar_always_visible", &s_navbar_always_visible);
    nvs_get_u8(h, "lock_hide_notifs", &s_lock_hide_notifs);
    nvs_get_u8(h, "ui_effects", &s_ui_effects);
    // Accent is stored as the same "RRGGBB" text the user types. strtoul with
    // base 16 on a buffer that nvs_get_str left untouched would parse whatever
    // garbage was on the stack, so the read is only trusted when it succeeds
    // AND produces the 6 characters we wrote.
    {
        char hex[8] = {0};
        size_t hlen = sizeof(hex);
        if (nvs_get_str(h, "accent_color", hex, &hlen) == ESP_OK && hlen >= 7) {
            s_accent_color = (uint32_t)strtoul(hex, NULL, 16) & 0x00FFFFFFu;
        }
    }
    nvs_close(h);
    // Sync into the kernel-global cupcake_ui.c's idle check actually reads
    // — nvs_get_u8() only touched our own local copy above. Without this,
    // a timeout the user picked in a previous session stays invisible to
    // the idle check until Settings happens to be reopened and a button
    // pressed again.
    purr_kernel_set_screen_timeout_min(s_screen_timeout_min);
    // Same reasoning for the lock screen's notification privacy: the system UI
    // reads the kernel flag, not this app's local copy, so a stored preference
    // is invisible until pushed across at boot.
    purr_kernel_set_lock_hide_notifications(s_lock_hide_notifs != 0);
    // Same again: every translucent surface in both System UI styles reads the
    // kernel flag at build time, so a stored preference is invisible until
    // pushed across here at boot.
    purr_kernel_set_ui_effects(s_ui_effects != 0);
    purr_kernel_set_accent_color(s_accent_color);
}

static void set_general_status(const char *msg) {
    if (s_general_status_lbl) purr_win_label_set(s_general_status_lbl, msg);
}
static void set_display_status(const char *msg) {
    if (s_display_status_lbl) purr_win_label_set(s_display_status_lbl, msg);
}
static void set_customization_status(const char *msg) {
    if (s_customization_status_lbl) purr_win_label_set(s_customization_status_lbl, msg);
}

// Shared by every category/WiFi/BT sub-window — see their doc comment
// above for why each one needs this. purr_win_t is a plain uint32_t handle
// (catcall_ui.h), so it round-trips through the callback's void* user
// pointer without needing per-window wrapper state.
static void on_subwin_back(purr_wid_t w, purr_event_t e, void *u) {
    (void)w; (void)e;
    purr_win_hide((purr_win_t)(uintptr_t)u);
}
static void add_back_button(purr_win_t win) {
    purr_win_button(win, "< Back", on_subwin_back, (void *)(uintptr_t)win);
}

// ── Theme buttons ─────────────────────────────────────────────────────────────

static void apply_theme_nvs(const char *id) {
    nvs_save_str("theme", id);
    strncpy(s_theme, id, sizeof(s_theme) - 1);

    char msg[48];
    snprintf(msg, sizeof(msg), "Theme set to '%s' — reboot to apply.", id);
    set_customization_status(msg);
}

static void on_theme_wce(purr_wid_t w, purr_event_t e, void *u)  { (void)w;(void)e;(void)u; apply_theme_nvs("wce");  }
static void on_theme_dark(purr_wid_t w, purr_event_t e, void *u) { (void)w;(void)e;(void)u; apply_theme_nvs("dark"); }

// ── Brightness ────────────────────────────────────────────────────────────────

static void set_brightness(uint8_t level) {
    s_brightness = level;
    const catcall_display_t *disp = purr_kernel_display();
    if (disp && disp->set_brightness) disp->set_brightness(level);
    nvs_save_u8("brightness", level);
    char buf[32];
    snprintf(buf, sizeof(buf), "Brightness: %d%%", (level * 100) / 255);
    purr_win_label_set(s_brightness_lbl, buf);
    set_display_status("Brightness updated.");
}

static void on_bright_high(purr_wid_t w, purr_event_t e, void *u) { (void)w;(void)e;(void)u; set_brightness(255); }
static void on_bright_mid (purr_wid_t w, purr_event_t e, void *u) { (void)w;(void)e;(void)u; set_brightness(160); }
static void on_bright_low (purr_wid_t w, purr_event_t e, void *u) { (void)w;(void)e;(void)u; set_brightness(80);  }

// ── Screen timeout ────────────────────────────────────────────────────────────
// Only meaningfully acted on by Cupcake's lock screen today (cupcake_ui.c's
// idle check) — kept here as a plain kernel-global rather than a direct
// call into cupcake_*, so this section stays backend-agnostic like the
// rest of Settings (this app also ships on MiniWin devices).

static void set_screen_timeout(uint8_t minutes) {
    s_screen_timeout_min = minutes;
    purr_kernel_set_screen_timeout_min(minutes);
    nvs_save_u8("screen_timeout", minutes);
    char buf[32];
    snprintf(buf, sizeof(buf), "Screen timeout: %d min", minutes);
    purr_win_label_set(s_screen_timeout_lbl, buf);
    set_display_status("Screen timeout updated.");
}

static void on_timeout_1(purr_wid_t w, purr_event_t e, void *u) { (void)w;(void)e;(void)u; set_screen_timeout(1); }
static void on_timeout_3(purr_wid_t w, purr_event_t e, void *u) { (void)w;(void)e;(void)u; set_screen_timeout(3); }
static void on_timeout_5(purr_wid_t w, purr_event_t e, void *u) { (void)w;(void)e;(void)u; set_screen_timeout(5); }

// ── Wallpaper ─────────────────────────────────────────────────────────────────
// "Default" (the launcher's built-in gradient) plus every file found under
// /sdcard/wallpapers/ — same opendir/readdir listing pattern fileman.c already
// uses. Selecting one just persists the choice to NVS; the launcher reads it
// back and loads the image itself on next boot (matching the existing
// theme/brightness "reboot to apply" behavior below).

static void refresh_wallpaper_list(void) {
    s_wallpaper_count = 0;

    strncpy(s_wallpaper_paths[s_wallpaper_count], "default", WALLPAPER_PATH_LEN - 1);
    strncpy(s_wallpaper_labels[s_wallpaper_count], "Default", sizeof(s_wallpaper_labels[0]) - 1);
    s_wallpaper_count++;

    DIR *d = opendir("/sdcard/wallpapers");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && s_wallpaper_count < MAX_WALLPAPERS) {
            if (ent->d_name[0] == '.') continue;
            snprintf(s_wallpaper_paths[s_wallpaper_count], WALLPAPER_PATH_LEN,
                     "/sdcard/wallpapers/%s", ent->d_name);
            strncpy(s_wallpaper_labels[s_wallpaper_count], ent->d_name,
                    sizeof(s_wallpaper_labels[0]) - 1);
            s_wallpaper_count++;
        }
        closedir(d);
    }

    for (int i = 0; i < s_wallpaper_count; i++) s_wallpaper_label_ptrs[i] = s_wallpaper_labels[i];
    purr_win_list_set_items(s_wallpaper_list, s_wallpaper_label_ptrs, s_wallpaper_count);
}

static void on_wallpaper_select(purr_wid_t w, purr_event_t e, void *u) {
    (void)w; (void)u;
    if (e != PURR_EVENT_ACTIVATED) return;
    int idx = purr_win_list_get_selected(s_wallpaper_list);
    if (idx < 0 || idx >= s_wallpaper_count) return;

    nvs_save_str("wallpaper", s_wallpaper_paths[idx]);
    char msg[WALLPAPER_PATH_LEN + 64];
    snprintf(msg, sizeof(msg), "Wallpaper set to '%s' — reboot to apply.", s_wallpaper_paths[idx]);
    set_customization_status(msg);
}

// ── WiFi ──────────────────────────────────────────────────────────────────────
// Lives in its own window (opened from the "WiFi Settings" button on the main
// Settings screen) rather than as an inline section — keeps the main window's
// widget count small (MiniWin's control/message-queue budget is finite) and
// gives WiFi its own focused space to connect/disconnect.

static void set_wifi_status(const char *msg) {
    if (s_wifi_status_lbl) purr_win_label_set(s_wifi_status_lbl, msg);
}

static void refresh_wifi_status(void) {
    switch (wifi_mgr_status()) {
        case WIFI_MGR_CONNECTED:
            { char buf[48]; snprintf(buf, sizeof(buf), "WiFi: connected (%s)", wifi_mgr_ip_str());
              set_wifi_status(buf); }
            break;
        case WIFI_MGR_CONNECTING: set_wifi_status("WiFi: connecting..."); break;
        case WIFI_MGR_FAILED:     set_wifi_status("WiFi: connection failed."); break;
        default:                  set_wifi_status("WiFi: idle."); break;
    }
}

static void close_wifi_dialog(void) {
    if (s_wifi_dlg_win) purr_win_destroy(s_wifi_dlg_win);
    s_wifi_dlg_win = 0;
    s_wifi_dlg_input = 0;
}

static void on_wifi_dlg_cancel(purr_wid_t w, purr_event_t e, void *u) { (void)w;(void)e;(void)u; close_wifi_dialog(); }

static void on_wifi_dlg_connect(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    const char *password = s_wifi_dlg_input ? purr_win_textarea_get(s_wifi_dlg_input) : "";
    wifi_mgr_connect(s_wifi_dlg_ssid, password ? password : "");
    close_wifi_dialog();
    refresh_wifi_status();
}

static void on_wifi_select(purr_wid_t w, purr_event_t e, void *u) {
    (void)w; (void)u;
    if (e != PURR_EVENT_ACTIVATED) return;
    int idx = purr_win_list_get_selected(s_wifi_list);
    if (idx < 0 || idx >= s_wifi_count) return;
    strncpy(s_wifi_dlg_ssid, s_wifi_ssids[idx], sizeof(s_wifi_dlg_ssid) - 1);

    // Open network — connect straight away, no password prompt needed.
    if (!s_wifi_secured[idx]) {
        wifi_mgr_connect(s_wifi_dlg_ssid, "");
        refresh_wifi_status();
        return;
    }

    s_wifi_dlg_win = purr_win_create("WiFi Password");
    char lbl[48];
    snprintf(lbl, sizeof(lbl), "Password for %s:", s_wifi_dlg_ssid);
    purr_win_label(s_wifi_dlg_win, lbl);
    s_wifi_dlg_input = purr_win_textarea(s_wifi_dlg_win, 90, 20);

    purr_wid_t row = purr_win_row(s_wifi_dlg_win, 4);
    purr_win_button(s_wifi_dlg_win, "Connect", on_wifi_dlg_connect, NULL);
    purr_win_button(s_wifi_dlg_win, "Cancel",  on_wifi_dlg_cancel,  NULL);
    purr_win_layout_end(row);

    purr_win_textarea_focus(s_wifi_dlg_input);
    // win_show() first — see terminal.c's terminal_init() for why (Cupcake's
    // win_show() raises the window above whatever kb_show() just showed).
    purr_win_show(s_wifi_dlg_win);
    purr_win_keyboard_show(s_wifi_dlg_win, s_wifi_dlg_input);
}

static void on_wifi_scan(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    set_wifi_status("Scanning...");
    int n = wifi_mgr_scan();
    if (n < 0) { set_wifi_status("WiFi scan failed."); return; }
    if (n > MAX_WIFI_RESULTS) n = MAX_WIFI_RESULTS;
    s_wifi_count = n;

    for (int i = 0; i < n; i++) {
        wifi_scan_result_t r;
        wifi_mgr_scan_at(i, &r);
        strncpy(s_wifi_ssids[i], r.ssid, sizeof(s_wifi_ssids[i]) - 1);
        s_wifi_secured[i] = r.secured;
        snprintf(s_wifi_labels[i], sizeof(s_wifi_labels[i]), "%s  %ddBm  (%s)",
                 r.ssid, r.rssi, r.secured ? "secured" : "open");
        s_wifi_label_ptrs[i] = s_wifi_labels[i];
    }
    purr_win_list_set_items(s_wifi_list, s_wifi_label_ptrs, s_wifi_count);
    set_wifi_status("Scan complete — tap a network to connect.");
}

static void on_wifi_disconnect(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    wifi_mgr_disconnect();
    refresh_wifi_status();
}

static void on_wifi_win_close(purr_wid_t win, purr_event_t event, void *u) {
    (void)win; (void)event; (void)u;
    close_wifi_dialog();
    s_wifi_win = 0;
    s_wifi_status_lbl = 0;
    s_wifi_list = 0;
}

static void on_wifi_settings_open(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    if (s_wifi_win) { purr_win_show(s_wifi_win); return; }

    s_wifi_win = purr_win_create("WiFi Settings");
    purr_win_on_close(s_wifi_win, on_wifi_win_close, NULL);
    add_back_button(s_wifi_win);

    purr_win_label(s_wifi_win, "Networks");
    s_wifi_list = purr_win_list(s_wifi_win, 90, 60);
    purr_win_list_on_select(s_wifi_list, on_wifi_select, NULL);

    purr_wid_t wr = purr_win_row(s_wifi_win, 4);
    purr_win_button(s_wifi_win, "Scan",       on_wifi_scan,       NULL);
    purr_win_button(s_wifi_win, "Disconnect", on_wifi_disconnect, NULL);
    purr_win_layout_end(wr);

    s_wifi_status_lbl = purr_win_label(s_wifi_win, "Ready.");
    refresh_wifi_status();
    purr_win_show(s_wifi_win);
}

// ── Bluetooth ─────────────────────────────────────────────────────────────────
// BLE only — T-Deck Plus's ESP32-S3 has no classic Bluetooth hardware (see
// bt_mgr.h's comment). Lives in its own window, same rationale as WiFi above.
// Gated behind CONFIG_BT_NIMBLE_ENABLED — see the s_bt_* state block above.
#ifdef CONFIG_BT_NIMBLE_ENABLED

static void set_bt_status(const char *msg) {
    if (s_bt_status_lbl) purr_win_label_set(s_bt_status_lbl, msg);
}

static void on_bt_toggle(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    bool want_on = !bt_mgr_is_enabled();
    bool on = bt_mgr_set_enabled(want_on);
    // bt_mgr_set_enabled() now lazily brings the NimBLE controller/host up
    // on its first enable — it can fail here (e.g. still no DMA-capable
    // memory available for some other reason) where it never used to be
    // able to at this point before (activation used to always happen at
    // boot instead). Only follow through on the Meshtastic companion
    // toggle if activation actually succeeded.
    if (want_on && !on) {
        set_bt_status("Bluetooth failed to start.");
        return;
    }
    mesh_ble_set_advertising(on);   // Meshtastic phone-app companion service follows the same toggle
    set_bt_status(on ? "Bluetooth enabled." : "Bluetooth disabled.");
}

// bt_mgr_scan() blocks its caller for up to 7s (duration + 2s grace,
// waiting on NimBLE's own scan-complete semaphore — see bt_mgr.c). Calling
// that directly from a button click ran it on cupcake_task itself, which is
// also the task subscribed to the 5s task watchdog (see cupcake_module.c) —
// starving it of a single esp_task_wdt_reset() call for that whole window
// tripped the watchdog for real, forcing a hard reboot, confirmed live.
// Same class of bug as proximity_rpc_call() elsewhere in this codebase
// (milkbar_app.c's own top comment) and fixed the same way: run the
// blocking call on its own background task, never on cupcake_task.
static void bt_scan_task(void *arg) {
    (void)arg;
    int n = bt_mgr_scan(5);
    if (n < 0) {
        set_bt_status("Bluetooth scan failed.");
    } else {
        if (n > MAX_BT_RESULTS) n = MAX_BT_RESULTS;
        s_bt_count = n;
        for (int i = 0; i < n; i++) {
            bt_scan_result_t r;
            bt_mgr_scan_at(i, &r);
            memcpy(s_bt_addrs[i], r.addr, 6);
            snprintf(s_bt_labels[i], sizeof(s_bt_labels[i]), "%s  %ddBm", r.name, r.rssi);
            s_bt_label_ptrs[i] = s_bt_labels[i];
        }
        purr_win_list_set_items(s_bt_list, s_bt_label_ptrs, s_bt_count);
        set_bt_status("Scan complete — tap a device to pair.");
    }
    s_bt_scanning = false;
    if (s_bt_scan_done) xSemaphoreGive(s_bt_scan_done);
    vTaskDeleteWithCaps(NULL);
}

static void on_bt_scan(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    if (!bt_mgr_is_enabled()) { set_bt_status("Enable Bluetooth first."); return; }
    if (s_bt_scanning) return;   // a scan is already in flight

    if (!s_bt_scan_done) s_bt_scan_done = xSemaphoreCreateBinary();
    set_bt_status("Scanning (BLE)...");
    s_bt_scanning = true;
    // PSRAM-backed stack — same rationale as milkbar_app.c's send_msg_task:
    // internal DRAM is already the scarce resource NimBLE itself is fighting
    // for, no reason for this task's own stack to compete for it too.
    TaskHandle_t task = NULL;
    BaseType_t ok = xTaskCreateWithCaps(bt_scan_task, "bt_scan", 4096, NULL, 3, &task, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) { s_bt_scanning = false; set_bt_status("Bluetooth scan failed."); }
}

static void on_bt_select(purr_wid_t w, purr_event_t e, void *u) {
    (void)w; (void)u;
    if (e != PURR_EVENT_ACTIVATED) return;
    int idx = purr_win_list_get_selected(s_bt_list);
    if (idx < 0 || idx >= s_bt_count) return;

    esp_err_t ret = bt_mgr_pair(s_bt_addrs[idx]);
    char msg[64];
    snprintf(msg, sizeof(msg), "Pairing with %s...", s_bt_labels[idx]);
    set_bt_status(ret == ESP_OK ? msg : "Pair request failed.");
}

static void on_bt_win_close(purr_wid_t win, purr_event_t event, void *u) {
    (void)win; (void)event; (void)u;
    s_bt_win = 0;
    s_bt_status_lbl = 0;
    s_bt_list = 0;
}

static void on_bt_settings_open(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    if (s_bt_win) { purr_win_show(s_bt_win); return; }

    s_bt_win = purr_win_create("Bluetooth Settings");
    purr_win_on_close(s_bt_win, on_bt_win_close, NULL);
    add_back_button(s_bt_win);

    purr_win_label(s_bt_win, "Devices (BLE)");
    s_bt_list = purr_win_list(s_bt_win, 90, 60);
    purr_win_list_on_select(s_bt_list, on_bt_select, NULL);

    purr_wid_t btr = purr_win_row(s_bt_win, 4);
    purr_win_button(s_bt_win, "Enable/Disable", on_bt_toggle, NULL);
    purr_win_button(s_bt_win, "Scan",           on_bt_scan,   NULL);
    purr_win_layout_end(btr);

    s_bt_status_lbl = purr_win_label(s_bt_win, bt_mgr_is_enabled() ? "Bluetooth enabled." : "Bluetooth disabled.");
    purr_win_show(s_bt_win);
}

#endif  // CONFIG_BT_NIMBLE_ENABLED

// ── Developer Mode ────────────────────────────────────────────────────────────
// Gates whether unsigned .hiss scripts are allowed to run — see
// purr_kernel_set_dev_mode()'s doc comment in purr_kernel.h. Only "unsigned"
// .hiss scripts are affected; a signed one always runs regardless of this.
// Off by default — persisted so it survives a reboot, same as brightness
// above.

static void on_dev_mode_toggle(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    s_dev_mode = s_dev_mode ? 0 : 1;
    purr_kernel_set_dev_mode(s_dev_mode != 0);
    nvs_save_u8("dev_mode", s_dev_mode);
    // No separate label any more — the General menu row's VALUE is the state,
    // and on_general_menu() calls general_refresh() right after this.
    set_general_status(s_dev_mode ? "Developer Mode enabled — unsigned .hiss scripts allowed."
                                   : "Developer Mode disabled — unsigned .hiss scripts blocked.");
}

// ── Lollipop nav/status bar ─────────────────────────────────────────────────────
// Overrides the Lollipop nav bar + status bar's normal auto-hide-while-in-
// an-app behavior — see purr_kernel_set_navbar_always_visible()'s doc
// comment in purr_kernel.h. Off by default (auto-hide), persisted like
// Developer Mode above.

// Lock screen notification privacy — see purr_kernel_lock_hide_notifications().
// Both system UI styles honour the kernel flag, so this one toggle covers the
// Android and iOS lock screens alike.
static void on_lock_notifs_toggle(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    s_lock_hide_notifs = s_lock_hide_notifs ? 0 : 1;
    purr_kernel_set_lock_hide_notifications(s_lock_hide_notifs != 0);
    nvs_save_u8("lock_hide_notifs", s_lock_hide_notifs);
    if (s_lock_notifs_lbl) {
        purr_win_label_set(s_lock_notifs_lbl,
            s_lock_hide_notifs ? "Notifications: Hidden (count only)"
                                : "Notifications: Shown");
    }
    set_customization_status(s_lock_hide_notifs
        ? "Lock screen shows a count; swipe up to reveal."
        : "Lock screen lists notifications.");
}

// ── UI effects + accent colour ──────────────────────────────────────────────
//
// One toggle and one hex field, both of which every translucent surface in both
// System UI styles honours through purr_systemui_fx_bg_opa(). Lives in
// Customization next to Theme and Wallpaper because it is the same kind of
// choice: what the device looks like.
//
// Worth being honest in the UI about the second reason it exists — turning
// effects off measurably speeds the UI up, because a translucent surface forces
// what is beneath it to be redrawn and alpha-blended per pixel instead of being
// skipped. That is why the status line says so rather than presenting this as
// purely cosmetic.
//
// Rebuild note: surfaces read the flag when they are CONSTRUCTED, not on every
// redraw, so already-built panels keep their old look until they are next
// rebuilt. The status text tells the user that instead of pretending otherwise.

static void refresh_effects_labels(void) {
    if (s_effects_lbl) {
        purr_win_label_set(s_effects_lbl,
            s_ui_effects ? "Effects: ON (translucent)" : "Effects: OFF (solid accent)");
    }
    if (s_accent_lbl) {
        char buf[40];
        snprintf(buf, sizeof(buf), "Accent: #%06lX", (unsigned long)s_accent_color);
        purr_win_label_set(s_accent_lbl, buf);
    }
}

static void on_effects_toggle(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    s_ui_effects = s_ui_effects ? 0 : 1;
    purr_kernel_set_ui_effects(s_ui_effects != 0);
    nvs_save_u8("ui_effects", s_ui_effects);
    // Restyle what is already built. The shade panel and nav bar are created
    // once at systemui init and only ever slid around afterwards, so without
    // this the toggle silently does nothing to them until the next reboot.
    // Safe to call straight from here: widget callbacks run on the UI task with
    // the UI lock already held.
    purr_systemui_fx_refresh();
    refresh_effects_labels();
    set_customization_status(s_ui_effects
        ? "Translucency on."
        : "Solid accent. Faster, and easier to read over a wallpaper.");
}

// Strict on purpose. strtoul() would happily accept "zzz" as 0 (silently
// turning the accent black) and "12" as 0x12, so a typo would look like it
// worked. Requiring exactly six hex digits means a bad entry is REPORTED rather
// than quietly applied — the user gets their old colour back and a message.
// Accepts an optional leading '#' and either case, because both are what people
// actually type.
static bool parse_hex_rgb(const char *s, uint32_t *out) {
    if (!s) return false;
    while (*s == ' ' || *s == '#') s++;
    uint32_t v = 0;
    int n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        uint32_t d;
        if      (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else return false;              // any non-hex character rejects outright
        if (n >= 6) return false;       // too long
        v = (v << 4) | d;
    }
    if (n != 6) return false;           // too short (covers the empty string)
    *out = v;
    return true;
}

static void on_accent_apply(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    const char *txt = s_accent_input ? purr_win_textarea_get(s_accent_input) : "";
    uint32_t rgb;
    if (!parse_hex_rgb(txt, &rgb)) {
        set_customization_status("Need 6 hex digits, e.g. 1C1C2E or #FF8800.");
        return;
    }
    s_accent_color = rgb;
    purr_kernel_set_accent_color(s_accent_color);
    char hex[8];
    snprintf(hex, sizeof(hex), "%06lX", (unsigned long)s_accent_color);
    nvs_save_str("accent_color", hex);
    purr_systemui_fx_refresh();   // see on_effects_toggle() for why
    refresh_effects_labels();
    if (s_ui_effects) {
        // Saying "applied" here would be a lie — nothing accent-coloured is on
        // screen while effects are on.
        set_customization_status("Accent saved. Turn Effects OFF to see it.");
    } else {
        set_customization_status("Accent applied.");
    }
}

static void on_navbar_visible_toggle(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    s_navbar_always_visible = s_navbar_always_visible ? 0 : 1;
    purr_kernel_set_navbar_always_visible(s_navbar_always_visible != 0);
    nvs_save_u8("navbar_always_visible", s_navbar_always_visible);
    purr_win_label_set(s_navbar_visible_lbl,
        s_navbar_always_visible ? "Keep bars visible: ON" : "Keep bars visible: OFF");
    set_display_status(s_navbar_always_visible ? "Nav/status bars stay visible in apps."
                                                : "Nav/status bars auto-hide in apps.");
}

// ── Storage ───────────────────────────────────────────────────────────────────

static void on_sd_refresh(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    set_general_status(purr_kernel_sd_available() ? "SD card: present." : "SD card: not mounted.");
}

// ── Reboot ────────────────────────────────────────────────────────────────────

static void on_reboot(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    set_general_status("Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(500));
    purr_kernel_reboot();
}

// ── About ─────────────────────────────────────────────────────────────────────
// Absorbed from the standalone about.c app — Settings is already a persistent
// window, so unlike about.c this is a one-shot fill on open rather than a
// periodic-updater task; a settings screen doesn't need live-ticking
// uptime/RAM. Content mirrors about.c's build_info() minus the box-drawing
// header, kept plain to match this app's other section labels.

static void build_about_text(char *buf, size_t sz) {
    size_t pos = 0;
#define APPEND(fmt, ...) \
    do { pos += snprintf(buf + pos, sz - pos, fmt "\n", ##__VA_ARGS__); } while(0)

    APPEND("PURR OS v%s  (KITT v%s)", PURR_KERNEL_VERSION, KITT_VERSION);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const char *model = "ESP32";
    if      (chip.model == CHIP_ESP32S3) model = "ESP32-S3";
    else if (chip.model == CHIP_ESP32S2) model = "ESP32-S2";
    else if (chip.model == CHIP_ESP32C3) model = "ESP32-C3";
    APPEND("Chip: %s r%d  %d cores", model, chip.revision, chip.cores);

    uint32_t flash_sz = 0;
    esp_flash_get_size(NULL, &flash_sz);
    APPEND("Flash: %lu MB   Free RAM: %lu KB",
           (unsigned long)(flash_sz / (1024 * 1024)),
           (unsigned long)(purr_kernel_free_ram() / 1024));

    uint64_t up_s = purr_kernel_uptime_ms() / 1000ULL;
    APPEND("Uptime: %02luh %02lum %02lus",
           (unsigned long)(up_s / 3600), (unsigned long)((up_s % 3600) / 60),
           (unsigned long)(up_s % 60));

    const catcall_display_t *disp  = purr_kernel_display();
    const catcall_touch_t   *touch = purr_kernel_touch();
    const catcall_input_t   *input = purr_kernel_input();
    const catcall_radio_t   *radio = purr_kernel_radio();
    const catcall_gps_t     *gps   = purr_kernel_gps();
    const catcall_ui_t      *ui    = purr_kernel_ui();
    APPEND("Display:%s Touch:%s Input:%s", disp ? disp->name : "-", touch ? touch->name : "-", input ? input->name : "-");
    APPEND("Radio:%s GPS:%s UI:%s", radio ? radio->name : "-", gps ? gps->name : "-", ui ? ui->name : "-");
    APPEND("SD card: %s", purr_kernel_sd_available() ? "mounted" : "not mounted");

#undef APPEND
}

static void on_open_about(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    if (s_about_win) { purr_win_show(s_about_win); return; }

    s_about_win = purr_win_create("About");
    add_back_button(s_about_win);

    char about_buf[512];
    build_about_text(about_buf, sizeof(about_buf));
    s_about_lbl = purr_win_label(s_about_win, about_buf);

    purr_win_show(s_about_win);
}

// ── Category windows ─────────────────────────────────────────────────────────
// Each built lazily on first tap and cached/reused afterward, same pattern
// as the WiFi/BT windows above — see add_back_button()'s doc comment for why
// each needs its own "< Back" button. Grouping: General (storage/developer/
// system — the catch-all for things that don't fit elsewhere), Display
// (brightness/timeout/bar visibility), Customization (theme/wallpaper),
// Connectivity (WiFi/BT, each still its own nested window). About is its
// own category too (on_open_about(), just above) rather than living inside
// General — it's read-only device info, not a setting to change, and grew
// large enough on its own to earn a dedicated screen.

// General, as three menu sections.
//
// Worth comparing against what this replaced. "Developer Mode" was three
// widgets plus a layout container: a header label, a second label carrying the
// ON/OFF state, and a row wrapping a "Toggle" button. It is now ONE row whose
// VALUE is the state, and tapping it toggles. That is the shape the screen
// always had — the contract simply had no way to say it, so the app spelled it
// out in buttons.
static const char *s_gen_storage[] = { "SD Status" };
static const char *s_gen_dev[]     = { "Developer Mode" };
static const char *s_gen_sys[]     = { "Reboot" };
static const char *s_gen_dev_val[1];

static void general_refresh(void) {
    if (!s_general_menu) return;
    s_gen_dev_val[0] = s_dev_mode ? "ON" : "OFF";
    static purr_menu_section_t secs[3];
    secs[0] = (purr_menu_section_t){ "Storage",   s_gen_storage, NULL,          1 };
    secs[1] = (purr_menu_section_t){ "Developer", s_gen_dev,     s_gen_dev_val, 1 };
    secs[2] = (purr_menu_section_t){ "System",    s_gen_sys,     NULL,          1 };
    purr_win_menu_set_sections(s_general_menu, secs, 3);
}

static void on_general_menu(purr_wid_t w, purr_event_t e, void *user) {
    (void)user;
    if (e != PURR_EVENT_ACTIVATED) return;
    switch (purr_win_menu_get_selected(w)) {
        case 0: on_sd_refresh(0, PURR_EVENT_CLICKED, NULL); break;
        case 1: on_dev_mode_toggle(0, PURR_EVENT_CLICKED, NULL);
                general_refresh();   // the row's value IS the state
                break;
        case 2: on_reboot(0, PURR_EVENT_CLICKED, NULL);     break;
        default: break;
    }
}

static void on_open_general(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    if (s_general_win) { purr_win_show(s_general_win); return; }

    s_general_win = purr_win_create("General");
    add_back_button(s_general_win);

    s_general_menu = purr_win_menu(s_general_win);
    purr_win_menu_on_select(s_general_menu, on_general_menu, NULL);
    general_refresh();

    s_general_status_lbl = purr_win_label(s_general_win, "Ready.");
    purr_win_show(s_general_win);
}

static void on_open_display(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    if (s_display_win) { purr_win_show(s_display_win); return; }

    s_display_win = purr_win_create("Display");
    add_back_button(s_display_win);

    char bright_str[32];
    snprintf(bright_str, sizeof(bright_str), "Brightness: %d%%", (s_brightness * 100) / 255);
    s_brightness_lbl = purr_win_label(s_display_win, bright_str);
    purr_wid_t br = purr_win_row(s_display_win, 4);
    purr_win_button(s_display_win, "Low",  on_bright_low,  NULL);
    purr_win_button(s_display_win, "Mid",  on_bright_mid,  NULL);
    purr_win_button(s_display_win, "High", on_bright_high, NULL);
    purr_win_layout_end(br);

    char timeout_str[32];
    snprintf(timeout_str, sizeof(timeout_str), "Screen timeout: %d min", s_screen_timeout_min);
    s_screen_timeout_lbl = purr_win_label(s_display_win, timeout_str);
    purr_wid_t tor = purr_win_row(s_display_win, 4);
    purr_win_button(s_display_win, "1 min", on_timeout_1, NULL);
    purr_win_button(s_display_win, "3 min", on_timeout_3, NULL);
    purr_win_button(s_display_win, "5 min", on_timeout_5, NULL);
    purr_win_layout_end(tor);

    char navbar_str[32];
    snprintf(navbar_str, sizeof(navbar_str), "Keep bars visible: %s", s_navbar_always_visible ? "ON" : "OFF");
    s_navbar_visible_lbl = purr_win_label(s_display_win, navbar_str);
    purr_wid_t nvr = purr_win_row(s_display_win, 4);
    purr_win_button(s_display_win, "Toggle", on_navbar_visible_toggle, NULL);
    purr_win_layout_end(nvr);

    s_display_status_lbl = purr_win_label(s_display_win, "Ready.");
    purr_win_show(s_display_win);
}

static void on_open_customization(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    if (s_customization_win) { purr_win_show(s_customization_win); return; }

    s_customization_win = purr_win_create("Customization");
    add_back_button(s_customization_win);

    purr_win_label(s_customization_win, "Theme");
    purr_wid_t tr = purr_win_row(s_customization_win, 4);
    purr_win_button(s_customization_win, "WCE Classic", on_theme_wce,  NULL);
    purr_win_button(s_customization_win, "Dark",        on_theme_dark, NULL);
    purr_win_layout_end(tr);

    char theme_str[40];
    snprintf(theme_str, sizeof(theme_str), "Active: %s", s_theme);
    purr_win_label(s_customization_win, theme_str);

    purr_win_label(s_customization_win, "Wallpaper");
    s_wallpaper_list = purr_win_list(s_customization_win, 90, 30);
    purr_win_list_on_select(s_wallpaper_list, on_wallpaper_select, NULL);
    refresh_wallpaper_list();

    // Lock screen notification privacy. Lives here rather than under Display
    // because it is a "what does my device show about me" choice, not a
    // panel/backlight one — and it sits next to Wallpaper, which is the other
    // setting that changes what the lock screen looks like.
    purr_win_label(s_customization_win, "Lock Screen");
    s_lock_notifs_lbl = purr_win_label(s_customization_win,
        s_lock_hide_notifs ? "Notifications: Hidden (count only)"
                            : "Notifications: Shown");
    purr_win_button(s_customization_win, "Toggle Notifications", on_lock_notifs_toggle, NULL);

    // Effects + accent. Sits under Lock Screen because turning effects off is
    // most visible there — the lock screen scrim over the wallpaper is the
    // largest translucent surface in the system.
    purr_win_label(s_customization_win, "Effects");
    s_effects_lbl = purr_win_label(s_customization_win, "");
    purr_win_button(s_customization_win, "Toggle Effects", on_effects_toggle, NULL);

    s_accent_lbl = purr_win_label(s_customization_win, "");
    purr_win_label(s_customization_win, "Accent colour (hex, e.g. 1C1C2E)");
    s_accent_input = purr_win_textarea(s_customization_win, 60, 12);
    purr_win_button(s_customization_win, "Apply Accent", on_accent_apply, NULL);

    // Both labels are created empty above and filled here, so the initial text
    // and every later update go through exactly one formatting path.
    refresh_effects_labels();

    s_customization_status_lbl = purr_win_label(s_customization_win, "Ready.");
    purr_win_show(s_customization_win);
}

// ── Mesh backend switch ──────────────────────────────────────────────────
// Shares the exact same preference + live-switch mechanism as MSN's own
// chooser screen (msn.c) — one source of truth
// (purr_kernel_mesh_backend_switch(), see its own comment in purr_kernel.c
// for how the mutual-exclusion guard cooperates with a no-reboot switch).

static void update_mesh_backend_status(void) {
    if (!s_mesh_backend_status_lbl) return;
    bool mt_active = purr_kernel_get_module("meshtastic") != NULL;
    bool mc_active = purr_kernel_get_module("meshcore") != NULL;
    const char *active = mt_active ? "Meshtastic" : (mc_active ? "MeshCore" : "none");
    char buf[48];
    snprintf(buf, sizeof(buf), "Mesh backend: %s", active);
    purr_win_label_set(s_mesh_backend_status_lbl, buf);
}

static void on_mesh_switch_confirm(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    int rc = purr_kernel_mesh_backend_switch(s_mesh_switch_target);

    if (s_mesh_switch_confirm_win) {
        purr_win_destroy(s_mesh_switch_confirm_win);
        s_mesh_switch_confirm_win = 0;
    }
    update_mesh_backend_status();
    if (rc != PURR_MODCTL_OK && rc != PURR_MODCTL_ERR_ALREADY && s_mesh_backend_status_lbl) {
        purr_win_label_set(s_mesh_backend_status_lbl, "Mesh backend: switch failed");
    }
}

static void on_mesh_switch_cancel(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    if (s_mesh_switch_confirm_win) {
        purr_win_destroy(s_mesh_switch_confirm_win);
        s_mesh_switch_confirm_win = 0;
    }
}

static void open_mesh_switch_confirm(purr_mesh_backend_t target, const char *name) {
    s_mesh_switch_target = target;
    if (s_mesh_switch_confirm_win) { purr_win_show(s_mesh_switch_confirm_win); return; }

    char msg[64];
    snprintf(msg, sizeof(msg), "Switch to %s?", name);

    s_mesh_switch_confirm_win = purr_win_create("Switch Backend");
    purr_win_label(s_mesh_switch_confirm_win, msg);
    purr_wid_t row = purr_win_row(s_mesh_switch_confirm_win, 4);
    purr_win_button(s_mesh_switch_confirm_win, "Switch", on_mesh_switch_confirm, NULL);
    purr_win_button(s_mesh_switch_confirm_win, "Cancel", on_mesh_switch_cancel,  NULL);
    purr_win_layout_end(row);

    purr_win_show(s_mesh_switch_confirm_win);
}

static void on_mesh_switch_meshtastic(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    if (purr_kernel_get_module("meshtastic")) return;   // already active
    open_mesh_switch_confirm(PURR_MESH_BACKEND_MESHTASTIC, "Meshtastic");
}

static void on_mesh_switch_meshcore(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    if (purr_kernel_get_module("meshcore")) return;   // already active
    open_mesh_switch_confirm(PURR_MESH_BACKEND_MESHCORE, "MeshCore");
}

static void on_open_connectivity(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    if (s_connectivity_win) { purr_win_show(s_connectivity_win); update_mesh_backend_status(); return; }

    s_connectivity_win = purr_win_create("Connectivity");
    add_back_button(s_connectivity_win);

    purr_win_label(s_connectivity_win, "Network");
    purr_wid_t nr = purr_win_row(s_connectivity_win, 4);
    purr_win_button(s_connectivity_win, "WiFi Settings",      on_wifi_settings_open, NULL);
#ifdef CONFIG_BT_NIMBLE_ENABLED
    purr_win_button(s_connectivity_win, "Bluetooth Settings", on_bt_settings_open,   NULL);
#endif
    purr_win_layout_end(nr);

    purr_win_label(s_connectivity_win, "Mesh");
    s_mesh_backend_status_lbl = purr_win_label(s_connectivity_win, "Mesh backend: ...");
    purr_wid_t mr = purr_win_row(s_connectivity_win, 4);
    purr_win_button(s_connectivity_win, "Use Meshtastic", on_mesh_switch_meshtastic, NULL);
    purr_win_button(s_connectivity_win, "Use MeshCore",   on_mesh_switch_meshcore,   NULL);
    purr_win_layout_end(mr);
    update_mesh_backend_status();

    purr_win_show(s_connectivity_win);
}

// ── Updates ───────────────────────────────────────────────────────────────────

static void set_updates_status(const char *msg) {
    if (s_updates_status_lbl) purr_win_label_set(s_updates_status_lbl, msg);
}

// Re-reads ota_mgr's current status/progress into the label — same manual
// "Refresh" pattern taskmgr/services use rather than a polling timer, since
// nothing in purr_win.h offers one and ota_mgr_check()/apply() already run on
// their own background task regardless.
static void refresh_updates_status(void) {
    char buf[96];
    switch (ota_mgr_status()) {
        case OTA_MGR_IDLE:
            snprintf(buf, sizeof(buf), "Current version: %s", ota_mgr_current_version());
            break;
        case OTA_MGR_CHECKING:
            snprintf(buf, sizeof(buf), "Checking for updates...");
            break;
        case OTA_MGR_UP_TO_DATE:
            snprintf(buf, sizeof(buf), "Up to date (v%s).", ota_mgr_current_version());
            break;
        case OTA_MGR_AVAILABLE:
            snprintf(buf, sizeof(buf), "Update available: v%s (current v%s)",
                     ota_mgr_available_version(), ota_mgr_current_version());
            break;
        case OTA_MGR_DOWNLOADING:
            snprintf(buf, sizeof(buf), "Downloading v%s... %d%%",
                     ota_mgr_available_version(), ota_mgr_progress_percent());
            break;
        case OTA_MGR_VERIFYING:
            snprintf(buf, sizeof(buf), "Verifying checksum...");
            break;
        case OTA_MGR_READY_TO_REBOOT:
            snprintf(buf, sizeof(buf), "v%s staged — reboot to apply.", ota_mgr_available_version());
            break;
        case OTA_MGR_FAILED:
            snprintf(buf, sizeof(buf), "Failed: %s", ota_mgr_error());
            break;
        default:
            snprintf(buf, sizeof(buf), "?");
            break;
    }
    set_updates_status(buf);
}

static void on_updates_refresh(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    refresh_updates_status();
}

static void ota_check_task(void *arg) {
    (void)arg;
    ota_mgr_check();
    s_ota_busy = false;
    vTaskDeleteWithCaps(NULL);
}

static void ota_apply_task(void *arg) {
    (void)arg;
    ota_mgr_apply();
    s_ota_busy = false;
    vTaskDeleteWithCaps(NULL);
}

static void ota_apply_sd_task(void *arg) {
    (void)arg;
    ota_mgr_apply_from_sd(OTA_MGR_SD_DEFAULT_PATH);
    s_ota_busy = false;
    vTaskDeleteWithCaps(NULL);
}

// PSRAM-backed stack and a size on par with bt_scan_task's own reasoning —
// TLS handshakes (esp_https_ota goes through esp-tls/mbedtls) are the
// deepest, most stack-hungry part of this call, more so than BLE scanning,
// so this task gets a larger allocation than bt_scan_task's 4096.
#define OTA_TASK_STACK 8192

static void on_updates_check(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    if (!ota_mgr_is_supported()) { set_updates_status("OTA not available on this build."); return; }
    if (ota_mgr_manifest_url()[0] == '\0') { set_updates_status("Set an update URL first."); return; }
    if (s_ota_busy) return;
    s_ota_busy = true;
    set_updates_status("Checking for updates...");
    TaskHandle_t task = NULL;
    BaseType_t ok = xTaskCreateWithCaps(ota_check_task, "ota_check", OTA_TASK_STACK, NULL, 3, &task, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) { s_ota_busy = false; set_updates_status("Could not start update check."); }
}

static void on_updates_download(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    if (ota_mgr_status() != OTA_MGR_AVAILABLE) { set_updates_status("Check for updates first."); return; }
    if (s_ota_busy) return;
    s_ota_busy = true;
    set_updates_status("Starting download...");
    TaskHandle_t task = NULL;
    BaseType_t ok = xTaskCreateWithCaps(ota_apply_task, "ota_apply", OTA_TASK_STACK, NULL, 3, &task, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) { s_ota_busy = false; set_updates_status("Could not start download."); }
}

// No "check for update available" precondition here, unlike
// on_updates_download() above — copying a file to OTA_MGR_SD_DEFAULT_PATH IS
// the user's decision to install it (see ota_mgr_apply_from_sd()'s own doc
// comment on why there's no version gate on this path).
static void on_updates_install_sd(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    if (!purr_kernel_sd_available()) { set_updates_status("No SD card mounted."); return; }
    if (s_ota_busy) return;
    s_ota_busy = true;
    set_updates_status("Reading " OTA_MGR_SD_DEFAULT_PATH "...");
    TaskHandle_t task = NULL;
    BaseType_t ok = xTaskCreateWithCaps(ota_apply_sd_task, "ota_apply_sd", OTA_TASK_STACK, NULL, 3, &task, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) { s_ota_busy = false; set_updates_status("Could not start SD install."); }
}

static void on_updates_reboot(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    if (ota_mgr_status() != OTA_MGR_READY_TO_REBOOT) return;
    set_updates_status("Rebooting...");
    purr_kernel_reboot();
}

static void close_ota_url_dialog(void) {
    if (s_ota_url_dlg_win) purr_win_destroy(s_ota_url_dlg_win);
    s_ota_url_dlg_win = 0;
    s_ota_url_dlg_input = 0;
}

static void on_ota_url_dlg_cancel(purr_wid_t w, purr_event_t e, void *u) { (void)w;(void)e;(void)u; close_ota_url_dialog(); }

static void on_ota_url_dlg_save(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    const char *url = s_ota_url_dlg_input ? purr_win_textarea_get(s_ota_url_dlg_input) : "";
    if (url && url[0]) ota_mgr_set_manifest_url(url);
    close_ota_url_dialog();
    refresh_updates_status();
}

static void on_updates_set_url(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    if (s_ota_url_dlg_win) { purr_win_show(s_ota_url_dlg_win); return; }

    s_ota_url_dlg_win = purr_win_create("Update URL");
    purr_win_label(s_ota_url_dlg_win, "Manifest URL (http/https):");
    s_ota_url_dlg_input = purr_win_textarea(s_ota_url_dlg_win, 90, 20);
    if (ota_mgr_manifest_url()[0]) purr_win_textarea_set(s_ota_url_dlg_input, ota_mgr_manifest_url());

    purr_wid_t row = purr_win_row(s_ota_url_dlg_win, 4);
    purr_win_button(s_ota_url_dlg_win, "Save",   on_ota_url_dlg_save,   NULL);
    purr_win_button(s_ota_url_dlg_win, "Cancel", on_ota_url_dlg_cancel, NULL);
    purr_win_layout_end(row);

    purr_win_textarea_focus(s_ota_url_dlg_input);
    purr_win_show(s_ota_url_dlg_win);
    purr_win_keyboard_show(s_ota_url_dlg_win, s_ota_url_dlg_input);
}

static void on_open_updates(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    if (s_updates_win) { purr_win_show(s_updates_win); refresh_updates_status(); return; }

    s_updates_win = purr_win_create("Updates");
    add_back_button(s_updates_win);

    if (!ota_mgr_is_supported()) {
        // No second OTA slot on this device's partition table (device.pcat
        // [device] ota is unset/false) — ota_mgr still compiles in (same
        // "always REQUIRES'd, runtime-optional" shape as wifi_mgr/bt_mgr/
        // meshtastic elsewhere in this file), it just never finds a target
        // partition. Say so plainly rather than showing controls that would
        // only ever fail.
        purr_win_label(s_updates_win, "OTA updates are not available on this build.");
        purr_win_show(s_updates_win);
        return;
    }

    s_updates_status_lbl = purr_win_label(s_updates_win, "...");

    purr_wid_t row1 = purr_win_row(s_updates_win, 4);
    purr_win_button(s_updates_win, "Check for Updates", on_updates_check, NULL);
    purr_win_button(s_updates_win, "Set URL",           on_updates_set_url, NULL);
    purr_win_layout_end(row1);

    purr_wid_t row2 = purr_win_row(s_updates_win, 4);
    purr_win_button(s_updates_win, "Download & Verify", on_updates_download, NULL);
    purr_win_button(s_updates_win, "Reboot to Apply",   on_updates_reboot,   NULL);
    purr_win_layout_end(row2);

    purr_win_label(s_updates_win, "From SD card (" OTA_MGR_SD_DEFAULT_PATH "):");
    purr_win_button(s_updates_win, "Install from SD", on_updates_install_sd, NULL);

    purr_win_button(s_updates_win, "Refresh", on_updates_refresh, NULL);

    refresh_updates_status();
    purr_win_show(s_updates_win);
}

// ── Category picker nav ──────────────────────────────────────────────────
// Same tile-grid-with-list-fallback shape MSN's own Home screen uses
// (msn.c's build_home_screen_nav()) — reused here so every multi-screen
// system app reads as one consistent design language instead of Settings
// keeping its older plain-button-row picker. Purely a top-level nav swap:
// each category's own sub-window (on_open_general() etc.) is untouched.

#define CAT_COUNT 6
static const char *s_category_labels[CAT_COUNT] = { "General", "Display", "Customization", "Connectivity", "Updates", "About" };

static void on_cat_menu(purr_wid_t w, purr_event_t e, void *user) {
    (void)user;
    if (e != PURR_EVENT_ACTIVATED) return;
    switch (purr_win_menu_get_selected(w)) {
        case 0: on_open_general(0, PURR_EVENT_CLICKED, NULL);       break;
        case 1: on_open_display(0, PURR_EVENT_CLICKED, NULL);       break;
        case 2: on_open_customization(0, PURR_EVENT_CLICKED, NULL); break;
        case 3: on_open_connectivity(0, PURR_EVENT_CLICKED, NULL);  break;
        case 4: on_open_updates(0, PURR_EVENT_CLICKED, NULL);       break;
        case 5: on_open_about(0, PURR_EVENT_CLICKED, NULL);         break;
        default: break;
    }
}

// One menu, one code path — replacing a tile grid, a hand-written list
// fallback for backends without one, and two #ifdef'd symbol tables.
static void build_category_nav(void) {
    static const purr_menu_section_t sec = {
        .header = NULL, .items = s_category_labels, .values = NULL, .count = CAT_COUNT,
    };
    s_cat_menu = purr_win_menu(s_win);
    purr_win_menu_set_sections(s_cat_menu, &sec, 1);
    purr_win_menu_on_select(s_cat_menu, on_cat_menu, NULL);
}

// ── Build UI ──────────────────────────────────────────────────────────────────

static int settings_init(void) {
    nvs_load();
    purr_kernel_set_dev_mode(s_dev_mode != 0);
    purr_kernel_set_navbar_always_visible(s_navbar_always_visible != 0);

    // Top-level window is just the category picker — each button opens its
    // own lazily-built, cached sub-window (see on_open_*() above). Keeps
    // this window's own widget count tiny (MiniWin's control/message-queue
    // budget is finite, same rationale the old WiFi/BT split already used)
    // and leaves room for more categories later without this screen growing
    // unbounded.
    s_win = purr_win_create("Settings");

    purr_win_label(s_win, "Settings");
    build_category_nav();

    purr_win_show(s_win);
    return 0;
}

static void settings_deinit(void) {
    close_wifi_dialog();
    if (s_wifi_win) { purr_win_destroy(s_wifi_win); s_wifi_win = 0; s_wifi_status_lbl = 0; s_wifi_list = 0; }
#ifdef CONFIG_BT_NIMBLE_ENABLED
    if (s_bt_scanning && s_bt_scan_done) xSemaphoreTake(s_bt_scan_done, pdMS_TO_TICKS(7500));
    if (s_bt_win)   { purr_win_destroy(s_bt_win);   s_bt_win   = 0; s_bt_status_lbl   = 0; s_bt_list   = 0; }
#endif
    if (s_general_win)       { purr_win_destroy(s_general_win);       s_general_win       = 0; s_general_status_lbl       = 0; }
    if (s_display_win)       { purr_win_destroy(s_display_win);       s_display_win       = 0; s_display_status_lbl       = 0; }
    if (s_customization_win) { purr_win_destroy(s_customization_win); s_customization_win = 0; s_customization_status_lbl = 0; }
    if (s_connectivity_win)  { purr_win_destroy(s_connectivity_win);  s_connectivity_win  = 0; s_mesh_backend_status_lbl = 0; }
    // No wait-on-semaphore here the way s_bt_scanning above needs one:
    // ota_check_task()/ota_apply_task() only ever touch ota_mgr's own static
    // state (via ota_mgr_check()/ota_mgr_apply()), never a widget directly —
    // reading that state back into s_updates_status_lbl only happens from
    // the UI-thread refresh button. A download in flight is left running
    // (ota_mgr is a persistent system module, not owned by this app's
    // lifetime) rather than aborted just because Settings was closed.
    close_ota_url_dialog();
    if (s_updates_win) { purr_win_destroy(s_updates_win); s_updates_win = 0; s_updates_status_lbl = 0; }
    if (s_about_win)         { purr_win_destroy(s_about_win);         s_about_win         = 0; s_about_lbl = 0; }
    if (s_mesh_switch_confirm_win) { purr_win_destroy(s_mesh_switch_confirm_win); s_mesh_switch_confirm_win = 0; }
    purr_win_destroy(s_win);
    s_win = 0; s_cat_menu = 0; s_general_menu = 0;
}

// ── Module header ─────────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(settings) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_APP,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "settings",
    .version           = "1.0.1",
    .kernel_min        = "0.11.1",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = settings_init,
    .deinit            = settings_deinit,
};
