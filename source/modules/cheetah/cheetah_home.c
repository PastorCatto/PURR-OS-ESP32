// cheetah_home.c — the desktop, XP-style.
//
// BB10/Cheetah's Active Frames + All Apps grid is paused (explicit instruction
// this session) and this file no longer builds either: no more unified
// paging, frame tiles, close glyphs, or the bottom-edge minimize-swipe
// hit-strip. That gesture's job now belongs to systemui_xp.c's taskbar (a
// button per running app, click-to-minimize/restore) — see
// source/modules/systemui/systemui_xp.c's own header comment for the
// systemui/launcher split this follows (taskbar = systemui, desktop =
// launcher). cheetah_hal.c and cheetah_win.c are UNCHANGED by the BB10-to-XP
// pivot itself; cheetah_win.c did separately gain a per-window close button
// this same pass — see its own header comment.
//
// ── Favorites ────────────────────────────────────────────────────────────
// The desktop shows a CURATED subset of the registry, not every app —
// that's what the Start Menu (systemui_xp.c) already covers, so mirroring
// it here would be redundant. Long-press empty desktop space (not an icon —
// LVGL doesn't bubble a child's press to the parent by default, so this
// naturally only fires on the background) reveals a small "Choose
// Favorites..." popup; tapping it opens a checklist of every registered
// app. Persisted to NVS (namespace "cheetah_home", key "favorites") as a
// newline-delimited name list — bounded-substring membership test, no real
// set structure needed at these app counts. First-ever-boot (nothing
// persisted yet) defaults to every app favorited, so the desktop isn't
// empty and the picker is immediately discoverable as how to trim it down;
// an explicitly-emptied list (the user unfavorited everything) is
// respected as-is on every later boot, not re-defaulted.
//
// Touch-only for this pass — the context menu and picker aren't added to
// cheetah_hal_group(). Long-press has no clean trackball/keyboard equivalent
// the way tap-to-launch already does, and this is explicitly a rough draft
// (the user's own framing throughout this session).
//
// Icons/colours are still reused from Mochi (mochi_icon_for_app() /
// mochi_color_for_app()).
//
// No top status strip to lay out under: systemui_xp.c draws nothing at the
// top of the screen (real XP's chrome lives entirely in its bottom
// taskbar), so icons start right at the top edge. The bottom IS reserved,
// via purr_systemui_navbar_height() — the taskbar's real height for this
// style, the same hook every other backend already uses to stay clear of
// its own nav bar.

#include "cheetah.h"
#include "../mochi/mochi.h"
#include "../systemui/systemui.h"
#include "../app_manager/app_manager.h"
#include "../user_mgr/user_mgr.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/purr_win.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdio.h>

// launch()'s install-prompt path (a LOCAL/HYBRID-placed remote app,
// app_manager_decide_placement()) needs app_manager_remote_download()
// and proximity's capability query — both conditionally-empty on
// esp32p4/tab5 (no radio there at all), same guard shape systemui_
// login.c's own "Log in to a server" screen already establishes for the
// identical reason. user_mgr.h has no such gate (universal, no radio
// dependency) and is included unconditionally above.
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#define CHEETAH_HAS_APP_DOWNLOAD 0
#else
#define CHEETAH_HAS_APP_DOWNLOAD 1
#include "../app_manager_remote/app_manager_remote.h"
#include "../proximity/proximity.h"
#include "../claw_loader/claw_loader.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#endif

static const char *TAG = "cheetah_home";

// XP's actual desktop used a solid teal (classic "Bliss"-era desktop teal is
// close to #008080). No real wallpaper asset exists in this tree, so this is
// the same kind of honest flat placeholder systemui_login.c's own
// background already is.
#define COL_BG     lv_color_hex(0x1C7A7A)
#define COL_LABEL  lv_color_hex(0xFFFFFF)

#define ICON_SIZE   40
#define ICON_RADIUS 6
#define LABEL_H     14
#define CELL_W      64
#define CELL_H      64
#define MARGIN      8

#define MAX_ICONS 64

static lv_obj_t *s_screen;
static lv_obj_t *s_icons[MAX_ICONS];
static int       s_icon_app[MAX_ICONS];   // slot -> registry index, since favorites leave gaps
static int       s_icon_count     = 0;
static int       s_last_app_count = -1;
static int       s_rows_per_col   = 1;
static int       s_sel            = 0;    // trackball/keyboard cursor — a SLOT index, not a registry index

