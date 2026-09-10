// tabby_shell.c — Tabby's home screen: a type-to-filter app launcher.
//
// ── The interaction model ───────────────────────────────────────────────────
// There is no search box to open and no mode to enter. The shell is always
// listening:
//
//   any printable key   append to the filter, list narrows live
//   backspace           delete one filter character (empty filter = no-op)
//   esc                 clear the filter; if already clear, nothing to undo
//   up / down           move the selection
//   trackball roll      move the selection (encoder steps, see tabby_hal.c)
//   enter / ball click  launch the selected app
//   touch on a row      select it and launch it
//
// Filtering is a case-insensitive substring match on the app name, not a
// prefix match — "man" finds both "app_manager" and "fileman", which matters
// when app names share prefixes ("mesh*"). Selection is clamped back into
// range on every re-filter so the highlight can never point past the end of a
// narrowed list.
//
// ── Layout (tuned for 320x240 landscape) ────────────────────────────────────
//   ┌──────────────────────────────┐
//   │ status bar                   │  owned by the systemui module, drawn on
//   ├──────────────────────────────┤  lv_layer_top() above everything here
//   │ filter bar                   │  FILTER_H
//   ├──────────────────────────────┤
//   │ ▸ app row (selected)         │  ROW_H each, scrolls
//   │   app row                    │
//   │   ...                        │
//   ├──────────────────────────────┤
//   │ hint bar                     │  HINT_H, sits above the system nav bar
//   └──────────────────────────────┘
//
// The shell claims the true full screen: the system UI's status bar and nav
// bar are lv_layer_top() overlays that draw *over* content rather than the
// layout reserving space for them. Only the hint bar offsets itself, by
// purr_systemui_navbar_height(), so it isn't hidden underneath the nav bar —
// and that call returns 0 when the system UI is compiled out, which correctly
// reclaims the space instead of leaving a gap.
//
// ── Rendering ───────────────────────────────────────────────────────────────
// Everything below the "Rendering" divider is the only part that knows it is
// talking to LVGL. It uses three primitives — filled rect, text at a point,
// and a highlight bar — plus one scroll-into-view. A direct-framebuffer
// renderer (RGB565 straight into a PSRAM buffer, one push_pixels() per dirty
// band, no object tree and no compositor) would replace exactly that section
// and nothing else: the filter/selection/launch logic above it holds no LVGL
// state at all, only an index and a char buffer.
//
// What such a renderer would NOT cover, and why this is still LVGL today: app
// windows go through the full catcall_ui_t widget contract (tabby_win.c), and
// a framebuffer backend would have to reimplement text layout, clipping, and
// scrolling for every one of those widgets to host real apps. Fast-path the
// shell first, keep LVGL for app content — those can coexist, but only if the
// shell's pixels and LVGL's are not both live at once, which is why this is a
// deliberate later step rather than a flag.

#include "tabby.h"
#include "../systemui/systemui.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/purr_win.h"
#include "../app_manager/app_manager.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static const char *TAG = "tabby_shell";

#define FILTER_H   24
#define HINT_H     18
#define ROW_H      26
#define MAX_APPS   64
#define FILTER_MAX 24

// Palette — deliberately dark and flat. Flat fills are the cheapest thing to
// push over SPI (no gradients to recompute per band) and read well on the
// T-Deck's small panel in daylight.
#define COL_BG        lv_color_make(0x0E, 0x10, 0x16)
#define COL_BAR       lv_color_make(0x1A, 0x1E, 0x28)
#define COL_SEL       lv_color_make(0x2E, 0x6F, 0xD8)
#define COL_TEXT      lv_color_white()
#define COL_TEXT_DIM  lv_color_make(0x8A, 0x90, 0xA0)
#define COL_ACCENT    lv_color_make(0x6E, 0xC8, 0x7A)

// ── Shell state (no LVGL types — see the Rendering note in this file's head) ─

static char s_filter[FILTER_MAX + 1] = "";
static int  s_filter_len = 0;

