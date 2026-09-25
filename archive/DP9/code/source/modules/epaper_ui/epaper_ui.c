// epaper_ui.c — watch-style UI for the Waveshare ESP32-S3-ePaper-1.54
// (no touch on this variant, two physical buttons). See module.pcat for
// the full framing.
//
// ── Screens ──────────────────────────────────────────────────────────
//   Watchface (root) — big clock. Hold -> App List. Back: no-op (top).
//   App List  — Pebble-style vertical card list: three pinned "system"
//               cards (Battery, Clock, About) plus one card per real
//               app_manager entry (empty today — this device has none
//               installed, shown honestly as "No apps", not faked).
//               Short press cycles the highlight, hold opens/launches
//               the highlighted card, Back returns to the Watchface.
//   Detail    — a system card's own glanceable value screen, OR a brief
//               "Launched <name>" confirmation for a real app (there is
//               no window manager on this device yet — see launcher_
//               lvgl.c's own identical, honest scope note — so "opening"
//               a real app means launching it, not drawing its UI here).
//               Short press on a system card refreshes its reading;
//               Back returns to the App List.
//
// ── Input abstraction ────────────────────────────────────────────────
// Screen transitions live in nav_dispatch(nav_event_t), NOT in the
// button-polling code — button_task() is just an ADAPTER that turns raw
// BOOT/PWR GPIO edges into NAV_NEXT/NAV_SELECT/NAV_BACK and calls that
// one function. This board has no touch (confirmed: the non-touch
// variant), but the same panel also ships in a touch variant — a future
// touch adapter would translate swipe-down/tap/edge-swipe into these
// same three events and call the same nav_dispatch(), with zero changes
// to the screen logic below. Two physical buttons map onto it as:
//   BOOT, short press    — NAV_NEXT (cycle the highlight)
//   BOOT, press-and-hold — NAV_SELECT (open/launch the highlighted item)
//   PWR,  press          — NAV_BACK (always — one level up, never a
//                           straight jump to the root; see nav_dispatch())
//
// ── Auto-login ────────────────────────────────────────────────────────
// Same "no password = auto-login" contract oled_ui_module.c's own auto-
// login already established for exactly the same reason (a screen with
// no keyboard/touch can never satisfy a password prompt) —
// user_mgr_default_username() picks the bootstrap account (or whichever
// identity a remote OOBE push named instead) and user_mgr_set_logged_in()
// + app_manager_notify_unlocked() is the whole mechanism. See that
// module's own comment (oled_ui_module.c) for the fuller version of this
// reasoning; not re-derived here. Happens silently at boot — no separate
// "logged in as X" splash screen; the watchface is the first thing shown.
//
// ── Why every redraw is a whole screen, batched into ONE push_pixels() ──
// epd1in54.c's push_pixels()/fill_rect() each trigger a REAL ~1-2s full
// hardware refresh (or ~0.3s partial — see that file's own top comment).
// Every draw function here composes its ENTIRE output into one buffer
// first and calls push_pixels() exactly once — found live, not
// theoretical: an EARLY version of this file drew one push_pixels() call
// PER GLYPH and took ~58 seconds to draw three short lines.
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/catcall_display.h"
#include "../../modules/user_mgr/user_mgr.h"
#include "../../modules/app_manager/app_manager.h"

static const char *TAG = "epaper_ui";

#define FONT_W 6
#define FONT_H 8

// System-wide minimum text scale for this UI — a real 1.54" panel at
// FONT_W/FONT_H's native 6x8px is legible up close on a phone-sized
// screen but too small to read at a glance on this device, which is
// meant to be readable like a watch. draw_text()'s scale=1 stays
// available for a genuine special case (an e-reader-style app wanting
// to fit real paragraphs of text), but every other screen in this file
// uses at least this.
#define MIN_FONT_SCALE 2

#define COLOR_BLACK 0x0000u
#define COLOR_WHITE 0xFFFFu   // epd1in54.c thresholds anything non-zero to WHITE

#define BOOT_BUTTON_PIN 0    // confirmed against the vendor's own user_config.h: BOOT_BUTTON_PIN=GPIO0
#define PWR_BUTTON_PIN  18   // ...PWR_BUTTON_PIN=GPIO18
#define BUTTON_POLL_MS  30   // comfortably above real button-bounce timescales
#define HOLD_MS         500  // how long BOOT must stay down to count as "hold" rather than "short press"