static void render_desktop(void);

// ── Favorites persistence ───────────────────────────────────────────────

#define FAV_NVS_NS  "cheetah_home"
#define FAV_NVS_KEY "favorites"
#define FAV_BUF_MAX 512

// Newline-delimited, bounded on both ends ("\nname1\nname2\n") so membership
// is a plain bounded-substring test — see this file's header comment for
// why that's enough at these app counts rather than a real set structure.
static char s_fav_names[FAV_BUF_MAX] = "\n";

// True only if something was actually persisted before (even an explicitly
// empty list) — the caller decides what "nothing yet" defaults to.
static bool fav_load(void)
{
    nvs_handle_t h;
    if (nvs_open(FAV_NVS_NS, NVS_READONLY, &h) != ESP_OK) { strcpy(s_fav_names, "\n"); return false; }
    size_t len = sizeof(s_fav_names);
    esp_err_t r = nvs_get_str(h, FAV_NVS_KEY, s_fav_names, &len);
    nvs_close(h);
    if (r != ESP_OK || s_fav_names[0] != '\n') { strcpy(s_fav_names, "\n"); return false; }
    return true;
}

static void fav_save(void)
{
    nvs_handle_t h;
    if (nvs_open(FAV_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, FAV_NVS_KEY, s_fav_names);
    nvs_commit(h);
    nvs_close(h);
}

static bool fav_has(const char *name)
{
    char pat[64];
    int n = snprintf(pat, sizeof(pat), "\n%s\n", name);
    if (n < 0 || n >= (int)sizeof(pat)) return false;
    return strstr(s_fav_names, pat) != NULL;
}

static void fav_toggle(const char *name)
{
    char pat[64];
    int n = snprintf(pat, sizeof(pat), "\n%s\n", name);
    if (n < 0 || n >= (int)sizeof(pat)) return;

    char *p = strstr(s_fav_names, pat);
    if (p) {
        // Remove "name\n", keeping the leading '\n' in place as the
        // boundary for whatever entry follows.
        size_t remove_len = strlen(name) + 1;
        memmove(p + 1, p + 1 + remove_len, strlen(p + 1 + remove_len) + 1);
    } else {
        size_t cur  = strlen(s_fav_names);
        size_t need = strlen(name) + 1;
        if (cur + need < sizeof(s_fav_names)) {
            strcat(s_fav_names, name);
            strcat(s_fav_names, "\n");
        }
    }
    fav_save();
}

// First-ever-boot only — see this file's header comment.
static void fav_default_all(void)
{
    strcpy(s_fav_names, "\n");
    int n = app_manager_count();
    for (int i = 0; i < n; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (!app) continue;
        size_t cur  = strlen(s_fav_names);
        size_t need = strlen(app->name) + 1;
        if (cur + need < sizeof(s_fav_names)) {
            strcat(s_fav_names, app->name);
            strcat(s_fav_names, "\n");
        }
    }
    fav_save();
}

// ── Launch ───────────────────────────────────────────────────────────────

#if CHEETAH_HAS_APP_DOWNLOAD
// ── Remote app install prompt ───────────────────────────────────────────
// Every remote-listed app not yet downloaded gets asked, every tap, until
// it IS downloaded — "Install locally" vs "Run on server" — see
// launch()'s own comment for why this changed from an app.pcat-declared
// LOCAL/HYBRID gate (app_placement_t defaults every app to REMOTE with
// no author-facing way yet to mark one otherwise, so that gate meant
// NOTHING ever actually asked in practice — a real, reported bug, not
// the intended "ask only for specially-marked apps" design). Same
// "purr_win_* calls are safe from a background task, the portable
// backend already defers internally" precedent this codebase already
// establishes (see milkbar's own retired background task / cheetah_
// win.c's own comment), but this dialog is built/torn down entirely on
// the LVGL/render task (launch() below already runs there, as an
// icon-click callback) — only the actual network download runs on its
// own background task, and that task never touches the dialog's window
// handle at all, sidestepping the question entirely rather than relying
// on it.
static purr_win_t s_install_win = 0;
static char        s_install_name[48];
static uint8_t      s_install_mac[6];
static int          s_install_registry_idx = -1;   // for "Run on server" — app_manager_launch_idx() needs the compacted remote index, not just the name

static bool personal_app_exists(const char *username, const char *name) {
    int n = claw_loader_personal_count(username);
    for (int i = 0; i < n; i++) {
        char existing[48];
        if (claw_loader_personal_at(username, i, existing, sizeof(existing)) &&
            strcmp(existing, name) == 0) return true;
    }
    return false;
}

// Never call directly — only the body of the task on_install_yes() spawns.
// Touches nothing LVGL-side; reports outcome via purr_kernel_notify()
// (thread-safe, no UI dependency) and app_manager_scan()/_launch_by_name()
// (app_manager.c's own local-registry calls, independent of s_remote_mode
// — same bypass the synthetic "Server Manager" entry's own launch already
// uses, see app_manager.c's remote_launch_idx()).
static void install_download_task(void *arg)
{
    (void)arg;
    const char *username = user_mgr_current_user();
    bool ok = app_manager_remote_download(s_install_mac, s_install_name, username);
    if (ok) {
        ESP_LOGI(TAG, "installed '%s' — launching locally", s_install_name);
        purr_kernel_notify("App installed", s_install_name, "cheetah");
        app_manager_scan();   // pick up the freshly-downloaded personal app
        app_manager_launch_by_name(s_install_name);
    } else {
        ESP_LOGW(TAG, "install failed for '%s'", s_install_name);
        purr_kernel_notify("Install failed", s_install_name, "cheetah");
    }
    vTaskDeleteWithCaps(NULL);
}

static void close_install_dialog(void)
{
    if (s_install_win) { purr_win_destroy(s_install_win); s_install_win = 0; }
}

static void on_install_yes(purr_wid_t w, purr_event_t e, void *user)
{
    (void)w; (void)e; (void)user;
    close_install_dialog();
    xTaskCreateWithCaps(install_download_task, "cheetah_dl", 4096, NULL, 3, NULL, MALLOC_CAP_SPIRAM);
}

static void on_install_no(purr_wid_t w, purr_event_t e, void *user)
{
    (void)w; (void)e; (void)user;
    close_install_dialog();
}

// "Run on server" — exactly what a plain remote tap already does
// (app_manager_launch_idx(), the same call launch() falls through to for
// any local or already-decided-remote app below). Choosing this doesn't
// download anything or change anything persistent — the SAME choice is
// offered again next time this app is tapped, since it's still not
// installed. A real, accepted simplification for this pass — see
// launch()'s own comment.
static void on_install_run_remote(purr_wid_t w, purr_event_t e, void *user)
{
    (void)w; (void)e; (void)user;
    close_install_dialog();
    if (s_install_registry_idx >= 0) app_manager_launch_idx(s_install_registry_idx);
}

// offer_remote=false is the one narrow case left where the choice isn't
// really a choice: app->placement == APP_PLACE_LOCAL (author-declared —
// the hand-maintained table in app_manager.c, still real, just no longer
// the ONLY way to reach this dialog at all) says this app can never run
// on the server, so there's nothing to offer alongside Install.
//
// When it IS a real choice, the recommended side is labeled using the
// same capability compare app_manager_decide_placement() already does
// for a HYBRID-declared app — a real recommendation, not a coin flip,
// just no longer the thing that silently DECIDES for the user the way
// an earlier version of this dialog did (see launch()'s own comment on
// why that read as "never asks").
static void open_install_dialog(int registry_idx, const char *name, const uint8_t mac[6], bool offer_remote)
{
    close_install_dialog();   // never orphan a previous prompt — same leak-prevention precedent milkbar's own open_pair_dialog() already established
    snprintf(s_install_name, sizeof(s_install_name), "%s", name);
    memcpy(s_install_mac, mac, 6);
    s_install_registry_idx = registry_idx;

    bool recommend_local = false;
    if (offer_remote) {
        proximity_device_t peer = {0};
        bool have_peer = proximity_find_device_by_mac(mac, &peer);
        bool peer_strong  = have_peer && (peer.caps & PROXIMITY_CAP_STRONG_COMPUTE);
        bool peer_display = have_peer && (peer.caps & PROXIMITY_CAP_HAS_DISPLAY);
        bool my_strong    = (proximity_get_own_caps() & PROXIMITY_CAP_STRONG_COMPUTE) != 0;
        recommend_local = app_manager_decide_placement(APP_PLACE_HYBRID, my_strong, peer_strong, peer_display) == APP_PLACE_LOCAL;
    }

    // 64-byte fixed text + up to 47 bytes of name (s_install_name's own
    // size minus the null) + null = up to 112 — genuinely needs more
    // than a round 96, not a GCC-can't-prove-it case (see this file's
    // own claw_loader_personal_root()-class precedent elsewhere in this
    // codebase for what THAT looks like instead).
    char msg[128];
    if (offer_remote) {
        snprintf(msg, sizeof(msg), "'%s' isn't installed. Install it locally, or run it on the server?", name);
    } else {
        snprintf(msg, sizeof(msg), "Install '%s' from this server?", name);
    }

    s_install_win = purr_win_create("New App");
    purr_win_label(s_install_win, msg);
    purr_wid_t row = purr_win_row(s_install_win, offer_remote ? 3 : 2);
    purr_win_button(s_install_win, recommend_local ? "Install (recommended)" : "Install", on_install_yes, NULL);
    if (offer_remote) {
        purr_win_button(s_install_win, recommend_local ? "Run on server" : "Run on server (recommended)",
                         on_install_run_remote, NULL);
    }
    purr_win_button(s_install_win, "Cancel", on_install_no, NULL);
    purr_win_layout_end(row);
    purr_win_show(s_install_win);
}
#endif // CHEETAH_HAS_APP_DOWNLOAD

static void launch(int registry_idx)
{
    const app_entry_t *app = app_manager_get(registry_idx);
    if (!app) return;
    ESP_LOGI(TAG, "launching '%s' (idx=%d)", app->name, registry_idx);

#if CHEETAH_HAS_APP_DOWNLOAD
    // Any remote-listed app: already installed locally (an earlier tap's
    // choice) launches the local copy directly, no re-asking. Otherwise
    // — regardless of app->placement, see open_install_dialog()'s own
    // comment on why this no longer gates on that — every tap offers the
    // real choice: Install, or Run on server. This is the direct fix for
    // the reported bug: gating the dialog on an app.pcat-declared LOCAL/
    // HYBRID placement meant it never actually showed for anything,
    // since nothing had a way to make that declaration yet.
    uint8_t remote_mac[6];
    if (app_manager_is_remote() && app_manager_remote_mac(remote_mac)) {
        const char *username = user_mgr_current_user();
        if (personal_app_exists(username, app->name)) {
            // Same bypass reasoning install_download_task() itself uses
            // — app_manager_launch_by_name() is the local-registry call,
            // independent of s_remote_mode.
            app_manager_launch_by_name(app->name);
        } else {
            open_install_dialog(registry_idx, app->name, remote_mac, app->placement != APP_PLACE_LOCAL);
        }
        purr_systemui_enter_app(registry_idx);
        return;
    }
#endif

    if (app->state == APP_STATE_RUNNING && app->window) {
        purr_win_show(app->window);
    } else {
        app_manager_launch_idx(registry_idx);
    }
    purr_systemui_enter_app(registry_idx);
}

// ── Icon grid ────────────────────────────────────────────────────────────

static void apply_selection_style(lv_obj_t *obj, bool selected)
{
    if (!obj) return;
    lv_obj_set_style_outline_width(obj, selected ? 3 : 0, 0);
    lv_obj_set_style_outline_color(obj, lv_color_white(), 0);
    lv_obj_set_style_outline_pad(obj, selected ? 3 : 0, 0);
    lv_obj_set_style_outline_opa(obj, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

// Always visible (no touch-hides-cursor nuance) — see the old BB10-era
// file's identical comment on this same tradeoff: icons here are sparse
// enough that a lingering ring is harmless, and it's the only feedback a
// keyboard/trackball user gets at all.
static void render_selection(void)
{
    for (int i = 0; i < s_icon_count; i++) {
        apply_selection_style(s_icons[i], i == s_sel);
    }
}

static void icon_click_cb(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (slot < 0 || slot >= s_icon_count) return;
    s_sel = slot;
    render_selection();
    launch(s_icon_app[slot]);
}

static void build_icon(lv_obj_t *parent, int registry_idx, int slot, lv_coord_t x, lv_coord_t y)
{
    const app_entry_t *app = app_manager_get(registry_idx);
    if (!app) return;

    lv_obj_t *sq = lv_obj_create(parent);
    lv_obj_remove_style_all(sq);
    lv_obj_set_size(sq, ICON_SIZE, ICON_SIZE);
    lv_obj_set_pos(sq, x, y);
    purr_fx_radius(sq, ICON_RADIUS);
    lv_obj_set_style_bg_color(sq, mochi_color_for_app(app->name), 0);
    lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sq, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sq, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sq, icon_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)slot);

    lv_obj_t *glyph = lv_img_create(sq);
    lv_img_set_src(glyph, mochi_icon_for_app(app->name));
    lv_img_set_zoom(glyph, (uint16_t)(((ICON_SIZE * 55 / 100) * 256) / 48));
    lv_obj_center(glyph);
    lv_obj_clear_flag(glyph, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, app->name);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl, (lv_coord_t)(CELL_W - 4));
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lbl, COL_LABEL, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(lbl, (lv_coord_t)(x + ICON_SIZE / 2 - (CELL_W - 4) / 2),
                         (lv_coord_t)(y + ICON_SIZE + 2));
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    if (s_icon_count < MAX_ICONS) {
        s_icons[s_icon_count]    = sq;
        s_icon_app[s_icon_count] = registry_idx;
        s_icon_count++;
    }
}