// Indices into app_manager's registry that currently pass the filter, in
// registry order. s_match_count == 0 means "nothing matches", which is a real
// state the list has to render (not an error).
static int s_match[MAX_APPS];
static int s_match_count = 0;
static int s_sel = 0;             // index into s_match, not into the registry

// Registry size at last rebuild — cheap way to notice apps appearing at
// runtime (pkg app install) without rebuilding the list every tick.
static int s_last_app_count = -1;

static bool name_matches(const char *name, const char *needle, int needle_len)
{
    if (needle_len == 0) return true;
    for (const char *p = name; *p; p++) {
        int i = 0;
        while (i < needle_len && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == needle_len) return true;
    }
    return false;
}

static void rebuild_matches(void)
{
    s_match_count = 0;
    int n = app_manager_count();
    if (n > MAX_APPS) n = MAX_APPS;
    for (int i = 0; i < n; i++) {
        const app_entry_t *app = app_manager_get(i);
        // app->name is a fixed char[48], never NULL — an empty first byte is
        // the real "unnamed entry" case to skip.
        if (!app || app->name[0] == '\0') continue;
        if (name_matches(app->name, s_filter, s_filter_len)) {
            s_match[s_match_count++] = i;
        }
    }
    // Clamp rather than reset to 0: narrowing a filter one character at a time
    // should keep the highlight where it is whenever that row still matches,
    // instead of snapping back to the top on every keystroke.
    if (s_sel >= s_match_count) s_sel = s_match_count > 0 ? s_match_count - 1 : 0;
    if (s_sel < 0)              s_sel = 0;
}

// ── Rendering ───────────────────────────────────────────────────────────────
// The LVGL-specific half. See this file's header comment for what a
// framebuffer renderer would replace.

static lv_obj_t *s_screen;
static lv_obj_t *s_filter_lbl;
static lv_obj_t *s_hint_lbl;
static lv_obj_t *s_list;          // scrollable container of row objects
static lv_obj_t *s_rows[MAX_APPS];
static int       s_row_count = 0;

static void row_click_cb(lv_event_t *e);

static void render_filter_bar(void)
{
    if (!s_filter_lbl) return;
    char buf[FILTER_MAX + 32];
    if (s_filter_len > 0) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_KEYBOARD " %s", s_filter);
        lv_obj_set_style_text_color(s_filter_lbl, COL_ACCENT, 0);
    } else if (tabby_hal_has_physical_keyboard()) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_KEYBOARD " type to search");
        lv_obj_set_style_text_color(s_filter_lbl, COL_TEXT_DIM, 0);
    } else {
        snprintf(buf, sizeof(buf), LV_SYMBOL_LIST " apps");
        lv_obj_set_style_text_color(s_filter_lbl, COL_TEXT_DIM, 0);
    }
    lv_label_set_text(s_filter_lbl, buf);
}

static void render_hint_bar(void)
{
    if (!s_hint_lbl) return;
    if (s_match_count == 0) {
        lv_label_set_text(s_hint_lbl, "no match  \xE2\x80\xA2  esc clears");
    } else if (s_filter_len > 0) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%d match%s  \xE2\x80\xA2  enter opens  \xE2\x80\xA2  esc clears",
                 s_match_count, s_match_count == 1 ? "" : "es");
        lv_label_set_text(s_hint_lbl, buf);
    } else {
        lv_label_set_text(s_hint_lbl, "roll/arrows move  \xE2\x80\xA2  enter opens");
    }
}

