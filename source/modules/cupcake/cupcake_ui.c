// cupcake_ui.c — "Lollipop": an Android 5-7 era companion UI built on top of
// Cupcake's existing status bar, HAL, and window management. Two separate
// bottom rows, not one: a persistent 3-button nav bar (Back/Home/Recents,
// see build_lp_navbar()) pinned to lv_layer_top() so it renders over every
// app window, not just the home screen — and, sitting just above it, a
// home-screen-only favorites row (2 apps, an all-apps launcher button, 2
// more apps — see build_lp_home_dock()) that disappears along with the rest
// of the home screen once an app opens, same as Cupcake's old dock did.
// Home leaves the foreground app running and hidden; Back closes it
// outright (no in-app back-navigation stack exists to unwind first, so
// closing is the only thing left for it to do). Recents opens a staggered
// card-carousel overview of running apps (see lp_recents_open() near the
// Running Apps panel) — Back/Home both dismiss it first if it's open,
// before falling through to their normal foreground-app behavior.
//
// Status bar + drag-down notification panel below is forked near-verbatim
// from cardstack_ui.c (renamed with a ck_/CK_ prefix) — confirmed while
// planning this that it depends only on LVGL, lv_layer_top(), and generic
// purr_kernel_*() accessors, nothing card-stack-specific. Not ported: the
// home card's baked "PURR OS DP1" strip — this launcher's own home screen
// doesn't need a redundant one. Kept as-is for Lollipop per explicit
// instruction — the status bar carries over unchanged, only the bottom
// navigation is new.

#include "cupcake.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/purr_win.h"
#include "../app_manager/app_manager.h"
#include "../../assets/icons/blackpurr_icons.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

// Lock screen's node-count line — only meaningful when meshtastic is
// actually compiled in (CONFIG_PURR_FEATURE_MESHTASTIC); guarded the same
// way lua_runtime.c's/meshdiag.c's radio access is, since this module
// builds for every device regardless of which mesh backend (or none) is
// active. MeshCore-backed devices don't get a node count on the lock
// screen yet — same scope limit, add the mc_manager_node_count()
// equivalent later if needed.
#ifdef CONFIG_PURR_FEATURE_MESHTASTIC
#include "../meshtastic/meshtastic.h"
#endif

static const char *TAG = "cupcake_ui";

// CUPCAKE_STATUS_PEEK_H and CUPCAKE_NAVBAR_H are defined in cupcake.h —
// cupcake_win.c needs both too, to keep app windows (and their close
// buttons) clear of the status hotzone and nav bar.
#define CUPCAKE_STATUS_EXPANDED_H 220
#define MAX_LAUNCHER_TILES 64
// Small square + a name label underneath — most apps share the same
// fallback icon (icon_for_app()'s tools-icon default), so without a label
// the "All Apps" grid is unreadable beyond the handful of apps with a real
// bundled icon. Width stays the old icon-only 64px; height grows to fit
// one line of label text below the icon.
#define LP_LAUNCHER_TILE_W  64
#define LP_LAUNCHER_TILE_H  84
#define LP_NAVBTN_SIZE     32

// Home-screen-only favorites row (2 apps, apps-launcher button, 2 more apps)
// pinned directly above the persistent nav bar — NOT part of the nav bar
// itself, so it's covered/hidden along with the rest of the home screen the
// moment an app opens, same as Cupcake's old dock was.
#define MAX_HOME_DOCK_FAV       4
#define LP_HOME_DOCK_H          48
#define LP_HOME_DOCK_FAV_SIZE   36
#define LP_HOME_DOCK_CENTER_SIZE 44

// All bundled icon assets are 48x48 source images — these are the actual
// on-screen sizes we want, applied via lv_img_set_zoom() (256 = 100%),
// since the container size alone doesn't rescale an lv_img's source bitmap.
#define ICON_PX_LAUNCHER    40
#define ICON_PX_HOME_DOCK   24
#define ICON_PX_HOME_DOCK_CENTER 28
#define ICON_ZOOM(px) (uint16_t)(((px) * 256) / 48)

// ── Icon lookup ──────────────────────────────────────────────────────────────

static const lv_img_dsc_t *icon_for_app(const char *name)
{
    static const struct { const char *app_name; const lv_img_dsc_t *icon; } map[] = {
        { "settings",   &bp_icon_settings_48   },
        { "about",      &bp_icon_about_48      },
        { "terminal",   &bp_icon_terminal_48   },
        { "fileman",    &bp_icon_fileman_48    },
        { "calculator", &bp_icon_calculator_48 },
        { "hwtest",     &bp_icon_tools_48      },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(name, map[i].app_name) == 0) return map[i].icon;
    }
    return &bp_icon_tools_48; // fallback for anything unmatched
}

// Deterministic per-app tint — same technique as cardstack_ui.c's
// app_tint_color(), renamed for this fork.
static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u; // FNV-1a
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static lv_color_t cupcake_tint_color(const char *name, uint8_t base)
{
    uint32_t h = hash_str(name);
    uint8_t r = (uint8_t)(base + ((h >> 0)  & 0x1F));
    uint8_t g = (uint8_t)(base + ((h >> 8)  & 0x1F));
    uint8_t b = (uint8_t)(base + ((h >> 16) & 0x1F));
    return lv_color_make(r, g, b);
}

// ── Tile builders ────────────────────────────────────────────────────────────

// Which app_manager index is currently in the foreground (its window shown,
// not minimized) — -1 means the home screen is showing, nothing open. No
// public API exists to ask a window's own visibility state (purr_win.h has
// no purr_win_is_visible()), so this is tracked locally here instead,
// updated at every place an app window gets shown/restored (launch_or_
// restore() below, and ck_taskmgr_open_cb()) and cleared by the nav bar's
// Home/Back handlers when they hide or close it. Used by those same
// handlers to know which app "leave without closing" or "close" applies to.
static int s_lp_foreground_idx = -1;

// ── Nav bar auto-hide ────────────────────────────────────────────────────────
// Visible permanently on the home screen; auto-hides the instant an app
// opens, reachable again only via an upward swipe on the (always-present,
// invisible) hotzone at the bottom edge — see build_lp_navbar_hotzone().
// A swipe-revealed bar re-hides itself after LP_NAVBAR_REVEAL_MS if left
// alone; deadline of 0 means "no pending auto-hide" (home screen, or an
// app just opened and hasn't been swiped-up on yet).
#define LP_NAVBAR_REVEAL_MS 5000
static lv_obj_t *s_lp_navbar;
static uint64_t  s_lp_navbar_hide_deadline_ms = 0;

// temporary=true (swipe-up reveal while in an app) arms the auto-hide
// countdown; temporary=false (returning to the home screen) leaves the bar
// up indefinitely instead.
static void lp_show_navbar(bool temporary)
{
    if (s_lp_navbar) lv_obj_clear_flag(s_lp_navbar, LV_OBJ_FLAG_HIDDEN);
    s_lp_navbar_hide_deadline_ms = temporary ? (purr_kernel_uptime_ms() + LP_NAVBAR_REVEAL_MS) : 0;
}

static void lp_hide_navbar(void)
{
    // Settings' "keep bars visible" toggle — see purr_kernel.h's doc
    // comment. Guarding here (rather than at every call site) covers the
    // deadline-tick auto-hide AND every explicit hide-on-app-open call
    // uniformly, and self-heals: flipping the toggle on while an app is
    // already foregrounded just means the next hide attempt silently no-ops
    // instead of requiring an extra "re-show now" path.
    if (purr_kernel_navbar_always_visible()) { lp_show_navbar(false); return; }
    if (s_lp_navbar) lv_obj_add_flag(s_lp_navbar, LV_OBJ_FLAG_HIDDEN);
    s_lp_navbar_hide_deadline_ms = 0;
}

// ── Status bar overlay ───────────────────────────────────────────────────────
// Same permanent-on-home-screen / auto-hide-while-in-an-app treatment as the
// nav bar above, but the reveal gesture is NOT a new swipe zone — it's
// whichever existing top hotzone the user is already touching to open the
// Notifications/Running-Apps drag-down panel (see ck_hotzone_*_pressed_cb()
// and ck_panel_drag_event_cb(), further down). Deliberately NOT reusing the
// nav bar's "own invisible hotzone, always present" trick here: app windows
// already start below CUPCAKE_STATUS_PEEK_H specifically so their title-bar
// close/minimize buttons stay clear of the status hotzones' touch region
// (see cupcake_win.c's mw_win_create()-equivalent comment) — moving that
// edge up to reclaim the full pixel height would put those buttons right
// back under a touch zone that has to stay live to catch a reveal swipe,
// breaking them. Piggybacking on the panel-open gesture instead means the
// status icons only ever need to become visible when that same hotzone
// region is already being interacted with for an unrelated reason, so
// nothing about window layout has to change.
#define LP_STATUS_REVEAL_MS 5000
static uint64_t s_lp_status_hide_deadline_ms = 0;
static void lp_show_status(bool temporary);
static void lp_hide_status(void);