static void render_desktop(void)
{
    if (!s_screen) return;
    lv_obj_clean(s_screen);
    s_icon_count = 0;

    uint16_t usable_h = (uint16_t)(cheetah_hal_height() - purr_systemui_navbar_height() - MARGIN);
    s_rows_per_col = (int)(usable_h / CELL_H);
    if (s_rows_per_col < 1) s_rows_per_col = 1;

    int n = app_manager_count();
    int placed = 0;
    // Remote mode (app_manager_is_remote(), see app_manager.h) shows EVERY
    // app the connected server reports, bypassing the favorites curation
    // below entirely — a persisted-by-NAME local favorites list has no
    // meaningful relationship to a transient remote app list, and Milkbar's
    // whole point in sending someone here is "show me what I can run on
    // that server," not a subset of it.
    bool remote = app_manager_is_remote();
    for (int i = 0; i < n && placed < MAX_ICONS; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (!app || (!remote && !fav_has(app->name))) continue;   // desktop = favorites only, see header comment (local mode)
        int col = placed / s_rows_per_col;
        int row = placed % s_rows_per_col;
        lv_coord_t x = (lv_coord_t)(MARGIN + col * CELL_W);
        lv_coord_t y = (lv_coord_t)(MARGIN + row * CELL_H);
        build_icon(s_screen, i, placed, x, y);
        placed++;
    }

    if (s_sel >= s_icon_count) s_sel = s_icon_count > 0 ? s_icon_count - 1 : 0;
    render_selection();
}

