// cupcake_ui.c — Cupcake's *launcher*: wallpaper, home screen, favourites
// dock, and the full-screen All-Apps drawer. Nothing else.
//
// This file used to hold the system UI too (status bar, drag-down panels,
// nav bar, Recents, lock screen) — all of that now lives in the systemui
// module (source/modules/systemui/), which Cupcake hosts. What's left here
// is only the stuff that is genuinely "the launcher": the screens a user
// sees when no app is open, plus the shared per-app visual identity (icon +
// tint) the system UI borrows for its Recents cards, handed over through the
// purr_systemui_host_t table at the bottom of this file.
//
// Layout model: app windows are genuinely full-screen (cupcake_win.c's
// ck_win_create()), and the system UI's bars draw over them on
// lv_layer_top() rather than the layout reserving space. So the home screen
// below gets the true full (w,h) as well — the one exception is the
// favourites dock, which offsets itself up by purr_systemui_navbar_height()
// to sit above the nav bar instead of underneath it.
//
// Home leaves the foreground app running and hidden; Back closes it
// outright (no in-app back-navigation stack exists to unwind first) — both
// live on the nav bar, i.e. in the systemui module now.

#include "cupcake.h"
#include "../systemui/systemui.h"
#include "../../kernel/catcalls/purr_win.h"
#include "../app_manager/app_manager.h"
#include "../../assets/icons/blackpurr_icons.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "cupcake_ui";

// ICON_ZOOM comes from systemui.h — shared with its Recents cards, which
// need the identical 48x48-source-to-on-screen-px conversion.

#define MAX_LAUNCHER_TILES 64
// Small square + a name label underneath — most apps share the same
// fallback icon (cupcake_icon_for_app()'s tools-icon default), so without a
// label the "All Apps" grid is unreadable beyond the handful of apps with a
// real bundled icon. Width stays the old icon-only 64px; height grows to fit
// one line of label text below the icon.
#define LP_LAUNCHER_TILE_W  64
#define LP_LAUNCHER_TILE_H  84

// Home-screen-only favorites row (2 apps, apps-launcher button, 2 more apps)
// pinned directly above the persistent nav bar — NOT part of the nav bar
// itself, so it's covered/hidden along with the rest of the home screen the
// moment an app opens, same as Cupcake's old dock was.
#define MAX_HOME_DOCK_FAV       4
#define LP_HOME_DOCK_H          48
#define LP_HOME_DOCK_FAV_SIZE   36
#define LP_HOME_DOCK_CENTER_SIZE 44

// Desired on-screen sizes, applied via ICON_ZOOM above — the container size
// alone doesn't rescale an lv_img's source bitmap.
#define ICON_PX_LAUNCHER    40
#define ICON_PX_HOME_DOCK   24
#define ICON_PX_HOME_DOCK_CENTER 28

// ── Shared visual identity (also used by the system UI's Recents cards) ─────

static const lv_img_dsc_t *cupcake_icon_for_app(const char *name)
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

// ── Launching ───────────────────────────────────────────────────────────────

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
    // Hands foreground tracking (and the bars' auto-hide) to the system UI,
    // which owns both — see systemui.h.
    purr_systemui_enter_app(idx);
}

// ── All-Apps drawer (full-screen overlay, small-square tiles) ───────────────
// Opened from the home dock's center button. Same 2-column-at-a-time-visible,
// scroll-for-more grid shape Cupcake's old drawer used, just with smaller
// icon+label LP_LAUNCHER_TILE squares instead of the old 136x80 ones — more
// columns fit per row as a result (comfortably 4 on a 320px-wide screen
// instead of 2), pure layout consequence of the smaller tile, not a
// separately hardcoded column count anywhere below.

static lv_obj_t *s_lp_launcher;
static lv_obj_t *s_lp_launcher_grid;

// Exposed to the system UI (as host->hide_drawer) so its "return to home
// screen" paths can clear the drawer along with every app window — the
// drawer is a lv_scr_act() sibling it otherwise has no handle on.
static void cupcake_launcher_hide_drawer(void)
{
    if (s_lp_launcher) lv_obj_add_flag(s_lp_launcher, LV_OBJ_FLAG_HIDDEN);
}