// Repaints the highlight without rebuilding rows — the hot path, since it runs
// on every selection step.
static void render_selection(void)
{
    for (int i = 0; i < s_row_count; i++) {
        if (!s_rows[i]) continue;
        bool sel = (i == s_sel);
        lv_obj_set_style_bg_color(s_rows[i], sel ? COL_SEL : COL_BG, 0);
        lv_obj_set_style_bg_opa(s_rows[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_t *lbl = lv_obj_get_child(s_rows[i], 0);
        if (lbl) lv_obj_set_style_text_color(lbl, sel ? COL_TEXT : COL_TEXT_DIM, 0);
    }
    if (s_sel >= 0 && s_sel < s_row_count && s_rows[s_sel]) {
        lv_obj_scroll_to_view(s_rows[s_sel], LV_ANIM_OFF);
    }
}

// Full row rebuild — only on filter change or registry change, never per tick.
static void render_rows(void)
{
    if (!s_list) return;
    lv_obj_clean(s_list);
    s_row_count = 0;

    if (s_match_count == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, "nothing matches");
        lv_obj_set_style_text_color(empty, COL_TEXT_DIM, 0);
        return;
    }

    for (int i = 0; i < s_match_count; i++) {
        const app_entry_t *app = app_manager_get(s_match[i]);
        if (!app) continue;

        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), ROW_H);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, app->name);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, LV_PCT(92));
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        // A running app gets a marker, so the list doubles as "what's open"
        // without needing a separate task switcher for the common case.
        if (app->state == APP_STATE_RUNNING) {
            lv_obj_t *dot = lv_label_create(row);
            lv_label_set_text(dot, LV_SYMBOL_PLAY);
            lv_obj_set_style_text_color(dot, COL_ACCENT, 0);
            lv_obj_set_style_text_font(dot, &lv_font_montserrat_14, 0);
            lv_obj_align(dot, LV_ALIGN_RIGHT_MID, -8, 0);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        }

        if (s_row_count < MAX_APPS) s_rows[s_row_count++] = row;
    }
    render_selection();
}

static void render_all(void)
{
    render_filter_bar();
    render_rows();
    render_hint_bar();
}

// ── Actions ─────────────────────────────────────────────────────────────────

static void launch_selected(void)
{
    if (s_sel < 0 || s_sel >= s_match_count) return;
    int idx = s_match[s_sel];
    const app_entry_t *app = app_manager_get(idx);
    if (!app) return;

    ESP_LOGI(TAG, "launching '%s' (registry idx=%d)", app->name, idx);
    // Already running with a tracked window: restore rather than relaunch.
    // app_manager_launch_idx() no-ops on an already-RUNNING app, so without
    // this an open-but-hidden app's row would look dead.
    if (app->state == APP_STATE_RUNNING && app->window) {
        purr_win_show(app->window);
    } else {
        app_manager_launch_idx(idx);
    }
    // Hands foreground tracking and the bars' auto-hide to the system UI,
    // which owns both.
    purr_systemui_enter_app(idx);
}

static void row_click_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_match_count) return;
    s_sel = i;
    render_selection();
    launch_selected();
}

static void move_selection(int delta)
{
    if (s_match_count == 0) return;
    s_sel += delta;
    // Clamp, don't wrap: on a filtered list, wrapping past the end back to the
    // top is disorienting when you can't see both ends at once.
    if (s_sel < 0)              s_sel = 0;
    if (s_sel >= s_match_count) s_sel = s_match_count - 1;
    render_selection();
}

static void filter_append(char c)
{
    if (s_filter_len >= FILTER_MAX) return;
    s_filter[s_filter_len++] = c;
    s_filter[s_filter_len]   = '\0';
    rebuild_matches();
    render_all();
}

static void filter_backspace(void)
{
    if (s_filter_len == 0) return;
    s_filter[--s_filter_len] = '\0';
    rebuild_matches();
    render_all();
}

static void filter_clear(void)
{
    if (s_filter_len == 0) return;
    s_filter[0]  = '\0';
    s_filter_len = 0;
    rebuild_matches();
    render_all();
}

// ── Input ───────────────────────────────────────────────────────────────────
// The shell root is a focusable object in the HAL's shared group, so both the
// keypad and the encoder deliver here whenever no app widget holds focus.
// LV_EVENT_KEY carries keypad characters; LV_EVENT_PRESSED on an encoder is
// the ball click; encoder steps arrive as LV_KEY_LEFT/RIGHT/UP/DOWN because
// LVGL translates enc_diff for a focused object that isn't in edit mode.