// ── Keyboard/trackball navigation ───────────────────────────────────────
// Column-major over the RENDERED slots (s_icon_app[] maps a slot back to
// its registry index) — see build_icon()/render_desktop() for the same
// col/row math this mirrors. Raw arrow keys, not LV_KEY_NEXT/PREV group-
// stepping: the trackball emits real arrow keys (cheetah_hal.c's
// ball_read_cb), and LVGL's default group navigation only responds to
// NEXT/PREV — a single focused root object with its own cursor state is
// what every other grid in this codebase this session already settled on.
static void desktop_key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (s_icon_count == 0) return;
    int col = (s_rows_per_col > 0) ? s_sel / s_rows_per_col : 0;

    switch (key) {
        case LV_KEY_LEFT: case LV_KEY_PREV: {
            int cand = s_sel - s_rows_per_col;
            if (cand >= 0) s_sel = cand;
            break;
        }
        case LV_KEY_RIGHT: case LV_KEY_NEXT: {
            int cand = s_sel + s_rows_per_col;
            if (cand < s_icon_count) s_sel = cand;
            break;
        }
        case LV_KEY_UP:
            if (s_sel > 0 && (s_sel - 1) / s_rows_per_col == col) s_sel--;
            break;
        case LV_KEY_DOWN:
            if (s_sel + 1 < s_icon_count && (s_sel + 1) / s_rows_per_col == col) s_sel++;
            break;
        case LV_KEY_ENTER:
            launch(s_icon_app[s_sel]);
            return;
        default:
            return;
    }
    render_selection();
}