// Recents card carousel — implementation lives further down (near the
// Running Apps panel it shares data/tint logic with), forward-declared here
// so the nav bar's Back/Home/Recents handlers just below can reach it.
static void lp_recents_open(void);
static void lp_recents_close(void);
static bool lp_recents_is_open(void);

// If the app is already running (and has a tracked window — see
// app_manager.c's window-created hook), tapping its icon again should
// restore that window instead of doing nothing: app_manager_launch_idx()
// on an already-RUNNING app just no-ops (see its re-launch guard), so
// without this an already-open-but-minimized app's icon would appear dead.
static void launch_or_restore(int idx)
{
    const app_entry_t *app = app_manager_get(idx);
    if (app && app->state == APP_STATE_RUNNING && app->window) {
        purr_win_show(app->window);
    } else {
        app_manager_launch_idx(idx);
    }
    s_lp_foreground_idx = idx;
    lp_hide_navbar();
    lp_hide_status();
}

static lv_obj_t *s_lp_launcher;

// Closes the launcher first, then launches/restores — matches the old
// drawer's behavior (tile tap dismisses the overlay, not just opens the app).
static void lp_launcher_tile_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "launching app idx=%d (from Lollipop launcher)", idx);
    if (s_lp_launcher) lv_obj_add_flag(s_lp_launcher, LV_OBJ_FLAG_HIDDEN);
    launch_or_restore(idx);
}

static void lp_launcher_close_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_lp_launcher) lv_obj_add_flag(s_lp_launcher, LV_OBJ_FLAG_HIDDEN);
}

// Opens the small-square all-apps launcher — the button that triggers this
// used to live in the nav bar, now lives in the home screen's own dock row
// instead (see build_lp_home_dock()), but the launcher itself is unchanged.
static void lp_apps_launcher_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_lp_launcher) {
        lv_obj_clear_flag(s_lp_launcher, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_lp_launcher);
    }
}

// Home: "leave the app without closing it" — hides the foreground app's
// window (same as its own Minimize button) and returns to the home screen.
// s_lp_foreground_idx < 0 means the home screen is already showing; no-op.
static void lp_navbar_home_click_cb(lv_event_t *e)
{
    (void)e;
    if (lp_recents_is_open()) lp_recents_close();
    if (s_lp_foreground_idx < 0) return;
    const app_entry_t *app = app_manager_get(s_lp_foreground_idx);
    if (app && app->window) purr_win_hide(app->window);
    s_lp_foreground_idx = -1;
    lp_show_navbar(false);
    lp_show_status(false);
}

// Back: no in-app back-navigation stack exists yet, so the only thing left
// to do is close the foreground app outright (distinct from Home, which
// only hides it) — app_manager_stop() is the same call the Running Apps
// panel's own Kill button already uses. No-op on the home screen already.
static void lp_navbar_back_click_cb(lv_event_t *e)
{
    (void)e;
    // Recents counts as "a screen" for Back to dismiss first, same as
    // Android's own back stack would — matches Home's identical guard above.
    if (lp_recents_is_open()) { lp_recents_close(); return; }
    if (s_lp_foreground_idx < 0) return;
    purr_kernel_ui_breadcrumb("navbar:back_stop_app");
    app_manager_stop(s_lp_foreground_idx);
    purr_kernel_ui_breadcrumb("navbar:back_post_stop");
    s_lp_foreground_idx = -1;
    lp_show_navbar(false);
    lp_show_status(false);
    purr_kernel_ui_breadcrumb("navbar:back_done");
}

static void lp_navbar_recents_click_cb(lv_event_t *e)
{
    (void)e;
    lp_recents_open();
}

// Icon + name-label tile, used by the Lollipop launcher's scrollable grid —
// see LP_LAUNCHER_TILE_W/_H's comment for why the label was added back.
static void build_lp_launcher_tile(lv_obj_t *parent, int app_idx, const char *name, lv_event_cb_t click_cb)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, LP_LAUNCHER_TILE_W, LP_LAUNCHER_TILE_H);
    lv_obj_set_style_radius(tile, 10, 0);
    lv_obj_set_style_bg_color(tile, cupcake_tint_color(name, 0x18), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_60, 0); // lets a wallpaper show through behind icons
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tile, click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)app_idx);

    lv_obj_t *icon = lv_img_create(tile);
    lv_img_set_src(icon, icon_for_app(name));
    lv_img_set_zoom(icon, ICON_ZOOM(ICON_PX_LAUNCHER));
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(tile);
    lv_label_set_text(lbl, name);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl, LP_LAUNCHER_TILE_W - 6);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
}

// Round button with a centered LVGL built-in symbol glyph (LV_SYMBOL_*),
// used by the nav bar's three buttons — no custom bitmap icon assets exist
// yet for "back"/"apps grid"/"recents" concepts, unlike actual app icons.
static lv_obj_t *build_lp_navbtn(lv_obj_t *parent, const char *symbol, lv_event_cb_t click_cb)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LP_NAVBTN_SIZE, LP_NAVBTN_SIZE);
    lv_obj_set_style_radius(btn, (lv_coord_t)(LP_NAVBTN_SIZE / 2), 0);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x30, 0x30, 0x30), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

// Round button with a centered app-icon bitmap — used by the home dock row
// (favorites + the all-apps launcher button), which needs real app icons
// rather than the nav bar's LVGL symbol glyphs.
static lv_obj_t *build_lp_dock_icon(lv_obj_t *parent, int app_idx, const lv_img_dsc_t *icon,
                                     lv_event_cb_t click_cb, lv_color_t bg,
                                     lv_coord_t size, lv_coord_t icon_px)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, (lv_coord_t)(size / 2), 0);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_60, 0); // lets a wallpaper show through behind icons
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)app_idx);

    lv_obj_t *img = lv_img_create(btn);
    lv_img_set_src(img, icon);
    lv_img_set_zoom(img, ICON_ZOOM(icon_px));
    lv_obj_center(img);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

static void lp_dock_fav_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "launching app idx=%d (from home dock)", idx);
    launch_or_restore(idx);
}

// ── Wallpaper ────────────────────────────────────────────────────────────────
// "default" (the gradient below) or an SD path chosen in Settings, persisted
// in NVS under the same "purr_settings" namespace settings.c already uses.
// Raw RGB565 files only — no on-device image decoder, matching how the
// icon-generation pipeline keeps decoding on the PC side. Falls back to the
// gradient on any missing/malformed file rather than failing to boot.

#define WALLPAPER_PATH_LEN 128

static lv_img_dsc_t s_wallpaper_img;