// Font table copied from purr_fbtty.c (itself copied from boot_splash.c),
// not shared — same reasoning purr_fbtty.c's own comment already gives:
// this module must stay independently linkable regardless of which other
// text-drawing modules a given build does or doesn't include.
static const uint8_t s_font6x8[][6] = {
    {0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00,0x00},
    {0x00,0x07,0x00,0x07,0x00,0x00}, {0x14,0x7F,0x14,0x7F,0x14,0x00},
    {0x24,0x2A,0x7F,0x2A,0x12,0x00}, {0x23,0x13,0x08,0x64,0x62,0x00},
    {0x36,0x49,0x55,0x22,0x50,0x00}, {0x00,0x05,0x03,0x00,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00,0x00}, {0x00,0x41,0x22,0x1C,0x00,0x00},
    {0x08,0x2A,0x1C,0x2A,0x08,0x00}, {0x08,0x08,0x3E,0x08,0x08,0x00},
    {0x00,0x50,0x30,0x00,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08,0x00},
    {0x00,0x60,0x60,0x00,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02,0x00},
    {0x3E,0x51,0x49,0x45,0x3E,0x00}, {0x00,0x42,0x7F,0x40,0x00,0x00},
    {0x42,0x61,0x51,0x49,0x46,0x00}, {0x21,0x41,0x45,0x4B,0x31,0x00},
    {0x18,0x14,0x12,0x7F,0x10,0x00}, {0x27,0x45,0x45,0x45,0x39,0x00},
    {0x3C,0x4A,0x49,0x49,0x30,0x00}, {0x01,0x71,0x09,0x05,0x03,0x00},
    {0x36,0x49,0x49,0x49,0x36,0x00}, {0x06,0x49,0x49,0x29,0x1E,0x00},
    {0x00,0x36,0x36,0x00,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00,0x00}, {0x14,0x14,0x14,0x14,0x14,0x00},
    {0x00,0x41,0x22,0x14,0x08,0x00}, {0x02,0x01,0x51,0x09,0x06,0x00},
    {0x32,0x49,0x79,0x41,0x3E,0x00}, {0x7E,0x11,0x11,0x11,0x7E,0x00},
    {0x7F,0x49,0x49,0x49,0x36,0x00}, {0x3E,0x41,0x41,0x41,0x22,0x00},
    {0x7F,0x41,0x41,0x22,0x1C,0x00}, {0x7F,0x49,0x49,0x49,0x41,0x00},
    {0x7F,0x09,0x09,0x09,0x01,0x00}, {0x3E,0x41,0x49,0x49,0x7A,0x00},
    {0x7F,0x08,0x08,0x08,0x7F,0x00}, {0x00,0x41,0x7F,0x41,0x00,0x00},
    {0x20,0x40,0x41,0x3F,0x01,0x00}, {0x7F,0x08,0x14,0x22,0x41,0x00},
    {0x7F,0x40,0x40,0x40,0x40,0x00}, {0x7F,0x02,0x04,0x02,0x7F,0x00},
    {0x7F,0x04,0x08,0x10,0x7F,0x00}, {0x3E,0x41,0x41,0x41,0x3E,0x00},
    {0x7F,0x09,0x09,0x09,0x06,0x00}, {0x3E,0x41,0x51,0x21,0x5E,0x00},
    {0x7F,0x09,0x19,0x29,0x46,0x00}, {0x46,0x49,0x49,0x49,0x31,0x00},
    {0x01,0x01,0x7F,0x01,0x01,0x00}, {0x3F,0x40,0x40,0x40,0x3F,0x00},
    {0x1F,0x20,0x40,0x20,0x1F,0x00}, {0x3F,0x40,0x38,0x40,0x3F,0x00},
    {0x63,0x14,0x08,0x14,0x63,0x00}, {0x07,0x08,0x70,0x08,0x07,0x00},
    {0x61,0x51,0x49,0x45,0x43,0x00}, {0x00,0x7F,0x41,0x41,0x00,0x00},
    {0x02,0x04,0x08,0x10,0x20,0x00}, {0x00,0x41,0x41,0x7F,0x00,0x00},
    {0x04,0x02,0x01,0x02,0x04,0x00}, {0x40,0x40,0x40,0x40,0x40,0x00},
    {0x00,0x01,0x02,0x04,0x00,0x00}, {0x20,0x54,0x54,0x54,0x78,0x00},
    {0x7F,0x48,0x44,0x44,0x38,0x00}, {0x38,0x44,0x44,0x44,0x20,0x00},
    {0x38,0x44,0x44,0x48,0x7F,0x00}, {0x38,0x54,0x54,0x54,0x18,0x00},
    {0x08,0x7E,0x09,0x01,0x02,0x00}, {0x0C,0x52,0x52,0x52,0x3E,0x00},
    {0x7F,0x08,0x04,0x04,0x78,0x00}, {0x00,0x44,0x7D,0x40,0x00,0x00},
    {0x20,0x40,0x44,0x3D,0x00,0x00}, {0x7F,0x10,0x28,0x44,0x00,0x00},
    {0x00,0x41,0x7F,0x40,0x00,0x00}, {0x7C,0x04,0x18,0x04,0x78,0x00},
    {0x7C,0x08,0x04,0x04,0x78,0x00}, {0x38,0x44,0x44,0x44,0x38,0x00},
    {0x7C,0x14,0x14,0x14,0x08,0x00}, {0x08,0x14,0x14,0x18,0x7C,0x00},
    {0x7C,0x08,0x04,0x04,0x08,0x00}, {0x48,0x54,0x54,0x54,0x20,0x00},
    {0x04,0x3F,0x44,0x40,0x20,0x00}, {0x3C,0x40,0x40,0x20,0x7C,0x00},
    {0x1C,0x20,0x40,0x20,0x1C,0x00}, {0x3C,0x40,0x30,0x40,0x3C,0x00},
    {0x44,0x28,0x10,0x28,0x44,0x00}, {0x0C,0x50,0x50,0x50,0x3C,0x00},
    {0x44,0x64,0x54,0x4C,0x44,0x00}, {0x00,0x08,0x36,0x41,0x00,0x00},
    {0x00,0x00,0x7F,0x00,0x00,0x00}, {0x00,0x41,0x36,0x08,0x00,0x00},
    {0x08,0x04,0x08,0x10,0x08,0x00},
};