// ── Long-press context menu + favorites picker ──────────────────────────

static lv_obj_t *s_context_backdrop;
static lv_obj_t *s_context_menu;

static lv_obj_t *s_picker_backdrop;
static lv_obj_t *s_picker_win;
static lv_obj_t *s_picker_list;

static void close_context_menu(void)
{
    lv_obj_add_flag(s_context_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_context_menu, LV_OBJ_FLAG_HIDDEN);
}

static void context_backdrop_click_cb(lv_event_t *e) { (void)e; close_context_menu(); }

static void picker_row_click_cb(lv_event_t *e);

static void rebuild_picker_list(void)
{
    if (!s_picker_list) return;
    lv_obj_clean(s_picker_list);

    int n = app_manager_count();
    for (int i = 0; i < n; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (!app) continue;
        bool on = fav_has(app->name);

        lv_obj_t *row = lv_obj_create(s_picker_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 34);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_left(row, 8, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, picker_row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *icon = lv_img_create(row);
        lv_img_set_src(icon, mochi_icon_for_app(app->name));
        lv_img_set_zoom(icon, ICON_ZOOM(20));
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, app->name);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, 140);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 26, 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *check = lv_obj_create(row);
        lv_obj_remove_style_all(check);
        lv_obj_set_size(check, 18, 18);
        lv_obj_align(check, LV_ALIGN_RIGHT_MID, -6, 0);
        lv_obj_set_style_radius(check, 3, 0);
        lv_obj_set_style_border_width(check, 2, 0);
        lv_obj_set_style_border_color(check, lv_color_hex(0x3C9A3C), 0);
        lv_obj_set_style_bg_color(check, lv_color_hex(0x3C9A3C), 0);
        lv_obj_set_style_bg_opa(check, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(check, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(check, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void picker_row_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const app_entry_t *app = app_manager_get(idx);
    if (!app) return;
    fav_toggle(app->name);
    rebuild_picker_list();
}

static void open_picker(void)
{
    rebuild_picker_list();
    lv_obj_clear_flag(s_picker_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_picker_win, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_picker_backdrop);
    lv_obj_move_foreground(s_picker_win);
}

static void choose_favorites_click_cb(lv_event_t *e)
{
    (void)e;
    close_context_menu();
    open_picker();
}

static void desktop_longpress_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_clear_flag(s_context_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_context_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_context_backdrop);
    lv_obj_move_foreground(s_context_menu);
}

static void picker_done_click_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_picker_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_picker_win, LV_OBJ_FLAG_HIDDEN);
    render_desktop();   // reflect whatever changed immediately
}