static bool load_wallpaper_from_sd(const char *path, lv_img_dsc_t *out, uint16_t w, uint16_t h)
{
    size_t expect = (size_t)w * (size_t)h * 2;
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    uint8_t *buf = heap_caps_malloc(expect, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = heap_caps_malloc(expect, MALLOC_CAP_8BIT);
    if (!buf) { fclose(f); return false; }

    size_t n = fread(buf, 1, expect, f);
    fclose(f);
    if (n != expect) { heap_caps_free(buf); return false; }

    memset(out, 0, sizeof(*out));
    out->header.cf = LV_IMG_CF_TRUE_COLOR;
    out->header.w  = w;
    out->header.h  = h;
    out->data_size = expect;
    out->data      = buf;
    return true;
}

// Baked into SPIFFS at build time — purrstrap.py's build_flash_image()
// stages every source/assets/wallpapers/*.rgb565 file into /flash/
// wallpapers/ unconditionally (not gated by [flash] like modules/apps,
// since it's a plain asset, not a module). load_wallpaper_from_sd() is
// generic fopen/fread underneath, so it works unmodified against this
// SPIFFS path exactly the same way it does against an SD card one.
#define LP_BUNDLED_WALLPAPER_PATH "/flash/wallpapers/wallpaper.rgb565"

static bool load_wallpaper_choice(lv_img_dsc_t *out, uint16_t w, uint16_t h)
{
    char path[WALLPAPER_PATH_LEN] = "default";
    nvs_handle_t hnd;
    if (nvs_open("purr_settings", NVS_READONLY, &hnd) == ESP_OK) {
        size_t len = sizeof(path);
        nvs_get_str(hnd, "wallpaper", path, &len);
        nvs_close(hnd);
    }
    if (strcmp(path, "default") == 0) {
        // Try the bundled wallpaper first — falls through to the plain
        // gradient (via this returning false) if it's missing, e.g. a
        // build with no wallpaper.rgb565 present at all.
        return load_wallpaper_from_sd(LP_BUNDLED_WALLPAPER_PATH, out, w, h);
    }
    return load_wallpaper_from_sd(path, out, w, h);
}

// ── Home screen ──────────────────────────────────────────────────────────────
// No dock anymore — shortcuts live in the Lollipop nav bar's launcher now
// (see below), so the home screen is wallpaper-only.

static lv_obj_t *s_home_screen;

// Favorites row pinned to the bottom of the home screen's own content area
// (already clear of both the status bar and nav bar — see cupcake_ui_init()'s
// body_h) — 2 apps, the all-apps launcher button, 2 more apps, "first N in
// registry order" same as Cupcake's old dock (no separate favorites concept
// exists yet). A child of s_home_screen, NOT lv_layer_top() like the nav
// bar — this row is meant to disappear along with the rest of the home
// screen the instant an app opens, unlike the persistent nav bar below it.
static void build_lp_home_dock(lv_obj_t *parent, uint16_t w)
{
    lv_obj_t *dock = lv_obj_create(parent);
    lv_obj_remove_style_all(dock);
    lv_obj_set_size(dock, w, LP_HOME_DOCK_H);
    // -CUPCAKE_NAVBAR_H, not 0 — the home screen is genuinely full height
    // now (cupcake_ui_init() no longer reserves nav bar space for it), so a
    // plain bottom-align here would land this row directly on top of the
    // nav bar's own footprint instead of sitting above it as intended.
    lv_obj_align(dock, LV_ALIGN_BOTTOM_MID, 0, -CUPCAKE_NAVBAR_H);
    lv_obj_set_style_bg_color(dock, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dock, LV_OPA_50, 0);
    lv_obj_clear_flag(dock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(dock, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dock, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    int n = app_manager_count();
    int fav_n = (n < MAX_HOME_DOCK_FAV) ? n : MAX_HOME_DOCK_FAV;
    int left_n = fav_n / 2;   // 2 of the 4 land left of center, the rest right

    for (int i = 0; i < left_n; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (!app) continue;
        build_lp_dock_icon(dock, i, icon_for_app(app->name), lp_dock_fav_click_cb,
                            cupcake_tint_color(app->name, 0x18), LP_HOME_DOCK_FAV_SIZE, ICON_PX_HOME_DOCK);
    }

    build_lp_dock_icon(dock, -1, BP_ICON_ALL_APPS_48, lp_apps_launcher_click_cb,
                        lv_color_make(0x40, 0x40, 0x40), LP_HOME_DOCK_CENTER_SIZE, ICON_PX_HOME_DOCK_CENTER);

    for (int i = left_n; i < fav_n; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (!app) continue;
        build_lp_dock_icon(dock, i, icon_for_app(app->name), lp_dock_fav_click_cb,
                            cupcake_tint_color(app->name, 0x18), LP_HOME_DOCK_FAV_SIZE, ICON_PX_HOME_DOCK);
    }
}

static void build_home_screen(uint16_t w, uint16_t h)
{
    s_home_screen = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_home_screen);
    lv_obj_set_size(s_home_screen, w, h);
    lv_obj_set_pos(s_home_screen, 0, 0);
    lv_obj_clear_flag(s_home_screen, LV_OBJ_FLAG_SCROLLABLE);

    if (load_wallpaper_choice(&s_wallpaper_img, w, h)) {
        lv_obj_t *bg = lv_img_create(s_home_screen);
        lv_img_set_src(bg, &s_wallpaper_img);
        lv_obj_set_pos(bg, 0, 0);
        lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_set_style_bg_color(s_home_screen, lv_color_make(0x10, 0x18, 0x30), 0);
        lv_obj_set_style_bg_grad_color(s_home_screen, lv_color_make(0x05, 0x08, 0x14), 0);
        lv_obj_set_style_bg_grad_dir(s_home_screen, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(s_home_screen, LV_OPA_COVER, 0);
    }

    build_lp_home_dock(s_home_screen, w);
}

// ── Lollipop nav bar (Back / Home / Recents) ────────────────────────────────
// Lives on lv_layer_top(), same trick the status bar above already uses —
// LVGL always hit-tests/paints that layer above lv_scr_act()'s whole tree
// regardless of z-order, which is what makes this bar persistent over every
// app window instead of only showing on the home screen like the old dock.
// No favorites/running-app icons in the bar itself yet, on request — just
// the three buttons. Back and Recents are stubs (see their click handlers).

// Home's icon is a plain filled circle, not a symbol glyph — matches real
// Android 5.0's minimalist 3-button nav (Home was literally just a circle,
// not a house), and distinguishes it from Back/Recents' text-glyph buttons.
static lv_obj_t *build_lp_home_navbtn(lv_obj_t *parent, lv_event_cb_t click_cb)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LP_NAVBTN_SIZE, LP_NAVBTN_SIZE);
    lv_obj_set_style_radius(btn, (lv_coord_t)(LP_NAVBTN_SIZE / 2), 0);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x30, 0x30, 0x30), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, click_cb, LV_EVENT_CLICKED, NULL);

    lv_coord_t dot = (lv_coord_t)(LP_NAVBTN_SIZE / 2);
    lv_obj_t *circle = lv_obj_create(btn);
    lv_obj_remove_style_all(circle);
    lv_obj_set_size(circle, dot, dot);
    lv_obj_set_style_radius(circle, (lv_coord_t)(dot / 2), 0);
    lv_obj_set_style_bg_color(circle, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(circle);
    return btn;
}

// Invisible strip at the very bottom edge, created BEFORE the bar (so it
// sits behind it in layer_top's z-order) and never hidden — the bar's
// buttons intercept touches normally while it's shown, but once
// lp_hide_navbar() hides the bar, this is the only thing left in that
// screen region to catch the upward swipe that brings it back. Only counts
// as a reveal swipe while an app is actually in the foreground; on the home
// screen the bar is already permanently visible, nothing to reveal.
//
// Deliberately much shorter than CUPCAKE_NAVBAR_H, not the bar's full
// height — confirmed live: a full-height always-clickable hotzone became an
// "invisible wall" once the bar hid, silently swallowing every tap an app
// made in that screen region (LVGL still gave it hit-test priority even
// though it renders nothing) instead of passing them through to the app
// underneath. A swipe only needs to *start* inside this strip — LVGL keeps
// tracking PRESSED/RELEASED on whichever object the touch began on even as
// the finger moves well outside its bounds — so shrinking it doesn't affect
// swipe detection at all, only how much of the screen it can block.
#define LP_NAVBAR_HOTZONE_H 10
static lv_coord_t s_lp_swipe_press_y0;

static void lp_navbar_hotzone_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    if (code == LV_EVENT_PRESSED) {
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        s_lp_swipe_press_y0 = pt.y;
    } else if (code == LV_EVENT_RELEASED) {
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        lv_coord_t dy = (lv_coord_t)(s_lp_swipe_press_y0 - pt.y); // positive = moved up
        if (dy >= 12 && s_lp_foreground_idx >= 0) lp_show_navbar(true);
    }
}