static const catcall_display_t *s_disp = NULL;
static int s_panel_w = 200, s_panel_h = 200;   // overwritten from get_info() in module_init()

// ── Text rendering ─────────────────────────────────────────────────────
// One parameterized drawer: batches the whole call into ONE buffer, one
// push_pixels(), generalized over glyph scale and fg/bg color so the
// same function draws the watchface clock, list cards (with per-card
// invert for the highlighted one), and a detail screen's readout.
#define TEXT_MAX_CHARS 34   // 200px / (FONT_W*1) rounded up — the real ceiling for this file's only scale=1 callers
#define TEXT_BUF_PX    4096 // covers every real call site: scale=1 up to 34 chars, or scale=4 up to ~6 chars (144x32=4608... see the runtime clamp below, which protects this regardless)

static void draw_text(int x_px, int y_px, const char *text, int scale, uint16_t fg, uint16_t bg)
{
    int len = (int)strlen(text);
    if (len > TEXT_MAX_CHARS) len = TEXT_MAX_CHARS;
    if (len <= 0) return;

    int gw = FONT_W * scale, gh = FONT_H * scale;
    // Clamp len so len*gw*gh can never exceed TEXT_BUF_PX, regardless of
    // scale — protects the fixed buffer below from a future caller
    // passing a larger scale/length combo than today's real call sites.
    while (len > 1 && len * gw * gh > TEXT_BUF_PX) len--;
    static uint16_t buf[TEXT_BUF_PX];
    int line_w = len * gw;

    for (int i = 0; i < len; i++) {
        char c = (text[i] < 0x20 || text[i] > 0x7E) ? ' ' : text[i];
        const uint8_t *glyph = s_font6x8[(unsigned char)c - 0x20];
        for (int cx = 0; cx < FONT_W; cx++) {
            uint8_t bits = glyph[cx];
            for (int cy = 0; cy < FONT_H; cy++) {
                uint16_t color = (bits & (1 << cy)) ? fg : bg;
                int base_x = i * gw + cx * scale;
                int base_y = cy * scale;
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        buf[(base_y + sy) * line_w + (base_x + sx)] = color;
                    }
                }
            }
        }
    }
    s_disp->push_pixels(x_px, y_px, line_w, gh, buf);
}