static void build_context_menu(uint16_t w, uint16_t h)
{
    s_context_backdrop = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_context_backdrop);
    lv_obj_set_size(s_context_backdrop, w, h);
    lv_obj_set_pos(s_context_backdrop, 0, 0);
    lv_obj_set_style_bg_opa(s_context_backdrop, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_context_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_context_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_context_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_context_backdrop, context_backdrop_click_cb, LV_EVENT_CLICKED, NULL);

    s_context_menu = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_context_menu);
    lv_obj_set_size(s_context_menu, 160, 36);
    lv_obj_center(s_context_menu);
    lv_obj_set_style_bg_color(s_context_menu, lv_color_hex(0xECECEC), 0);
    lv_obj_set_style_bg_opa(s_context_menu, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_context_menu, 4, 0);
    lv_obj_set_style_border_color(s_context_menu, lv_color_hex(0x808080), 0);
    lv_obj_set_style_border_width(s_context_menu, 1, 0);
    lv_obj_clear_flag(s_context_menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_context_menu, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_context_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_context_menu, choose_favorites_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(s_context_menu);
    lv_label_set_text(lbl, "Choose Favorites...");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
}

static void build_picker(uint16_t w, uint16_t h)
{
    // Opaque-ish scrim, deliberately NOT dismissible by tapping it — losing
    // picks by a stray outside tap would be a worse rough edge than making
    // Done the one way out.
    s_picker_backdrop = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_picker_backdrop);
    lv_obj_set_size(s_picker_backdrop, w, h);
    lv_obj_set_pos(s_picker_backdrop, 0, 0);
    lv_obj_set_style_bg_color(s_picker_backdrop, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_picker_backdrop, LV_OPA_50, 0);
    lv_obj_clear_flag(s_picker_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_picker_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_picker_backdrop, LV_OBJ_FLAG_HIDDEN);

    lv_coord_t pw = (lv_coord_t)(w * 4 / 5);
    lv_coord_t ph = (lv_coord_t)(h * 75 / 100);
    s_picker_win = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_picker_win);
    lv_obj_set_size(s_picker_win, pw, ph);
    lv_obj_center(s_picker_win);
    lv_obj_set_style_bg_color(s_picker_win, lv_color_hex(0xECECEC), 0);
    lv_obj_set_style_bg_opa(s_picker_win, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_picker_win, lv_color_hex(0x808080), 0);
    lv_obj_set_style_border_width(s_picker_win, 1, 0);
    lv_obj_clear_flag(s_picker_win, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_picker_win, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = lv_label_create(s_picker_win);
    lv_label_set_text(title, "Choose Favorites");
    lv_obj_set_style_text_color(title, lv_color_hex(0x000000), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 6, 4);

    lv_coord_t list_y = 28;
    lv_coord_t list_h = (lv_coord_t)(ph - list_y - 36);
    s_picker_list = lv_obj_create(s_picker_win);
    lv_obj_remove_style_all(s_picker_list);
    lv_obj_set_size(s_picker_list, (lv_coord_t)(pw - 8), list_h);
    lv_obj_set_pos(s_picker_list, 4, list_y);
    lv_obj_set_layout(s_picker_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_picker_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_picker_list, LV_DIR_VER);
    lv_obj_set_style_pad_row(s_picker_list, 2, 0);

    lv_obj_t *done_btn = lv_btn_create(s_picker_win);
    lv_obj_set_size(done_btn, 70, 28);
    lv_obj_align(done_btn, LV_ALIGN_BOTTOM_RIGHT, -6, -6);
    lv_obj_add_event_cb(done_btn, picker_done_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *done_lbl = lv_label_create(done_btn);
    lv_label_set_text(done_lbl, "Done");
    lv_obj_center(done_lbl);
}

// ── Hosting the system UI ───────────────────────────────────────────────

static lv_color_t cheetah_tint_color(const char *name, uint8_t base)
{
    (void)base;
    return mochi_color_for_app(name);
}

// No drawer/page concept left — the desktop just re-renders, same as a
// registry-change refresh would.
static void cheetah_hide_drawer(void)
{
    render_desktop();
}

static const purr_systemui_host_t s_systemui_host = {
    .width                   = cheetah_hal_width,
    .height                  = cheetah_hal_height,
    .icon_for_app            = mochi_icon_for_app,
    .tint_color              = cheetah_tint_color,
    .hide_drawer             = cheetah_hide_drawer,
    .hide_foreground_windows = cheetah_win_hide_foreground,
    .last_activity_ms        = cheetah_hal_last_activity_ms,
    .wallpaper               = NULL,   // no wallpaper asset — see this file's COL_BG comment
    .suppress_navbar         = false,  // moot for the XP style — see systemui_xp.c's header comment
    .group                   = cheetah_hal_group,
};

// ── Public API ───────────────────────────────────────────────────────────

void cheetah_home_go_home(void)
{
    if (!s_screen) return;
    lv_obj_move_foreground(s_screen);
    purr_systemui_return_home();

    lv_group_t *g = cheetah_hal_group();
    if (g) lv_group_focus_obj(s_screen);
}

void cheetah_home_tick(void)
{
    purr_systemui_tick();

    int n = app_manager_count();
    if (n != s_last_app_count) {
        s_last_app_count = n;
        render_desktop();
    }
}

void cheetah_home_init(void)
{
    uint16_t w = cheetah_hal_width();
    uint16_t h = cheetah_hal_height();

    if (!fav_load()) fav_default_all();   // first-ever-boot — see header comment

    s_screen = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, w, h);
    lv_obj_set_pos(s_screen, 0, 0);
    lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);   // needed for LONG_PRESSED to fire on empty space
    lv_obj_add_event_cb(s_screen, desktop_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(s_screen, desktop_longpress_cb, LV_EVENT_LONG_PRESSED, NULL);

    lv_group_t *g = cheetah_hal_group();
    if (g) lv_group_add_obj(g, s_screen);

    s_last_app_count = app_manager_count();
    render_desktop();

    build_context_menu(w, h);
    build_picker(w, h);

    // Taskbar, Start Menu, tray, idle lock — with the XP style's own
    // purr_systemui_navbar_height() already accounted for above.
    purr_systemui_init(&s_systemui_host);

    cheetah_home_go_home();

    ESP_LOGI(TAG, "Desktop built (%ux%u, %d/%d apps favorited)", w, h, s_icon_count, app_manager_count());
}