// Closes the drawer first, then launches/restores — matches the old
// behavior (tile tap dismisses the overlay, not just opens the app).
static void lp_launcher_tile_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "launching app idx=%d (from All Apps)", idx);
    cupcake_launcher_hide_drawer();
    launch_or_restore(idx);
}

static void lp_launcher_close_click_cb(lv_event_t *e)
{
    (void)e;
    cupcake_launcher_hide_drawer();
}

// Opens the small-square all-apps drawer — the button that triggers this
// lives in the home screen's own dock row (see build_lp_home_dock()).
static void lp_apps_launcher_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_lp_launcher) {
        lv_obj_clear_flag(s_lp_launcher, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_lp_launcher);
    }
}

// Icon + name-label tile, used by the drawer's scrollable grid — see
// LP_LAUNCHER_TILE_W/_H's comment for why the label was added back.
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
    lv_img_set_src(icon, cupcake_icon_for_app(name));
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

    // Title bar above, nav bar below. The nav bar term was missing entirely, so
    // the grid ran the full height of the panel and its bottom row of icons sat
    // underneath the Back/Home/Recents buttons — unreadable, and untappable
    // since the bar swallows the touch.
    //
    // purr_systemui_navbar_height() rather than the PURR_SYSTEMUI_NAVBAR_H
    // constant: it reports 0 for the iOS style and for a host that suppressed
    // the bar, so this same expression stays correct on every configuration
    // instead of reserving a strip of dead space where no bar is ever drawn.
    lv_coord_t page_h = (lv_coord_t)(h - 32 - purr_systemui_navbar_height());

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

// ── Wallpaper ────────────────────────────────────────────────────────────────
// "default" (the gradient below) or an SD path chosen in Settings, persisted
// in NVS under the same "purr_settings" namespace settings.c already uses.
// Raw RGB565 files only — no on-device image decoder, matching how the
// icon-generation pipeline keeps decoding on the PC side. Falls back to the
// gradient on any missing/malformed file rather than failing to boot.

#define WALLPAPER_PATH_LEN 128

static lv_img_dsc_t s_wallpaper_img;
// Whether s_wallpaper_img actually holds a decoded image. The load can fail
// (no SD, missing file, wrong size), and the systemui wallpaper hook must not
// hand out a half-initialised descriptor.
static bool s_wallpaper_loaded = false;

// systemui host hook — the lock screen draws whatever the home screen shows.
// NULL is a normal answer when no wallpaper loaded; the lock screen then keeps
// its plain background.
static const lv_img_dsc_t *cupcake_wallpaper(void) {
    return s_wallpaper_loaded ? &s_wallpaper_img : NULL;
}

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

static lv_obj_t *s_home_screen;

// Round button with a centered app-icon bitmap — used by the home dock row
// (favorites + the all-apps button), which needs real app icons rather than
// the nav bar's LVGL symbol glyphs.
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