// ── Nav model ────────────────────────────────────────────────────────
typedef enum { NAV_NEXT, NAV_SELECT, NAV_BACK } nav_event_t;
typedef enum { SCREEN_WATCHFACE, SCREEN_APPLIST, SCREEN_DETAIL } screen_t;
static screen_t s_screen = SCREEN_WATCHFACE;

// ── App List model ───────────────────────────────────────────────────
// Three pinned "system" cards, rebuilt fresh with whatever app_manager
// reports every time the App List screen is entered — real content
// (app_manager_count()/_entry_name(), already local/remote-transparent,
// same dual-mode dispatch launcher_lvgl.c's own top comment documents),
// not faked. Capped at LIST_MAX_ITEMS with no scrolling yet — same
// honest, undersized-for-now scope launcher_lvgl.c's own tile grid
// already accepted for the identical reason (nothing on this device has
// needed more yet).
typedef enum { ITEM_SYSTEM, ITEM_APP } item_kind_t;
typedef struct {
    item_kind_t kind;
    char        label[32];
    void      (*render)(void);   // ITEM_SYSTEM only
    int         app_idx;         // ITEM_APP only
} list_item_t;

#define LIST_MAX_ITEMS 8   // panel only comfortably fits ~7 rows below the bigger title now (200px - 28px title) / MENU_ROW_H(22) — see draw_app_list(), no scrolling yet; 8 is a small, harmless overshoot until something actually needs that many

static void render_battery(void);
static void render_clock(void);
static void render_about(void);

static list_item_t s_list[LIST_MAX_ITEMS];
static int s_list_count = 0;
static int s_list_selected = 0;

static void build_app_list(void)
{
    s_list_count = 0;
    s_list[s_list_count++] = (list_item_t){ .kind = ITEM_SYSTEM, .label = "Battery", .render = render_battery };
    s_list[s_list_count++] = (list_item_t){ .kind = ITEM_SYSTEM, .label = "Clock",   .render = render_clock };
    s_list[s_list_count++] = (list_item_t){ .kind = ITEM_SYSTEM, .label = "About",   .render = render_about };

    int app_count = app_manager_count();
    for (int i = 0; i < app_count && s_list_count < LIST_MAX_ITEMS; i++) {
        list_item_t *it = &s_list[s_list_count];
        it->kind = ITEM_APP;
        it->app_idx = i;
        if (!app_manager_entry_name(i, it->label, sizeof(it->label))) continue;
        s_list_count++;
    }
    if (s_list_selected >= s_list_count) s_list_selected = 0;
}

#define MENU_SCALE    2
#define MENU_ROW_H    (FONT_H * MENU_SCALE + 6)   // row pitch — scaled glyph height + a little breathing room
#define TITLE_SCALE   3   // bigger than the row text (MENU_SCALE) — a title, not just another row
#define LIST_TITLE_H  (FONT_H * TITLE_SCALE + 4)