static void shell_key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);

    switch (key) {
        case LV_KEY_UP:
        case LV_KEY_LEFT:
        case LV_KEY_PREV:
            move_selection(-1);
            return;
        case LV_KEY_DOWN:
        case LV_KEY_RIGHT:
        case LV_KEY_NEXT:
            move_selection(+1);
            return;
        case LV_KEY_ENTER:
            launch_selected();
            return;
        case LV_KEY_BACKSPACE:
            filter_backspace();
            return;
        case LV_KEY_ESC:
            filter_clear();
            return;
        default:
            break;
    }

    // Printable ASCII becomes filter text. Anything outside that range is a
    // control/navigation code this shell has no meaning for — dropped rather
    // than inserted, so a stray keycode can't corrupt the filter string.
    if (key >= 0x20 && key < 0x7F) filter_append((char)key);
}

// Encoder click. LVGL sends LV_EVENT_PRESSED to the focused object for an
// encoder button press; treating it as "open" matches the physical
// affordance (press the thing you just rolled to).
static void shell_pressed_cb(lv_event_t *e)
{
    (void)e;
    launch_selected();
}

// ── Hosting the system UI ───────────────────────────────────────────────────
// Tabby is the second host for source/modules/systemui/, after Cupcake. The
// hook table is the entire coupling — no LVGL objects cross it in either
// direction.
//
// icon_for_app/tint_color feed the Recents cards. Tabby has no bundled bitmap
// icon set of its own (the launcher is text-first by design), so it reuses
// blackpurr's shared 48px icons the same way Cupcake does, and derives a
// per-app tint by hashing the name so the same app reads as the same colour
// everywhere it appears.

static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u; // FNV-1a
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static lv_color_t tabby_tint_color(const char *name, uint8_t base)
{
    uint32_t h = hash_str(name);
    uint8_t r = (uint8_t)(base + ((h >> 0)  & 0x1F));
    uint8_t g = (uint8_t)(base + ((h >> 8)  & 0x1F));
    uint8_t b = (uint8_t)(base + ((h >> 16) & 0x1F));
    return lv_color_make(r, g, b);
}

static const lv_img_dsc_t *tabby_icon_for_app(const char *name)
{
    extern const lv_img_dsc_t bp_icon_settings_48, bp_icon_about_48,
                              bp_icon_terminal_48, bp_icon_fileman_48,
                              bp_icon_calculator_48, bp_icon_tools_48;
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
    return &bp_icon_tools_48;
}

// Tabby's home screen has no separate app-drawer overlay to dismiss — the app
// list *is* the home screen — so the system UI's "return home" path only needs
// the filter reset, which go_home() does.
static void tabby_hide_drawer(void)
{
    filter_clear();
}

static const purr_systemui_host_t s_systemui_host = {
    .width                   = tabby_hal_width,
    .height                  = tabby_hal_height,
    .icon_for_app            = tabby_icon_for_app,
    .tint_color              = tabby_tint_color,
    .hide_drawer             = tabby_hide_drawer,
    .hide_foreground_windows = tabby_win_hide_foreground,
    .last_activity_ms        = tabby_hal_last_activity_ms,
    // Lets systemui's own login screen (systemui_login.c) join the same
    // group tabby_hal.c's keyboard/trackball indevs are bound to, so a
    // physical keyboard can type a password there too, not just tap.
    .group                   = tabby_hal_group,
};

// ── Public API ──────────────────────────────────────────────────────────────

void tabby_shell_go_home(void)
{
    filter_clear();
    rebuild_matches();
    render_all();
    lv_group_t *g = tabby_hal_group();
    if (g && s_screen) {
        lv_group_focus_obj(s_screen);
        // Encoder motion only reaches the focused object as LV_KEY_LEFT/RIGHT
        // while its group is in editing mode; otherwise LVGL consumes enc_diff
        // as lv_group_focus_next/prev (lv_indev.c's indev_encoder_proc). With
        // the shell root as the only group member there is nowhere to step, so
        // without this the trackball does nothing at all — confirmed on
        // hardware. Keypad input is unaffected either way.
        lv_group_set_editing(g, true);
    }
}