// Favorites row pinned to the bottom of the home screen — 2 apps, the
// all-apps button, 2 more apps, "first N in registry order" same as
// Cupcake's old dock (no separate favorites concept exists yet). A child of
// s_home_screen, NOT lv_layer_top() like the nav bar — this row is meant to
// disappear along with the rest of the home screen the instant an app opens,
// unlike the persistent nav bar below it.
static void build_lp_home_dock(lv_obj_t *parent, uint16_t w)
{
    lv_obj_t *dock = lv_obj_create(parent);
    lv_obj_remove_style_all(dock);
    lv_obj_set_size(dock, w, LP_HOME_DOCK_H);
    // Offset by the nav bar's height, not 0 — the home screen is genuinely
    // full height (cupcake_ui_init() reserves no nav bar space for it), so a
    // plain bottom-align here would land this row directly on top of the nav
    // bar's own footprint instead of sitting above it as intended. Asking the
    // system UI rather than using a constant means this correctly reclaims
    // the space when that module is compiled out (it then reports 0).
    lv_obj_align(dock, LV_ALIGN_BOTTOM_MID, 0, -purr_systemui_navbar_height());
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
        build_lp_dock_icon(dock, i, cupcake_icon_for_app(app->name), lp_dock_fav_click_cb,
                            cupcake_tint_color(app->name, 0x18), LP_HOME_DOCK_FAV_SIZE, ICON_PX_HOME_DOCK);
    }

    build_lp_dock_icon(dock, -1, BP_ICON_ALL_APPS_48, lp_apps_launcher_click_cb,
                        lv_color_make(0x40, 0x40, 0x40), LP_HOME_DOCK_CENTER_SIZE, ICON_PX_HOME_DOCK_CENTER);

    for (int i = left_n; i < fav_n; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (!app) continue;
        build_lp_dock_icon(dock, i, cupcake_icon_for_app(app->name), lp_dock_fav_click_cb,
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
        s_wallpaper_loaded = true;
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

// ── Hosting the system UI ───────────────────────────────────────────────────
// Cupcake is the *host* backend for the systemui module: it owns the render
// task everything runs on and answers the handful of questions the system UI
// can't answer itself. Static const so it can be handed over by pointer and
// outlive the init call, as purr_systemui_init() requires.

static const purr_systemui_host_t s_systemui_host = {
    .width                   = cupcake_hal_width,
    .height                  = cupcake_hal_height,
    .icon_for_app            = cupcake_icon_for_app,
    .tint_color              = cupcake_tint_color,
    .hide_drawer             = cupcake_launcher_hide_drawer,
    .hide_foreground_windows = cupcake_win_hide_foreground,
    .last_activity_ms        = cupcake_hal_last_activity_ms,
    .wallpaper               = cupcake_wallpaper,
};

// ── Public API ────────────────────────────────────────────────────────────────

void cupcake_ui_init(void)
{
    uint16_t w = cupcake_hal_width();
    uint16_t h = cupcake_hal_height();

    // System UI FIRST, then the launcher. Order is load-bearing, not stylistic.
    //
    // purr_systemui_navbar_height() reports 0 until purr_systemui_init() has run
    // — deliberately, so a host that suppresses the bar reclaims the space
    // rather than leaving a gap. Building the launcher first therefore made
    // every nav-bar inset in this file evaluate to zero, including
    // build_lp_home_dock()'s, which only LOOKED like it was offsetting itself.
    // The bar then drew on top of the bottom row of icons.
    //
    // Safe to hoist: purr_systemui_init() calls only host->width() and
    // host->height(), both of which are HAL queries with no dependency on the
    // launcher existing. It also builds onto lv_layer_top(), which always
    // composites above lv_scr_act(), so going first costs it no z-order.
    //
    // Called unconditionally — when the module is compiled out it is a stub, so
    // no #ifdef is needed here.
    purr_systemui_init(&s_systemui_host);

    // Both screens still get the true full (w,h): the status bar and nav bar are
    // lv_layer_top() overlays that draw OVER content, and a full-bleed wallpaper
    // behind them is the intended look. What has to avoid them is anything
    // INTERACTIVE — the dock and the All-Apps grid — each of which now insets
    // itself by a height that is finally non-zero at build time.
    build_home_screen(w, h);
    build_lp_launcher(w, h);

    ESP_LOGI(TAG, "launcher built (%d total apps)", app_manager_count());
}

void cupcake_ui_tick(void)
{
    // The launcher itself is fully static once built — every per-tick
    // refresh (status icons, notifications, running apps, idle lock, the
    // bars' auto-hide countdowns) belongs to the system UI.
    purr_systemui_tick();
}

// Thin forwarders so cupcake_hal.c doesn't need to know who owns the lock
// screen — see cupcake.h.
bool cupcake_ui_is_locked(void) { return purr_systemui_is_locked(); }
void cupcake_ui_wake(void)      { purr_systemui_wake(); }