static void build_lp_navbar_hotzone(uint16_t w)
{
    lv_obj_t *zone = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(zone);
    lv_obj_set_size(zone, w, LP_NAVBAR_HOTZONE_H);
    lv_obj_align(zone, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(zone, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(zone, lp_navbar_hotzone_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(zone, lp_navbar_hotzone_event_cb, LV_EVENT_RELEASED, NULL);
}

static void build_lp_navbar(uint16_t w)
{
    // Must be created first — see build_lp_navbar_hotzone()'s comment for
    // why the z-order (hotzone behind, bar in front) matters here.
    build_lp_navbar_hotzone(w);

    s_lp_navbar = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_lp_navbar);
    lv_obj_set_size(s_lp_navbar, w, CUPCAKE_NAVBAR_H);
    lv_obj_align(s_lp_navbar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_lp_navbar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_lp_navbar, LV_OPA_50, 0);
    lv_obj_clear_flag(s_lp_navbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(s_lp_navbar, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_lp_navbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_lp_navbar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    build_lp_navbtn(s_lp_navbar, LV_SYMBOL_LEFT, lp_navbar_back_click_cb);
    build_lp_home_navbtn(s_lp_navbar, lp_navbar_home_click_cb);
    build_lp_navbtn(s_lp_navbar, LV_SYMBOL_STOP, lp_navbar_recents_click_cb);
}

// ── Lollipop launcher (full-screen overlay, small-square tiles) ────────────
// Opened from the nav bar's center button. Same 2-column-at-a-time-visible,
// scroll-for-more grid shape Cupcake's old drawer used, just with smaller
// icon-only LP_LAUNCHER_TILE squares instead of the old 136x80 icon+label
// ones — more columns fit per row as a result (comfortably 4 on a 320px-wide
// screen instead of 2), pure layout consequence of the smaller tile, not a
// separately hardcoded column count anywhere below.

static lv_obj_t *s_lp_launcher_grid;

static void build_lp_launcher(uint16_t w, uint16_t h)
{
    s_lp_launcher = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_lp_launcher);
    lv_obj_set_size(s_lp_launcher, w, h);
    lv_obj_set_pos(s_lp_launcher, 0, 0);
    lv_obj_set_style_bg_color(s_lp_launcher, lv_color_make(0x18, 0x18, 0x18), 0);
    lv_obj_set_style_bg_opa(s_lp_launcher, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_lp_launcher, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_lp_launcher, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title_bar = lv_obj_create(s_lp_launcher);
    lv_obj_remove_style_all(title_bar);
    lv_obj_set_size(title_bar, w, 32);
    lv_obj_set_pos(title_bar, 0, 0);
    lv_obj_set_style_bg_opa(title_bar, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(title_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_lbl = lv_label_create(title_bar);
    lv_label_set_text(title_lbl, "All Apps");
    lv_obj_set_style_text_color(title_lbl, lv_color_white(), 0);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t *close_btn = lv_label_create(title_bar);
    lv_obj_set_style_text_color(close_btn, lv_color_white(), 0);
    lv_label_set_text(close_btn, LV_SYMBOL_CLOSE);
    lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_flag(close_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(close_btn, lp_launcher_close_click_cb, LV_EVENT_CLICKED, NULL);

    lv_coord_t page_h = (lv_coord_t)(h - 32);

    s_lp_launcher_grid = lv_obj_create(s_lp_launcher);
    lv_obj_remove_style_all(s_lp_launcher_grid);
    lv_obj_set_size(s_lp_launcher_grid, w, page_h);
    lv_obj_set_pos(s_lp_launcher_grid, 0, 32);
    lv_obj_set_style_bg_opa(s_lp_launcher_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(s_lp_launcher_grid, LV_DIR_VER);
    lv_obj_clear_flag(s_lp_launcher_grid, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_layout(s_lp_launcher_grid, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_lp_launcher_grid, LV_FLEX_FLOW_ROW_WRAP);
    // SPACE_EVENLY's gap comes from *leftover* space in the container — past
    // the first couple of rows there's no leftover left to distribute, so
    // the row gap collapses to 0 and rows render flush against each other
    // (confirmed live on the old drawer this was ported from). Fixed
    // pad_row/pad_column gaps hold regardless of overflow; START packs rows
    // from the top instead of trying to spread them across insufficient space.
    lv_obj_set_flex_align(s_lp_launcher_grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(s_lp_launcher_grid, 8, 0);
    lv_obj_set_style_pad_row(s_lp_launcher_grid, 10, 0);
    lv_obj_set_style_pad_column(s_lp_launcher_grid, 10, 0);

    int n = app_manager_count();
    if (n > MAX_LAUNCHER_TILES) n = MAX_LAUNCHER_TILES;

    for (int i = 0; i < n; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (!app) continue;
        build_lp_launcher_tile(s_lp_launcher_grid, i, app->name, lp_launcher_tile_click_cb);
    }
}

// ── Status panels (forked from cardstack_ui.c, ck_/CK_ prefix) ──────────────
// Two independent drag-down panels instead of one: which half of the screen
// the swipe starts from decides which one opens — left = Notifications,
// right = Running Apps (labeled that way for now rather than "Quick
// Settings" since there are no real toggles to put there yet; a natural
// place to add them later).

typedef enum { CK_STATUS_PEEK, CK_STATUS_EXPANDED } ck_status_state_t;

typedef struct {
    lv_obj_t          *panel;
    lv_obj_t          *handle;   // grab handle, shown only once expanded
    ck_status_state_t  state;
    lv_coord_t         press_y0;
    lv_coord_t         base_y;
} ck_panel_t;

static ck_panel_t s_notif_panel;
static ck_panel_t s_quick_panel;

static lv_obj_t *s_status_hotzone_left;
static lv_obj_t *s_status_hotzone_right;
static lv_obj_t *s_icon_wifi;
static lv_obj_t *s_icon_lora;
static lv_obj_t *s_icon_mail;
static lv_obj_t *s_icon_mail_badge;
static lv_obj_t *s_icon_battery;
static lv_obj_t *s_batt_voltage_lbl;
static lv_obj_t *s_status_notif_box;
static lv_obj_t *s_status_taskmgr_box;

static lv_obj_t *s_lock_screen;
static bool      s_locked = false;

// Actual implementation of the forward-declared lp_show_status()/lp_hide_
// status() from earlier — deferred to here since they need the icon handles
// above to exist first. temporary=true arms the same kind of auto-hide
// countdown as the nav bar; temporary=false (home screen) leaves them up.
static void lp_show_status(bool temporary)
{
    lv_obj_t *icons[] = { s_icon_wifi, s_icon_lora, s_icon_mail, s_icon_mail_badge,
                           s_icon_battery, s_batt_voltage_lbl };
    for (size_t i = 0; i < sizeof(icons) / sizeof(icons[0]); i++) {
        if (icons[i]) lv_obj_clear_flag(icons[i], LV_OBJ_FLAG_HIDDEN);
    }
    // The panels themselves also need showing, not just the icon labels
    // floating on top of them — each has its own opaque black background,
    // and at rest (CK_STATUS_PEEK) is positioned so its bottom PEEK_H
    // sliver is always on-screen. Confirmed live: hiding only the icons
    // left that solid black strip behind regardless.
    if (s_notif_panel.panel) lv_obj_clear_flag(s_notif_panel.panel, LV_OBJ_FLAG_HIDDEN);
    if (s_quick_panel.panel) lv_obj_clear_flag(s_quick_panel.panel, LV_OBJ_FLAG_HIDDEN);
    s_lp_status_hide_deadline_ms = temporary ? (purr_kernel_uptime_ms() + LP_STATUS_REVEAL_MS) : 0;
}

static void lp_hide_status(void)
{
    // Settings' "keep bars visible" toggle — see lp_hide_navbar()'s matching
    // guard/comment above.
    if (purr_kernel_navbar_always_visible()) { lp_show_status(false); return; }
    lv_obj_t *icons[] = { s_icon_wifi, s_icon_lora, s_icon_mail, s_icon_mail_badge,
                           s_icon_battery, s_batt_voltage_lbl };
    for (size_t i = 0; i < sizeof(icons) / sizeof(icons[0]); i++) {
        if (icons[i]) lv_obj_add_flag(icons[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_notif_panel.panel) lv_obj_add_flag(s_notif_panel.panel, LV_OBJ_FLAG_HIDDEN);
    if (s_quick_panel.panel) lv_obj_add_flag(s_quick_panel.panel, LV_OBJ_FLAG_HIDDEN);
    s_lp_status_hide_deadline_ms = 0;
}

static lv_coord_t ck_panel_y_for_state(ck_status_state_t s)
{
    switch (s) {
        case CK_STATUS_PEEK:     return (lv_coord_t)(-(CUPCAKE_STATUS_EXPANDED_H - CUPCAKE_STATUS_PEEK_H));
        case CK_STATUS_EXPANDED: return 0;
    }
    return (lv_coord_t)(-(CUPCAKE_STATUS_EXPANDED_H - CUPCAKE_STATUS_PEEK_H));
}

static void ck_panel_set_state(ck_panel_t *p, ck_status_state_t s)
{
    p->state = s;
    lv_obj_set_y(p->panel, ck_panel_y_for_state(s));
    if (s == CK_STATUS_EXPANDED) {
        lv_obj_clear_flag(p->handle, LV_OBJ_FLAG_HIDDEN);
        // Reveal (and keep up, no countdown) while a panel is actually open
        // and presumably being read — see lp_show_status()'s doc comment
        // for why this piggybacks on the panel gesture at all.
        lp_show_status(false);
    } else {
        lv_obj_add_flag(p->handle, LV_OBJ_FLAG_HIDDEN);
        // Collapsing back to peek: arm the same auto-hide countdown the nav
        // bar uses if an app is in the foreground; on the home screen,
        // status stays permanently visible like everywhere else.
        lp_show_status(s_lp_foreground_idx >= 0);
    }
}

// Shared drag handler for both panels and both hotzones — which panel it's
// dragging is passed as user_data (&s_notif_panel or &s_quick_panel).
static void ck_panel_drag_event_cb(lv_event_t *e)
{
    ck_panel_t *p = (ck_panel_t *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    if (code == LV_EVENT_PRESSED) {
        // Reveal immediately so the panel is actually visible while being
        // dragged, not just once ck_panel_set_state() fires on release —
        // temporary=true here is harmless even if the drag ends up
        // collapsing back to PEEK, since RELEASED below always calls
        // ck_panel_set_state() right after, which sets the correct
        // countdown-vs-permanent state for wherever the drag actually ends.
        lp_show_status(true);
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        p->press_y0 = pt.y;
        p->base_y   = lv_obj_get_y(p->panel);
    } else if (code == LV_EVENT_PRESSING) {
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        lv_coord_t dy = (lv_coord_t)(pt.y - p->press_y0);
        lv_coord_t ny = (lv_coord_t)(p->base_y + dy);
        if (ny < ck_panel_y_for_state(CK_STATUS_PEEK)) ny = ck_panel_y_for_state(CK_STATUS_PEEK);
        if (ny > 0) ny = 0;
        lv_obj_set_y(p->panel, ny);
    } else if (code == LV_EVENT_RELEASED) {
        lv_coord_t y = lv_obj_get_y(p->panel);
        ck_status_state_t target = (y > -(CUPCAKE_STATUS_EXPANDED_H / 2)) ? CK_STATUS_EXPANDED : CK_STATUS_PEEK;
        ck_panel_set_state(p, target);
    }
}

// Both panels are full width once expanded, so only one should be open at a
// time — starting a drag on one collapses the other first.
static void ck_hotzone_left_pressed_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) return;
    if (s_quick_panel.state == CK_STATUS_EXPANDED) ck_panel_set_state(&s_quick_panel, CK_STATUS_PEEK);
}
static void ck_hotzone_right_pressed_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) return;
    if (s_notif_panel.state == CK_STATUS_EXPANDED) ck_panel_set_state(&s_notif_panel, CK_STATUS_PEEK);
}

static void ck_build_panel(ck_panel_t *p, uint16_t w, const char *title, lv_obj_t **out_box)
{
    p->panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(p->panel);
    lv_obj_set_size(p->panel, w, CUPCAKE_STATUS_EXPANDED_H);
    lv_obj_set_pos(p->panel, 0, ck_panel_y_for_state(CK_STATUS_PEEK));
    lv_obj_set_style_bg_color(p->panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(p->panel, LV_OPA_COVER, 0);
    lv_obj_clear_flag(p->panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p->panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p->panel, ck_panel_drag_event_cb, LV_EVENT_PRESSED, p);
    lv_obj_add_event_cb(p->panel, ck_panel_drag_event_cb, LV_EVENT_PRESSING, p);
    lv_obj_add_event_cb(p->panel, ck_panel_drag_event_cb, LV_EVENT_RELEASED, p);

    lv_obj_t *title_lbl = lv_label_create(p->panel);
    lv_obj_set_style_text_color(title_lbl, lv_color_make(0xA0, 0xA0, 0xA0), 0);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_pos(title_lbl, 4, CUPCAKE_STATUS_PEEK_H + 2);

    p->handle = lv_obj_create(p->panel);
    lv_obj_remove_style_all(p->handle);
    lv_obj_set_size(p->handle, 40, 5);
    lv_obj_set_style_bg_color(p->handle, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_obj_set_style_bg_opa(p->handle, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p->handle, 3, 0);
    lv_obj_clear_flag(p->handle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(p->handle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(p->handle, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_add_flag(p->handle, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *box = lv_obj_create(p->panel);
    lv_obj_remove_style_all(box);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_size(box, (lv_coord_t)(w - 8), 160);
    lv_obj_set_pos(box, 4, CUPCAKE_STATUS_PEEK_H + 18);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    // Scrollable + vertical-only, same fix as the "All Apps" launcher grid
    // (build_lp_launcher()'s s_lp_launcher_grid) — this box's row count tracks live
    // data (notification count / running-app count) with no upper bound, and
    // with scrolling cleared plus remove_style_all()'s default clipping gone,
    // rows past the fixed 160px height used to bleed out unclipped into the
    // status bar rendered underneath on lv_layer_top().
    lv_obj_add_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);

    *out_box = box;
}

// Forward-declared: defined further down alongside the rest of the
// notifications/task-manager refresh logic, but needed here to wire up the
// panels' title-bar action buttons at build time.
static void ck_notif_clear_cb(lv_event_t *e);
static void ck_taskmgr_home_cb(lv_event_t *e);

static void ck_build_status_panels(uint16_t w)
{
    ck_build_panel(&s_notif_panel, w, "Notifications", &s_status_notif_box);
    ck_build_panel(&s_quick_panel, w, "Running Apps", &s_status_taskmgr_box);

    // Action button on the empty right side of each panel's title bar,
    // opposite the title label (which sits at x=4) — same clickable-label
    // style already used for the drawer's title-bar close icon.
    lv_obj_t *notif_clear_btn = lv_label_create(s_notif_panel.panel);
    lv_obj_set_style_text_color(notif_clear_btn, lv_color_white(), 0);
    lv_label_set_text(notif_clear_btn, LV_SYMBOL_CLOSE);
    lv_obj_set_pos(notif_clear_btn, (lv_coord_t)(w - 20), CUPCAKE_STATUS_PEEK_H + 2);
    lv_obj_add_flag(notif_clear_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(notif_clear_btn, ck_notif_clear_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *taskmgr_home_btn = lv_label_create(s_quick_panel.panel);
    lv_obj_set_style_text_color(taskmgr_home_btn, lv_color_white(), 0);
    lv_label_set_text(taskmgr_home_btn, LV_SYMBOL_HOME);
    lv_obj_set_pos(taskmgr_home_btn, (lv_coord_t)(w - 20), CUPCAKE_STATUS_PEEK_H + 2);
    lv_obj_add_flag(taskmgr_home_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(taskmgr_home_btn, ck_taskmgr_home_cb, LV_EVENT_CLICKED, NULL);

    s_status_hotzone_left = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_status_hotzone_left);
    lv_obj_set_size(s_status_hotzone_left, (lv_coord_t)(w / 2), CUPCAKE_STATUS_PEEK_H);
    lv_obj_set_pos(s_status_hotzone_left, 0, 0);
    lv_obj_set_style_bg_opa(s_status_hotzone_left, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_status_hotzone_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_status_hotzone_left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_status_hotzone_left, ck_hotzone_left_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_status_hotzone_left, ck_panel_drag_event_cb, LV_EVENT_PRESSED, &s_notif_panel);
    lv_obj_add_event_cb(s_status_hotzone_left, ck_panel_drag_event_cb, LV_EVENT_PRESSING, &s_notif_panel);
    lv_obj_add_event_cb(s_status_hotzone_left, ck_panel_drag_event_cb, LV_EVENT_RELEASED, &s_notif_panel);

    s_status_hotzone_right = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_status_hotzone_right);
    lv_obj_set_size(s_status_hotzone_right, (lv_coord_t)(w - w / 2), CUPCAKE_STATUS_PEEK_H);
    lv_obj_set_pos(s_status_hotzone_right, (lv_coord_t)(w / 2), 0);
    lv_obj_set_style_bg_opa(s_status_hotzone_right, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_status_hotzone_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_status_hotzone_right, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_status_hotzone_right, ck_hotzone_right_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_status_hotzone_right, ck_panel_drag_event_cb, LV_EVENT_PRESSED, &s_quick_panel);
    lv_obj_add_event_cb(s_status_hotzone_right, ck_panel_drag_event_cb, LV_EVENT_PRESSING, &s_quick_panel);
    lv_obj_add_event_cb(s_status_hotzone_right, ck_panel_drag_event_cb, LV_EVENT_RELEASED, &s_quick_panel);
}

// The envelope/mail icon always opens Notifications specifically, regardless
// of which hotzone half was last used.
static void ck_mail_icon_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_quick_panel.state == CK_STATUS_EXPANDED) ck_panel_set_state(&s_quick_panel, CK_STATUS_PEEK);
    ck_panel_set_state(&s_notif_panel, s_notif_panel.state == CK_STATUS_PEEK ? CK_STATUS_EXPANDED : CK_STATUS_PEEK);
}

#define CK_ICON_ON  lv_color_make(0x4D, 0xD0, 0x6B)
#define CK_ICON_OFF lv_color_make(0x55, 0x55, 0x55)

static void ck_build_status_icons(uint16_t w)
{
    s_icon_battery = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(s_icon_battery, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_icon_battery, lv_color_white(), 0);
    lv_label_set_text(s_icon_battery, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_pos(s_icon_battery, 6, 4);

    // Raw voltage next to the icon — the icon alone only has 5 discrete
    // states and no fuel gauge backs it (adc_battery.c's single-pin ADC
    // reading + a generic LiPo curve is an approximation), so the number
    // is the one actually trustworthy reading here.
    s_batt_voltage_lbl = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(s_batt_voltage_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_batt_voltage_lbl, lv_color_make(0xA0, 0xA0, 0xA0), 0);
    lv_label_set_text(s_batt_voltage_lbl, "");
    lv_obj_set_pos(s_batt_voltage_lbl, 24, 4);

    s_icon_wifi = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(s_icon_wifi, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_icon_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_pos(s_icon_wifi, (lv_coord_t)(w - 58), 4);

    s_icon_lora = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(s_icon_lora, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_icon_lora, LV_SYMBOL_GPS);
    lv_obj_set_pos(s_icon_lora, (lv_coord_t)(w - 36), 4);

    s_icon_mail = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(s_icon_mail, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_icon_mail, lv_color_white(), 0);
    lv_label_set_text(s_icon_mail, LV_SYMBOL_ENVELOPE);
    lv_obj_set_pos(s_icon_mail, (lv_coord_t)(w - 16), 4);
    lv_obj_add_flag(s_icon_mail, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_icon_mail, ck_mail_icon_click_cb, LV_EVENT_CLICKED, NULL);

    s_icon_mail_badge = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_icon_mail_badge);
    lv_obj_set_size(s_icon_mail_badge, 6, 6);
    lv_obj_set_style_bg_color(s_icon_mail_badge, lv_color_make(0xE0, 0x30, 0x30), 0);
    lv_obj_set_style_bg_opa(s_icon_mail_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_icon_mail_badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_pos(s_icon_mail_badge, (lv_coord_t)(w - 8), 2);
    lv_obj_clear_flag(s_icon_mail_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_icon_mail_badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_icon_mail_badge, LV_OBJ_FLAG_HIDDEN);
}

static void ck_refresh_status_icons(void)
{
    lv_obj_set_style_text_color(s_icon_wifi, purr_kernel_wifi_connected() ? CK_ICON_ON : CK_ICON_OFF, 0);
    lv_obj_set_style_text_color(s_icon_lora, purr_kernel_lora_available() ? CK_ICON_ON : CK_ICON_OFF, 0);

    // Skip touching the badge while the whole status row is auto-hidden
    // (lp_hide_status(), in-app) — s_icon_wifi's own hidden state doubles as
    // "is the row currently supposed to be visible at all", since every
    // icon (including this badge) is always toggled together by lp_show_
    // status()/lp_hide_status(). Without this guard, a real unread
    // notification would unconditionally re-show just the badge on the very
    // next tick even while everything else stayed correctly hidden.
    if (!lv_obj_has_flag(s_icon_wifi, LV_OBJ_FLAG_HIDDEN)) {
        if (purr_kernel_notify_count() > 0) lv_obj_clear_flag(s_icon_mail_badge, LV_OBJ_FLAG_HIDDEN);
        else                                 lv_obj_add_flag(s_icon_mail_badge, LV_OBJ_FLAG_HIDDEN);
    }

    int pct = purr_kernel_battery_percent();
    const char *sym;
    lv_color_t  color = lv_color_white();
    if (pct < 0)        { sym = LV_SYMBOL_BATTERY_FULL;  color = CK_ICON_OFF; }
    else if (pct > 80)  sym = LV_SYMBOL_BATTERY_FULL;
    else if (pct > 55)  sym = LV_SYMBOL_BATTERY_3;
    else if (pct > 30)  sym = LV_SYMBOL_BATTERY_2;
    else if (pct > 10)  sym = LV_SYMBOL_BATTERY_1;
    else                { sym = LV_SYMBOL_BATTERY_EMPTY; color = lv_color_make(0xE0, 0x40, 0x40); }
    lv_label_set_text(s_icon_battery, sym);
    lv_obj_set_style_text_color(s_icon_battery, color, 0);

    int mv = purr_kernel_battery_voltage_mv();
    char vbuf[16];
    if (mv < 0) vbuf[0] = '\0';
    else        snprintf(vbuf, sizeof(vbuf), "%d.%02dV", mv / 1000, (mv % 1000) / 10);
    lv_label_set_text(s_batt_voltage_lbl, vbuf);
}

static void ck_refresh_status_notif_box(void)
{
    lv_obj_clean(s_status_notif_box);
    int n = purr_kernel_notify_count();
    for (int i = 0; i < n; i++) {
        purr_notification_t note;
        if (!purr_kernel_notify_at(i, &note)) break;
        lv_obj_t *row = lv_label_create(s_status_notif_box);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_text_color(row, lv_color_white(), 0);
        char line[PURR_NOTIFY_TITLE_LEN + PURR_NOTIFY_BODY_LEN + 4];
        snprintf(line, sizeof(line), "%s: %s", note.title, note.body);
        lv_label_set_text(row, line);
    }
}

// Deferred, not a direct ck_refresh_status_notif_box() call — that function
// calls lv_obj_clean() on the notif box, and this callback runs from inside
// an LVGL click-event dispatch (the Clear button's LV_EVENT_CLICKED), the
// same "synchronous rebuild mid-dispatch" shape that hung cupcake_task
// elsewhere (see cupcake_win.c's ck_list_set_items_async_cb() comment for
// the full mechanism). lv_async_call() defers the actual lv_obj_clean()+
// rebuild to the start of the next lv_timer_handler() tick, outside this
// event's call stack.
static void ck_notif_clear_refresh_cb(void *user)
{
    (void)user;
    ck_refresh_status_notif_box();
}

static void ck_notif_clear_cb(lv_event_t *e)
{
    (void)e;
    purr_kernel_notify_clear();
    lv_async_call(ck_notif_clear_refresh_cb, NULL);
}

// ── Running Apps (task manager, in the drag-down panel) ─────────────────────
// Same data/row pattern as cardstack_ui.c's Task Manager card
// (build_taskmgr_card/refresh_taskmgr_card/taskmgr_kill_cb) — Cupcake has no
// card stack to host an equivalent card in, so this lives in the
// notification panel instead, since that's the one persistent overlay
// already reachable from anywhere.

static void ck_taskmgr_kill_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    app_manager_stop(idx);
    if (idx == s_lp_foreground_idx) {
        s_lp_foreground_idx = -1;
        lp_show_navbar(false);
        lp_show_status(false);
    }
}

// Full "return to home screen" — hides the launcher (if open) and every
// currently-visible running app's window, then collapses this panel.
// Apps keep running (this only hides their windows, same as Minimize) — it's
// a navigation shortcut, not a kill-all.
static void ck_taskmgr_home_cb(lv_event_t *e)
{
    (void)e;
    if (s_lp_launcher) lv_obj_add_flag(s_lp_launcher, LV_OBJ_FLAG_HIDDEN);
    s_lp_foreground_idx = -1;
    lp_show_navbar(false);
    lp_show_status(false);
    int n = app_manager_count();
    for (int i = 0; i < n; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (app && app->state == APP_STATE_RUNNING && app->window) {
            purr_win_hide(app->window);
        }
    }
    ck_panel_set_state(&s_quick_panel, CK_STATUS_PEEK);
}

static void ck_taskmgr_open_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const app_entry_t *app = app_manager_get(idx);
    if (app && app->window) purr_win_show(app->window);
    s_lp_foreground_idx = idx;
    lp_hide_navbar();
    lp_hide_status();
}

static void ck_refresh_status_taskmgr(void)
{
    lv_obj_clean(s_status_taskmgr_box);
    int n = app_manager_count();
    bool any = false;
    for (int i = 0; i < n; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (!app || app->state != APP_STATE_RUNNING) continue;
        any = true;

        lv_obj_t *row = lv_obj_create(s_status_taskmgr_box);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 24);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_label_set_text(lbl, app->name);
        lv_obj_set_pos(lbl, 0, 4);

        lv_obj_t *kill_btn = lv_btn_create(row);
        lv_obj_set_size(kill_btn, 44, 20);
        lv_obj_align(kill_btn, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_t *kill_lbl = lv_label_create(kill_btn);
        lv_label_set_text(kill_lbl, "Kill");
        lv_obj_center(kill_lbl);
        lv_obj_add_event_cb(kill_btn, ck_taskmgr_kill_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *open_btn = lv_btn_create(row);
        lv_obj_set_size(open_btn, 44, 20);
        lv_obj_align(open_btn, LV_ALIGN_RIGHT_MID, -48, 0);
        lv_obj_t *open_lbl = lv_label_create(open_btn);
        lv_label_set_text(open_lbl, "Open");
        lv_obj_center(open_lbl);
        lv_obj_add_event_cb(open_btn, ck_taskmgr_open_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if (!any) {
        lv_obj_t *lbl = lv_label_create(s_status_taskmgr_box);
        lv_obj_set_style_text_color(lbl, lv_color_make(0x80, 0x80, 0x80), 0);
        lv_label_set_text(lbl, "No running apps");
    }
}

// ── Recents (card carousel) ─────────────────────────────────────────────────
// Deliberately NOT Cardstack's own scroll-snap-one-at-a-time carousel
// (cardstack_ui.c) — per spec this is a staggered/layered deck instead: a
// single running app's card takes up ~2/3 of the screen; two or more shrink
// and overlap so adjacent cards visibly peek out above/below the current
// one, scrollable like a physical stack. No live window thumbnails exist
// (purr_win.h has no such capability), so each card is a tinted placeholder
// — same cupcake_tint_color()/icon_for_app() identity Lollipop's launcher
// tiles and home dock already use, just bigger. Built lazily on first open
// and cleaned+refilled on every subsequent one (ck_refresh_status_taskmgr()'s
// same pattern just above), since the running-app list can change between
// opens.
static lv_obj_t *s_lp_recents_backdrop = NULL;
static lv_obj_t *s_lp_recents_scroll   = NULL;

static bool lp_recents_is_open(void)
{
    return s_lp_recents_backdrop && !lv_obj_has_flag(s_lp_recents_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void lp_recents_close(void)
{
    if (s_lp_recents_backdrop) lv_obj_add_flag(s_lp_recents_backdrop, LV_OBJ_FLAG_HIDDEN);
}

// Tapping a card opens that app and dismisses Recents — same open logic as
// the Running Apps panel's own Open button (ck_taskmgr_open_cb).
static void lp_recents_card_open_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const app_entry_t *app = app_manager_get(idx);
    if (app && app->window) purr_win_show(app->window);
    s_lp_foreground_idx = idx;
    lp_hide_navbar();
    lp_hide_status();
    lp_recents_close();
}

// The kill (X) button is its own clickable child — LVGL only fires a
// widget's own registered callback for the object that actually caught the
// touch, not its ancestors too (no LV_OBJ_FLAG_EVENT_BUBBLE set anywhere
// here), so tapping it never also triggers the card's open handler above.
static void lp_recents_card_kill_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    app_manager_stop(idx);
    if (idx == s_lp_foreground_idx) {
        s_lp_foreground_idx = -1;
        lp_show_navbar(false);
        lp_show_status(false);
    }
    lp_recents_close();
}

// Tapping the dimmed backdrop, or empty space between/around the cards,
// dismisses Recents without picking anything — same "tap outside to
// cancel" gesture the WiFi-password dialog's Cancel button serves in
// settings.c, just gesture-driven instead of a dedicated button here.
static void lp_recents_backdrop_click_cb(lv_event_t *e)
{
    (void)e;
    lp_recents_close();
}

static void lp_recents_open(void)
{
    uint16_t w = cupcake_hal_width();
    uint16_t h = cupcake_hal_height();

    if (!s_lp_recents_backdrop) {
        s_lp_recents_backdrop = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_lp_recents_backdrop);
        lv_obj_set_size(s_lp_recents_backdrop, w, h);
        lv_obj_set_pos(s_lp_recents_backdrop, 0, 0);
        lv_obj_set_style_bg_color(s_lp_recents_backdrop, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_lp_recents_backdrop, LV_OPA_70, 0);
        lv_obj_clear_flag(s_lp_recents_backdrop, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_lp_recents_backdrop, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_lp_recents_backdrop, lp_recents_backdrop_click_cb, LV_EVENT_CLICKED, NULL);

        // Left clickable (default) so an empty tap within it also bubbles
        // to a real CLICKED on itself (closing Recents) when no card
        // catches it, same reasoning as the backdrop above — and so it can
        // still receive the vertical drag that drives scrolling.
        s_lp_recents_scroll = lv_obj_create(s_lp_recents_backdrop);
        lv_obj_remove_style_all(s_lp_recents_scroll);
        lv_obj_set_size(s_lp_recents_scroll, w, h - CUPCAKE_NAVBAR_H);
        lv_obj_set_pos(s_lp_recents_scroll, 0, 0);
        lv_obj_set_style_bg_opa(s_lp_recents_scroll, LV_OPA_TRANSP, 0);
        lv_obj_set_scroll_dir(s_lp_recents_scroll, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(s_lp_recents_scroll, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_event_cb(s_lp_recents_scroll, lp_recents_backdrop_click_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_clean(s_lp_recents_scroll);

    int n = app_manager_count();
    int running = 0;
    for (int i = 0; i < n; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (app && app->state == APP_STATE_RUNNING) running++;
    }

    uint16_t avail_h = h - CUPCAKE_NAVBAR_H;
    uint16_t card_w  = (uint16_t)((w * 85) / 100);

    if (running == 0) {
        lv_obj_t *lbl = lv_label_create(s_lp_recents_scroll);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_label_set_text(lbl, "No running apps");
        lv_obj_center(lbl);
    } else {
        // A lone card gets ~2/3 of the available height, centered. Two or
        // more shrink to 45% each and overlap by 55% of their own height
        // (i.e. a 45%-tall sliver of every card behind the front one still
        // peeks out) — approximates the "three cards layered at different
        // heights" stack from the spec while staying scrollable for any
        // count beyond what fits on screen at once.
        uint16_t card_h = (running == 1) ? (uint16_t)((avail_h * 2) / 3)
                                          : (uint16_t)((avail_h * 45) / 100);
        uint16_t step      = (running == 1) ? 0 : (uint16_t)((card_h * 45) / 100);
        uint16_t content_h = (uint16_t)(card_h + (running > 1 ? (running - 1) * step : 0));
        uint16_t top_pad   = (content_h < avail_h) ? (uint16_t)((avail_h - content_h) / 2) : 0;

        int shown = 0;
        for (int i = 0; i < n; i++) {
            const app_entry_t *app = app_manager_get(i);
            if (!app || app->state != APP_STATE_RUNNING) continue;

            lv_obj_t *card = lv_obj_create(s_lp_recents_scroll);
            lv_obj_remove_style_all(card);
            lv_obj_set_size(card, card_w, card_h);
            lv_obj_set_pos(card, (w - card_w) / 2, top_pad + shown * step);
            lv_obj_set_style_radius(card, 16, 0);
            lv_obj_set_style_bg_color(card, cupcake_tint_color(app->name, 0x30), 0);
            lv_obj_set_style_bg_opa(card, LV_OPA_80, 0);
            lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(card, lp_recents_card_open_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

            lv_obj_t *icon = lv_img_create(card);
            lv_img_set_src(icon, icon_for_app(app->name));
            lv_img_set_zoom(icon, ICON_ZOOM(40));
            lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 14);
            lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t *lbl = lv_label_create(card);
            lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
            lv_label_set_text(lbl, app->name);
            lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 60);
            lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t *kill_btn = lv_btn_create(card);
            lv_obj_set_size(kill_btn, 28, 28);
            lv_obj_align(kill_btn, LV_ALIGN_TOP_RIGHT, -8, 8);
            lv_obj_t *kill_lbl = lv_label_create(kill_btn);
            lv_label_set_text(kill_lbl, LV_SYMBOL_CLOSE);
            lv_obj_center(kill_lbl);
            lv_obj_add_event_cb(kill_btn, lp_recents_card_kill_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

            shown++;
        }
    }

    lv_obj_clear_flag(s_lp_recents_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_lp_recents_backdrop);
    // Recents is itself a system-level overview, same as the home screen —
    // stays up like the home screen's own bars do (lp_show_navbar(false)/
    // lp_show_status(false), the "permanent" — not auto-hide — call), and
    // needs re-raising above the backdrop it might now sit behind since the
    // backdrop was just (re)moved to the front above.
    if (s_lp_navbar) lv_obj_move_foreground(s_lp_navbar);
    lp_show_navbar(false);
    lp_show_status(false);
}

// ── Lock screen ──────────────────────────────────────────────────────────────
// No PIN — tap or swipe to dismiss, matching where OOBE/security work
// already stands (deferred to a future v1.0 first-run setup flow). Built
// the same way ck_build_panel() blocks input for the notification/quick
// panels above: a fully opaque, clickable object on lv_layer_top(), which
// LVGL always hit-tests above every app window's lv_scr_act() tree
// (cupcake_win.c:168's comment) — so this one object is enough to swallow
// touches/keys meant for whatever's underneath while locked.

static void restore_brightness(void)
{
    uint8_t level = 255;   // same default as settings.c's own s_brightness
    nvs_handle_t h;
    // "purr_settings"/"brightness" — settings.c's own NVS_NS/key
    // (source/apps/system/settings/settings.c). Read directly rather than
    // through a shared API since settings.c isn't guaranteed to have run
    // yet this boot (it lazy-loads on first open, same as this value).
    if (nvs_open("purr_settings", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "brightness", &level);
        nvs_close(h);
    }
    const catcall_display_t *disp = purr_kernel_display();
    if (disp && disp->set_brightness) disp->set_brightness(level);
}

static void ck_lock_dismiss_cb(lv_event_t *e)
{
    (void)e;
    if (!s_locked) return;
    s_locked = false;
    lv_obj_add_flag(s_lock_screen, LV_OBJ_FLAG_HIDDEN);
    // No need to reset the idle timestamp here — this dismiss callback
    // only runs as a downstream effect of the same touch that just fired
    // touch_read_cb()/mark_activity() in cupcake_hal.c earlier in this
    // same lv_timer_handler() pass, so cupcake_hal_last_activity_ms() is
    // already "now" by the time we get here.
}

// Minimalist-centered redesign (ASCII mockup approved by the user before
// building this): big uptime clock as the focal point, a small "up Xh Ym"
// subtitle right under it (states plainly that this isn't wall-clock time,
// instead of only a code comment saying so), a brand mark, a real-data
// status line (node/message counts) and a battery reading (reusing the
// same symbol-threshold logic as the status bar's own battery icon,
// ck_refresh_status_icons(), for a consistent look), and an unlock hint at
// the bottom. Dropped the earlier mockup's "signal bars" — there's no real
// per-device signal-strength metric to back it (RSSI is peer-relative),
// and this screen only ever shows numbers that come from an existing,
// already-used catcall (purr_kernel_battery_percent()/purr_kernel_notify_
// count()/mesh_manager_node_count()), not invented ones.
//
// Labels are populated by ck_lock_refresh_info(), called once on lock
// (ck_lock_check_idle()) and then every tick while locked (cupcake_ui_
// tick()) so they stay live for anyone glancing at a locked device, not
// just at the instant it locked.
static lv_obj_t *s_lock_clock_lbl;
static lv_obj_t *s_lock_uptime_lbl;
static lv_obj_t *s_lock_status_lbl;
static lv_obj_t *s_lock_battery_lbl;

static void ck_build_lock_screen(uint16_t w, uint16_t h)
{
    s_lock_screen = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_lock_screen);
    lv_obj_set_size(s_lock_screen, w, h);
    lv_obj_set_pos(s_lock_screen, 0, 0);
    lv_obj_set_style_bg_color(s_lock_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_lock_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_lock_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_lock_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_lock_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_lock_screen, ck_lock_dismiss_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_lock_screen, ck_lock_dismiss_cb, LV_EVENT_RELEASED, NULL);

    // No RTC/NTP anywhere in this codebase (checked — no sntp usage at
    // all, and catcall_gps_t carries no time field either), so this is
    // elapsed uptime formatted as a clock, not wall-clock time — the
    // subtitle below says so explicitly. Still genuinely useful at a
    // glance (confirms the device is alive and how long it's been
    // running) without claiming to be something it isn't. montserrat_32
    // (already linked in for Milk Bottle's big-text display) makes this
    // the visual focal point the mockup called for.
    s_lock_clock_lbl = lv_label_create(s_lock_screen);
    lv_obj_set_style_text_color(s_lock_clock_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_lock_clock_lbl, &lv_font_montserrat_32, 0);
    lv_label_set_text(s_lock_clock_lbl, "00:00:00");
    lv_obj_set_style_text_align(s_lock_clock_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lock_clock_lbl, LV_ALIGN_CENTER, 0, -60);

    s_lock_uptime_lbl = lv_label_create(s_lock_screen);
    lv_obj_set_style_text_color(s_lock_uptime_lbl, lv_color_make(0xB0, 0xB0, 0xB0), 0);
    lv_obj_set_style_text_font(s_lock_uptime_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_lock_uptime_lbl, "");
    lv_obj_set_style_text_align(s_lock_uptime_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lock_uptime_lbl, LV_ALIGN_CENTER, 0, -26);

    lv_obj_t *brand = lv_label_create(s_lock_screen);
    lv_obj_set_style_text_color(brand, lv_color_white(), 0);
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_14, 0);
    lv_label_set_text(brand, "PURR OS");
    lv_obj_set_style_text_align(brand, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(brand, LV_ALIGN_CENTER, 0, 6);

    s_lock_status_lbl = lv_label_create(s_lock_screen);
    lv_obj_set_style_text_color(s_lock_status_lbl, lv_color_white(), 0);
    lv_label_set_text(s_lock_status_lbl, "");
    lv_obj_set_style_text_align(s_lock_status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lock_status_lbl, LV_ALIGN_CENTER, 0, 36);

    s_lock_battery_lbl = lv_label_create(s_lock_screen);
    lv_obj_set_style_text_color(s_lock_battery_lbl, lv_color_white(), 0);
    lv_label_set_text(s_lock_battery_lbl, "");
    lv_obj_set_style_text_align(s_lock_battery_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lock_battery_lbl, LV_ALIGN_CENTER, 0, 58);

    lv_obj_t *hint = lv_label_create(s_lock_screen);
    lv_obj_set_style_text_color(hint, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_label_set_text(hint, "tap to unlock");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);
}

static void ck_lock_refresh_info(void)
{
    uint64_t up_s = purr_kernel_uptime_ms() / 1000ULL;
    unsigned hh = (unsigned)((up_s / 3600ULL) % 24ULL);
    unsigned mm = (unsigned)((up_s / 60ULL) % 60ULL);
    unsigned ss = (unsigned)(up_s % 60ULL);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u", hh, mm, ss);
    lv_label_set_text(s_lock_clock_lbl, buf);

    unsigned up_hh = (unsigned)(up_s / 3600ULL);
    if (up_hh > 0) snprintf(buf, sizeof(buf), "up %uh %um", up_hh, mm);
    else           snprintf(buf, sizeof(buf), "up %um", mm);
    lv_label_set_text(s_lock_uptime_lbl, buf);

    int nodes = 0;
#ifdef CONFIG_PURR_FEATURE_MESHTASTIC
    nodes = mesh_manager_node_count();
#endif
    int n = purr_kernel_notify_count();
    snprintf(buf, sizeof(buf), "%d node%s  |  %d message%s",
             nodes, nodes == 1 ? "" : "s", n, n == 1 ? "" : "s");
    lv_label_set_text(s_lock_status_lbl, buf);

    // Same threshold/symbol choice as ck_refresh_status_icons()'s own
    // battery icon, for a consistent look between the status bar and lock
    // screen.
    int pct = purr_kernel_battery_percent();
    if (pct < 0) {
        lv_label_set_text(s_lock_battery_lbl, "");
    } else {
        const char *sym = pct > 80 ? LV_SYMBOL_BATTERY_FULL :
                           pct > 55 ? LV_SYMBOL_BATTERY_3 :
                           pct > 30 ? LV_SYMBOL_BATTERY_2 :
                           pct > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
        snprintf(buf, sizeof(buf), "%s %d%%", sym, pct);
        lv_label_set_text(s_lock_battery_lbl, buf);
    }
}

bool cupcake_ui_is_locked(void) { return s_locked; }

void cupcake_ui_wake(void)
{
    if (!s_locked) return;
    restore_brightness();
}

static void ck_lock_check_idle(void)
{
    if (s_locked) return;
    uint8_t timeout_min = purr_kernel_screen_timeout_min();
    uint64_t elapsed_ms  = purr_kernel_uptime_ms() - cupcake_hal_last_activity_ms();
    if (elapsed_ms < (uint64_t)timeout_min * 60000ULL) return;

    s_locked = true;
    const catcall_display_t *disp = purr_kernel_display();
    if (disp && disp->set_brightness) disp->set_brightness(0);
    ck_lock_refresh_info();
    lv_obj_clear_flag(s_lock_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_lock_screen);
}

// ── Public API ────────────────────────────────────────────────────────────────

void cupcake_ui_init(void)
{
    uint16_t w = cupcake_hal_width();
    uint16_t h = cupcake_hal_height();
    // Both the status bar (top) and Lollipop nav bar (bottom) are separate
    // lv_layer_top() overlays — same reasoning as app windows going full
    // screen: they draw over content, they don't need content to make room
    // for them. Home screen/launcher/wallpaper get the true full (w,h) now;
    // build_lp_home_dock() separately offsets itself clear of the nav bar's
    // own footprint specifically, since that's the one row that actually
    // needs to avoid sitting directly underneath it.
    build_home_screen(w, h);
    build_lp_launcher(w, h);
    build_lp_navbar(w);

    ck_build_status_panels(w);
    ck_build_status_icons(w);
    ck_build_lock_screen(w, h);

    ESP_LOGI(TAG, "Lollipop home screen built (%d total apps)", app_manager_count());
}

void cupcake_ui_tick(void)
{
    ck_refresh_status_notif_box();
    ck_refresh_status_taskmgr();
    ck_refresh_status_icons();
    ck_lock_check_idle();
    if (s_locked) ck_lock_refresh_info();

    // Re-hide a swipe-revealed nav bar once its countdown expires — deadline
    // 0 means "no pending auto-hide" (home screen, or an app just opened
    // and the bar hasn't been swiped up on since), so this is a no-op then.
    if (s_lp_navbar_hide_deadline_ms != 0 && purr_kernel_uptime_ms() >= s_lp_navbar_hide_deadline_ms) {
        lp_hide_navbar();
    }
    // Same idea for the status row's own countdown, armed separately (see
    // ck_panel_set_state()) whenever a drag-down panel collapses back to
    // peek while an app is in the foreground.
    if (s_lp_status_hide_deadline_ms != 0 && purr_kernel_uptime_ms() >= s_lp_status_hide_deadline_ms) {
        lp_hide_status();
    }
}