void tabby_shell_init(void)
{
    uint16_t w = tabby_hal_width();
    uint16_t h = tabby_hal_height();

    // Full-screen root. Focusable and in the shared group so keypad/encoder
    // events land here whenever no app widget has focus.
    s_screen = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, w, h);
    lv_obj_set_pos(s_screen, 0, 0);
    lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_screen, shell_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(s_screen, shell_pressed_cb, LV_EVENT_PRESSED, NULL);

    // Filter bar — sits just below the system status strip.
    lv_obj_t *fbar = lv_obj_create(s_screen);
    lv_obj_remove_style_all(fbar);
    lv_obj_set_size(fbar, w, FILTER_H);
    lv_obj_set_pos(fbar, 0, PURR_SYSTEMUI_STATUS_H);
    lv_obj_set_style_bg_color(fbar, COL_BAR, 0);
    lv_obj_set_style_bg_opa(fbar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(fbar, LV_OBJ_FLAG_SCROLLABLE);

    s_filter_lbl = lv_label_create(fbar);
    lv_obj_set_style_text_font(s_filter_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_filter_lbl, LV_ALIGN_LEFT_MID, 8, 0);

    // App list. Height is whatever's left between the filter bar and the hint
    // bar, with the hint bar itself lifted clear of the system nav bar.
    lv_coord_t list_y = PURR_SYSTEMUI_STATUS_H + FILTER_H;
    lv_coord_t hint_y = (lv_coord_t)(h - purr_systemui_navbar_height() - HINT_H);
    lv_coord_t list_h = (lv_coord_t)(hint_y - list_y);
    if (list_h < ROW_H) list_h = ROW_H;   // degenerate panel guard

    s_list = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, w, list_h);
    lv_obj_set_pos(s_list, 0, list_y);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_list, 4, 0);
    lv_obj_set_style_pad_row(s_list, 2, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_layout(s_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // Hint bar.
    lv_obj_t *hbar = lv_obj_create(s_screen);
    lv_obj_remove_style_all(hbar);
    lv_obj_set_size(hbar, w, HINT_H);
    lv_obj_set_pos(hbar, 0, hint_y);
    lv_obj_set_style_bg_color(hbar, COL_BAR, 0);
    lv_obj_set_style_bg_opa(hbar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(hbar, LV_OBJ_FLAG_SCROLLABLE);

    s_hint_lbl = lv_label_create(hbar);
    lv_obj_set_style_text_font(s_hint_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hint_lbl, COL_TEXT_DIM, 0);
    lv_obj_align(s_hint_lbl, LV_ALIGN_LEFT_MID, 8, 0);

    lv_group_t *g = tabby_hal_group();
    if (g) {
        lv_group_add_obj(g, s_screen);
        lv_group_focus_obj(s_screen);
        lv_group_set_editing(g, true);   // see tabby_shell_go_home()
    }

    rebuild_matches();
    s_last_app_count = app_manager_count();
    render_all();

    // Status bar, panels, nav bar, Recents, lock — the systemui module, same
    // one Cupcake hosts. Unconditional: it stubs itself out when compiled off.
    purr_systemui_init(&s_systemui_host);

    ESP_LOGI(TAG, "shell built (%dx%d, %d apps)", w, h, app_manager_count());
}

void tabby_shell_tick(void)
{
    purr_systemui_tick();

    // Pick up apps appearing/disappearing at runtime, and running-state
    // changes that alter a row's marker. Only rebuilds when the registry size
    // actually moved — a full row rebuild every 200ms would fight scrolling.
    int n = app_manager_count();
    if (n != s_last_app_count) {
        s_last_app_count = n;
        rebuild_matches();
        render_all();
    }
}