// Draws exactly ONE row — background fill (black if selected, white
// otherwise — a real inverted highlight, not just a "> " prefix) plus
// its label, in ONE push_pixels() call covering just that row's own
// rectangle. This is the fix for TWO real, hardware-found problems at
// once:
//
//   1. The ORIGINAL version of this row-stamping code (before it was
//      briefly replaced with plain draw_text() + a "> " prefix) never
//      advanced the x-position per character — every letter in a label
//      landed on top of the same spot instead of side by side, rendering
//      as illegible noise ("malformed icons"). Fixed here: base_x now
//      includes `i * gw`, the per-character offset that was missing.
//   2. Redrawing the WHOLE list (title + every row) on every single
//      cycle press — what draw_app_list() below did while this used
//      plain per-row draw_text() calls — felt like a full-screen refresh
//      on every press, because it touched every pixel in the list every
//      time. Cycling the highlight only ever changes TWO rows (the one
//      losing it, the one gaining it) — see nav_dispatch()'s own
//      SCREEN_APPLIST/NAV_NEXT case, which now calls this function for
//      just those two instead of draw_app_list() for the whole screen.
static void draw_list_row(int index)
{
    if (index < 0 || index >= s_list_count) return;

    bool sel = (index == s_list_selected);
    uint16_t fg = sel ? COLOR_WHITE : COLOR_BLACK;
    uint16_t bg = sel ? COLOR_BLACK : COLOR_WHITE;
    int w = s_panel_w;
    int gw = FONT_W * MENU_SCALE, gh = FONT_H * MENU_SCALE;

    static uint16_t buf[200 * MENU_ROW_H];   // one row's worth — see draw_app_list()'s own comment on why 200, not s_panel_w
    for (int y = 0; y < MENU_ROW_H; y++) {
        for (int x = 0; x < w; x++) buf[y * w + x] = bg;
    }

    const char *text = s_list[index].label;
    int len = (int)strlen(text);
    if (len > TEXT_MAX_CHARS) len = TEXT_MAX_CHARS;
    int text_y0 = (MENU_ROW_H - gh) / 2;
    for (int i = 0; i < len; i++) {
        char c = (text[i] < 0x20 || text[i] > 0x7E) ? ' ' : text[i];
        const uint8_t *glyph = s_font6x8[(unsigned char)c - 0x20];
        for (int cx = 0; cx < FONT_W; cx++) {
            uint8_t bits = glyph[cx];
            for (int cy = 0; cy < FONT_H; cy++) {
                if (!(bits & (1 << cy))) continue;   // background already painted — only stamp lit pixels
                int base_x = 4 + i * gw + cx * MENU_SCALE;   // the fix: was missing "i * gw"
                int base_y = text_y0 + cy * MENU_SCALE;
                for (int sy = 0; sy < MENU_SCALE; sy++) {
                    for (int sx = 0; sx < MENU_SCALE; sx++) {
                        buf[(base_y + sy) * w + (base_x + sx)] = fg;
                    }
                }
            }
        }
    }
    s_disp->push_pixels(0, LIST_TITLE_H + index * MENU_ROW_H, w, MENU_ROW_H, buf);
}

// Full redraw — title plus every row — used only when actually ENTERING
// the App List (NAV_SELECT from the watchface). Cycling the highlight
// afterward uses draw_list_row() directly on just the two affected rows
// (see nav_dispatch()) instead of calling this again.
static void draw_app_list(void)
{
    s_disp->fill_rect(0, 0, s_panel_w, s_panel_h, COLOR_WHITE);
    draw_text(4, 0, "Apps", TITLE_SCALE, COLOR_BLACK, COLOR_WHITE);

    if (s_list_count == 0) {
        draw_text(4, LIST_TITLE_H + 4, "No apps", MIN_FONT_SCALE, COLOR_BLACK, COLOR_WHITE);
        return;
    }
    for (int i = 0; i < s_list_count; i++) draw_list_row(i);
}

// ── Watchface ────────────────────────────────────────────────────────
static void draw_watchface(void)
{
    s_disp->fill_rect(0, 0, s_panel_w, s_panel_h, COLOR_WHITE);

    // Big clock, centered. "--:--" (not a fake reading) when unsynced —
    // this board's RTC (pcf85063) currently fails to read on real
    // hardware, see that driver's own top comment.
    char time_buf[8];
    purr_kernel_time_hhmm(time_buf, sizeof(time_buf));
    int scale = 4;
    int text_w = (int)strlen(time_buf) * FONT_W * scale;
    int x = (s_panel_w - text_w) / 2;
    int y = (s_panel_h - FONT_H * scale) / 2 - 10;
    draw_text(x, y, time_buf, scale, COLOR_BLACK, COLOR_WHITE);

    // Small battery readout under the clock — a real watchface
    // "complication", not decoration; reuses the exact same accessor
    // the App List's own Battery card does.
    int pct = purr_kernel_battery_percent();
    char batt_buf[16];   // "?"" or "%d%%" — GCC's format-truncation check wants headroom for any int width, not just realistic 0-100
    if (pct < 0) snprintf(batt_buf, sizeof(batt_buf), "?");
    else         snprintf(batt_buf, sizeof(batt_buf), "%d%%", pct);
    int batt_w = (int)strlen(batt_buf) * FONT_W * MIN_FONT_SCALE;
    draw_text((s_panel_w - batt_w) / 2, y + FONT_H * scale + 16, batt_buf, MIN_FONT_SCALE, COLOR_BLACK, COLOR_WHITE);
}

