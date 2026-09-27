// miniwin_appmgr.c — see miniwin_appmgr.h for the full picture.
#include <string.h>
#include <stdio.h>
#include "MiniWin/miniwin.h"
#include "MiniWin/gl/gl.h"
#include "MiniWin/hal/hal_lcd.h"
#include "icon_lib/purr_icon_lib.h"
#include "../app_manager/app_manager.h"
#include "miniwin_appmgr.h"

#define ICON_SIZE   32
#define CELL_W      72
#define CELL_H      64
#define GRID_LEFT   8
#define GRID_TOP    8
#define GRID_ROWS   2

static mw_handle_t s_win = MW_INVALID_HANDLE;

// The ONE place that decides which library icon (icon_lib/purr_icon_lib.h)
// each app gets — change an association here, not the library itself, to
// give an app a different icon. Falls back to "program_manager" for
// anything not listed yet, same as purr_miniwin_icon_get()'s own caller-decides-
// the-fallback contract.
static const char *icon_name_for_app(const char *app_name)
{
    if (strcmp(app_name, "diagnostics") == 0) return "help";
    if (strcmp(app_name, "magidos")     == 0) return "pif_editor";
    if (strcmp(app_name, "doom")        == 0) return "solitaire";
    if (strcmp(app_name, "moy")         == 0) return "cardfile";
    if (strcmp(app_name, "home")        == 0) return "program_manager";
    return "program_manager";
}

static int grid_columns(int count)
{
    int cols = (count + GRID_ROWS - 1) / GRID_ROWS;   // ceil(count / 2)
    return cols > 0 ? cols : 1;
}

static void cell_origin(int index, int *out_x, int *out_y)
{
    int cols = grid_columns(app_manager_count());
    int col = index % cols;
    int row = index / cols;
    *out_x = GRID_LEFT + col * CELL_W;
    *out_y = GRID_TOP  + row * CELL_H;
}

static void appmgr_paint(mw_handle_t window_handle, const mw_gl_draw_info_t *draw_info)
{
    mw_util_rect_t client = mw_get_window_client_rect(window_handle);

    mw_gl_set_solid_fill_colour(MW_HAL_LCD_WHITE);
    mw_gl_clear_pattern();
    mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_fill(MW_GL_FILL);
    mw_gl_rectangle(draw_info, 0, 0, client.width, client.height);

    // Explicit graphics-context state for the labels drawn below —
    // deliberately NOT assumed left over from the background fill above
    // or from MiniWin's own window-chrome drawing (title bar text uses
    // MW_GL_TITLE_FONT, a different font/colour state entirely) running
    // before this callback. mw_gl_string()'s own doc comment says font,
    // foreground/background colour, and transparency are ALL "controlled
    // by gc" (the shared graphics-context state) — every one of those set
    // here, not relied on as a default.
    //
    // Real, hardware-found bug, actually root-caused this time: text
    // glyphs draw via mw_gl_fg_pixel(), which reads gc.fg_colour — a
    // FIELD COMPLETELY SEPARATE FROM gc.solid_fill_colour (what
    // mw_gl_set_solid_fill_colour() actually sets, used only by
    // mw_gl_rectangle() and friends). gl.c's own mw_gl_reset() initialises
    // gc.fg_colour = MW_HAL_LCD_WHITE by default — so every previous
    // attempt here (setting solid fill colour to black, picking a
    // specific font) changed nothing real: the actual text colour was
    // white-on-white against this window's own white background the
    // whole time. mw_gl_set_fg_colour() is the function that ACTUALLY
    // controls text/line colour; MW_GL_FONT_9's own availability was
    // never the problem (its case in get_font_data() has no #ifdef guard
    // at all, unlike 12/16/20/24 — it was always compiled in), so this
    // stays on the default font rather than a change that turned out to
    // be based on a wrong theory.
    mw_gl_set_fg_colour(MW_HAL_LCD_BLACK);
    mw_gl_set_bg_transparency(MW_GL_BG_TRANSPARENT);

    int count = app_manager_count();
    for (int i = 0; i < count; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (!app) continue;

        int x, y;
        cell_origin(i, &x, &y);

        uint16_t iw, ih;
        const uint8_t *rgb;
        if (purr_miniwin_icon_get(icon_name_for_app(app->name), &iw, &ih, &rgb)) {
            mw_gl_colour_bitmap(draw_info, x + (CELL_W - ICON_SIZE) / 2, y, iw, ih, rgb);
        }

        // Label centered under the icon (matching the icon's own
        // centering above) and truncated to fit inside this cell's own
        // width — a plain "x + 2" left-aligned draw, with no width check
        // at all, both misaligned every label relative to its icon AND
        // let longer names (e.g. "diagnostics") run past CELL_W straight
        // into the next column's cell, overlapping it. mw_gl_get_font_
        // width() gives the CURRENT font's real glyph width rather than
        // assuming one — mw_gl_string()'s own per-character advance is
        // exactly (font_width + 1), matching what it uses internally.
        char label[48];   // matches app_entry_t.name's own declared size
        snprintf(label, sizeof(label), "%s", app->name);
        int glyph_w = mw_gl_get_font_width() + 1;
        int max_chars = glyph_w > 0 ? CELL_W / glyph_w : (int)strlen(label);
        if (max_chars < 1) max_chars = 1;
        int len = (int)strlen(label);
        if (len > max_chars) { label[max_chars] = '\0'; len = max_chars; }
        int text_w = len * glyph_w;
        // -17px: the icons themselves aren't perfectly centered within
        // their own declared 32x32 canvas (composited from source PNGs
        // whose real drawn content isn't always canvas-centered), so a
        // label centered strictly on CELL_W's own bounding box reads as
        // visibly right of the icon above it. Direct feedback across
        // three rounds (-3px, then -10px, then "add another 7px") — a
        // manual correction, not a real per-icon content-bounds
        // calculation (that would need each icon's own actual
        // non-transparent pixel extent measured, not assumed).
        int label_x = x + (CELL_W - text_w) / 2 - 17;
        if (label_x < x) label_x = x;
        mw_gl_string(draw_info, label_x, y + ICON_SIZE + 4, label);
    }
}

static void appmgr_message(const mw_message_t *message)
{
    if (!message || message->message_id != MW_TOUCH_DOWN_MESSAGE) return;

    int16_t tx = (int16_t)(message->message_data >> 16);
    int16_t ty = (int16_t)(message->message_data & 0xFFFF);

    int count = app_manager_count();
    for (int i = 0; i < count; i++) {
        int x, y;
        cell_origin(i, &x, &y);
        if (tx >= x && tx < x + CELL_W && ty >= y && ty < y + CELL_H) {
            app_manager_launch_idx(i);
            return;
        }
    }
}

void miniwin_appmgr_open(void)
{
    if (s_win != MW_INVALID_HANDLE) {
        mw_set_window_visible(s_win, true);
        mw_bring_window_to_front(s_win);
        mw_paint_all();
        return;
    }

    int cols = grid_columns(app_manager_count());
    int disp_w = mw_hal_lcd_get_display_width();
    int disp_h = mw_hal_lcd_get_display_height();
    int16_t w = (int16_t)(cols * CELL_W + 2 * GRID_LEFT);
    int16_t h = (int16_t)(GRID_ROWS * CELL_H + 2 * GRID_TOP + 24);   // +24 ~ title bar
    if (w > disp_w) w = (int16_t)disp_w;
    if (h > disp_h) h = (int16_t)disp_h;

    mw_util_rect_t r = { 8, 8, w, h };
    s_win = mw_add_window(&r, "App Manager", appmgr_paint, appmgr_message,
                           NULL, 0,
                           MW_WINDOW_FLAG_HAS_BORDER | MW_WINDOW_FLAG_HAS_TITLE_BAR,
                           NULL);
    if (s_win == MW_INVALID_HANDLE) return;

    mw_set_window_visible(s_win, true);
    mw_bring_window_to_front(s_win);
    mw_paint_all();
}