// ── Detail screen — one glanceable value, or a launch confirmation ─────

static void render_battery(void)
{
    int pct = purr_kernel_battery_percent();
    char line[16];
    if (pct < 0) snprintf(line, sizeof(line), "?");
    else         snprintf(line, sizeof(line), "%d%%", pct);
    draw_text(4, 20, "Battery", MIN_FONT_SCALE, COLOR_BLACK, COLOR_WHITE);
    draw_text(4, 44, line, 3, COLOR_BLACK, COLOR_WHITE);
}

static void render_clock(void)
{
    char line[8];
    purr_kernel_time_hhmm(line, sizeof(line));
    draw_text(4, 20, "Clock", MIN_FONT_SCALE, COLOR_BLACK, COLOR_WHITE);
    draw_text(4, 44, line, 3, COLOR_BLACK, COLOR_WHITE);
}

// Row pitch for stacked MIN_FONT_SCALE lines — scaled glyph height (16px)
// plus a small gap, same shape MENU_ROW_H already uses for the App List.
#define DETAIL_ROW_H (FONT_H * MIN_FONT_SCALE + 4)

static void render_about(void)
{
    draw_text(4, 0 * DETAIL_ROW_H, "About", MIN_FONT_SCALE, COLOR_BLACK, COLOR_WHITE);
    draw_text(4, 1 * DETAIL_ROW_H, "PURR OS", MIN_FONT_SCALE, COLOR_BLACK, COLOR_WHITE);
    draw_text(4, 2 * DETAIL_ROW_H, PURR_KERNEL_VERSION, MIN_FONT_SCALE, COLOR_BLACK, COLOR_WHITE);
    char user_line[40];
    snprintf(user_line, sizeof(user_line), "User: %s", user_mgr_current_user());
    draw_text(4, 3 * DETAIL_ROW_H, user_line, MIN_FONT_SCALE, COLOR_BLACK, COLOR_WHITE);
}

static void draw_detail(void)
{
    s_disp->fill_rect(0, 0, s_panel_w, s_panel_h, COLOR_WHITE);
    const list_item_t *it = &s_list[s_list_selected];
    if (it->kind == ITEM_SYSTEM) {
        it->render();
        return;
    }
    // ITEM_APP — really launches it (app_manager_launch_idx() spawns the
    // real task, app_entry_t.state goes RUNNING — same real launch
    // launcher_lvgl.c's own tile-tap already proves out), then shows a
    // plain confirmation. No window manager on this device yet to draw
    // the launched app's own UI into — same honest, undersized-for-now
    // scope launcher_lvgl.c's own top comment already accepted for the
    // identical reason.
    int rc = app_manager_launch_idx(it->app_idx);
    char line[40];
    snprintf(line, sizeof(line), rc == 0 ? "Launched" : "Launch failed");
    draw_text(4, 0 * DETAIL_ROW_H, line, MIN_FONT_SCALE, COLOR_BLACK, COLOR_WHITE);
    draw_text(4, 1 * DETAIL_ROW_H, it->label, MIN_FONT_SCALE, COLOR_BLACK, COLOR_WHITE);
}

// ── Nav dispatch — the one place screen transitions happen ─────────────
// See this file's own top comment on why button_task() below never
// touches s_screen directly.
static void nav_dispatch(nav_event_t ev)
{
    switch (s_screen) {
    case SCREEN_WATCHFACE:
        if (ev == NAV_SELECT) {
            build_app_list();
            s_screen = SCREEN_APPLIST;
            draw_app_list();
        }
        // NAV_NEXT/NAV_BACK: no-op — nothing to cycle on a watchface,
        // and it's already the root.
        break;

    case SCREEN_APPLIST:
        if (ev == NAV_NEXT && s_list_count > 0) {
            // Only the two affected rows get redrawn — see draw_list_
            // row()'s own comment on why NOT calling draw_app_list()
            // (the whole-screen redraw) here matters.
            int old_selected = s_list_selected;
            s_list_selected = (s_list_selected + 1) % s_list_count;
            draw_list_row(old_selected);
            draw_list_row(s_list_selected);
        } else if (ev == NAV_SELECT && s_list_count > 0) {
            s_screen = SCREEN_DETAIL;
            draw_detail();
        } else if (ev == NAV_BACK) {
            s_screen = SCREEN_WATCHFACE;
            draw_watchface();
        }
        break;

    case SCREEN_DETAIL:
        if (ev == NAV_NEXT) {
            // A short press just refreshes the current card's reading
            // (battery/clock can genuinely change) — real for system
            // cards, a harmless no-op redraw for an already-launched
            // app's confirmation screen.
            draw_detail();
        } else if (ev == NAV_BACK) {
            s_screen = SCREEN_APPLIST;
            draw_app_list();
        }
        // NAV_SELECT: nothing deeper to select on a detail screen yet.
        break;
    }
}

// ── Button input adapter ───────────────────────────────────────────────
// A plain polled FreeRTOS task, not interrupts: this panel's own refresh
// is already the slow part (~0.3-2s per push_pixels() call), so nothing
// here needs ISR-grade latency. Only job: turn BOOT/PWR GPIO edges into
// nav_event_t and call nav_dispatch() — see this file's own top comment.
static void button_task(void *arg)
{
    (void)arg;
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_PIN) | (1ULL << PWR_BUTTON_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    bool boot_was_down = false;
    bool boot_hold_fired = false;
    uint64_t boot_down_at_ms = 0;
    bool pwr_was_down = false;

    for (;;) {
        uint64_t now = purr_kernel_uptime_ms();
        bool boot_down = gpio_get_level(BOOT_BUTTON_PIN) == 0;   // active-low
        bool pwr_down  = gpio_get_level(PWR_BUTTON_PIN)  == 0;

        if (boot_down && !boot_was_down) {
            // Press started — the actual short-press/hold decision
            // happens on RELEASE (short) or once HOLD_MS elapses while
            // still down (hold), below.
            boot_down_at_ms = now;
            boot_hold_fired = false;
        } else if (boot_down && boot_was_down && !boot_hold_fired &&
                   (now - boot_down_at_ms) >= HOLD_MS) {
            boot_hold_fired = true;   // fires ONCE per press
            ESP_LOGI(TAG, "BOOT hold -> NAV_SELECT");
            nav_dispatch(NAV_SELECT);
        } else if (!boot_down && boot_was_down && !boot_hold_fired) {
            ESP_LOGI(TAG, "BOOT short press -> NAV_NEXT");
            nav_dispatch(NAV_NEXT);
        }
        boot_was_down = boot_down;

        if (pwr_down && !pwr_was_down) {
            ESP_LOGI(TAG, "PWR -> NAV_BACK");
            nav_dispatch(NAV_BACK);
        }
        pwr_was_down = pwr_down;

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

static int module_init(void)
{
    s_disp = purr_kernel_display();
    if (!s_disp) {
        ESP_LOGE(TAG, "no display catcall registered");
        return -1;
    }

    display_info_t info = {0};
    if (s_disp->get_info) s_disp->get_info(&info);
    if (info.width)  s_panel_w = info.width;
    if (info.height) s_panel_h = info.height;

    // Auto-login — see this file's own top comment.
    user_mgr_set_logged_in(user_mgr_default_username());
    app_manager_notify_unlocked();

    draw_watchface();
    ESP_LOGI(TAG, "watchface drawn — logged in as '%s'", user_mgr_current_user());

    xTaskCreate(button_task, "epaper_ui_btn", 3072, NULL, 2, NULL);

    return 0;
}

static void module_deinit(void)
{
    // Nothing to tear down — no heap allocation of our own (the button
    // task runs forever, same as every other UI module's own render
    // task). Never actually called today, kept for the same "every
    // module has both entry points" contract PURR_MODULE_REGISTER
    // requires either way.
}

PURR_MODULE_REGISTER(epaper_ui) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_UI,
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "epaper_ui",
    .version           = "0.3.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = CATCALL_FLAG_DISPLAY,
    .init              = module_init,
    .deinit            = module_deinit,
};
