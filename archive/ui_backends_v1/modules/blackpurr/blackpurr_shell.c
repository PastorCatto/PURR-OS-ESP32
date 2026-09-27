// blackpurr_shell.c — BlackPURR text-mode shell
//
// Draws directly via catcall_display fill_rect/push_pixels using a 6x8 bitmap
// font. No LVGL. Touch coordinates come straight from GT911 (0-319, 0-239).
// Trackball moves a virtual cursor; BBQ20 keys navigate the grid.

#include "blackpurr.h"
#include "../../kernel/core/purr_kernel.h"
#include "../app_manager/app_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define CHAR_W  6
#define CHAR_H  8

static const char *TAG = "bp_shell";

// ── 6x8 bitmap font (columns, bit0=top) ──────────────────────────────────────

const uint8_t BP_FONT6x8[][6] = {
    {0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x00,0x00,0x5F,0x00,0x00,0x00}, // '!'
    {0x00,0x07,0x00,0x07,0x00,0x00}, // '"'
    {0x14,0x7F,0x14,0x7F,0x14,0x00}, // '#'
    {0x24,0x2A,0x7F,0x2A,0x12,0x00}, // '$'
    {0x23,0x13,0x08,0x64,0x62,0x00}, // '%'
    {0x36,0x49,0x55,0x22,0x50,0x00}, // '&'
    {0x00,0x05,0x03,0x00,0x00,0x00}, // '\''
    {0x00,0x1C,0x22,0x41,0x00,0x00}, // '('
    {0x00,0x41,0x22,0x1C,0x00,0x00}, // ')'
    {0x08,0x2A,0x1C,0x2A,0x08,0x00}, // '*'
    {0x08,0x08,0x3E,0x08,0x08,0x00}, // '+'
    {0x00,0x50,0x30,0x00,0x00,0x00}, // ','
    {0x08,0x08,0x08,0x08,0x08,0x00}, // '-'
    {0x00,0x60,0x60,0x00,0x00,0x00}, // '.'
    {0x20,0x10,0x08,0x04,0x02,0x00}, // '/'
    {0x3E,0x51,0x49,0x45,0x3E,0x00}, // '0'
    {0x00,0x42,0x7F,0x40,0x00,0x00}, // '1'
    {0x42,0x61,0x51,0x49,0x46,0x00}, // '2'
    {0x21,0x41,0x45,0x4B,0x31,0x00}, // '3'
    {0x18,0x14,0x12,0x7F,0x10,0x00}, // '4'
    {0x27,0x45,0x45,0x45,0x39,0x00}, // '5'
    {0x3C,0x4A,0x49,0x49,0x30,0x00}, // '6'
    {0x01,0x71,0x09,0x05,0x03,0x00}, // '7'
    {0x36,0x49,0x49,0x49,0x36,0x00}, // '8'
    {0x06,0x49,0x49,0x29,0x1E,0x00}, // '9'
    {0x00,0x36,0x36,0x00,0x00,0x00}, // ':'
    {0x00,0x56,0x36,0x00,0x00,0x00}, // ';'
    {0x08,0x14,0x22,0x41,0x00,0x00}, // '<'
    {0x14,0x14,0x14,0x14,0x14,0x00}, // '='
    {0x00,0x41,0x22,0x14,0x08,0x00}, // '>'
    {0x02,0x01,0x51,0x09,0x06,0x00}, // '?'
    {0x32,0x49,0x79,0x41,0x3E,0x00}, // '@'
    {0x7E,0x11,0x11,0x11,0x7E,0x00}, // 'A'
    {0x7F,0x49,0x49,0x49,0x36,0x00}, // 'B'
    {0x3E,0x41,0x41,0x41,0x22,0x00}, // 'C'
    {0x7F,0x41,0x41,0x22,0x1C,0x00}, // 'D'
    {0x7F,0x49,0x49,0x49,0x41,0x00}, // 'E'
    {0x7F,0x09,0x09,0x09,0x01,0x00}, // 'F'
    {0x3E,0x41,0x49,0x49,0x7A,0x00}, // 'G'
    {0x7F,0x08,0x08,0x08,0x7F,0x00}, // 'H'
    {0x00,0x41,0x7F,0x41,0x00,0x00}, // 'I'
    {0x20,0x40,0x41,0x3F,0x01,0x00}, // 'J'
    {0x7F,0x08,0x14,0x22,0x41,0x00}, // 'K'
    {0x7F,0x40,0x40,0x40,0x40,0x00}, // 'L'
    {0x7F,0x02,0x0C,0x02,0x7F,0x00}, // 'M'
    {0x7F,0x04,0x08,0x10,0x7F,0x00}, // 'N'
    {0x3E,0x41,0x41,0x41,0x3E,0x00}, // 'O'
    {0x7F,0x09,0x09,0x09,0x06,0x00}, // 'P'
    {0x3E,0x41,0x51,0x21,0x5E,0x00}, // 'Q'
    {0x7F,0x09,0x19,0x29,0x46,0x00}, // 'R'
    {0x46,0x49,0x49,0x49,0x31,0x00}, // 'S'
    {0x01,0x01,0x7F,0x01,0x01,0x00}, // 'T'
    {0x3F,0x40,0x40,0x40,0x3F,0x00}, // 'U'
    {0x1F,0x20,0x40,0x20,0x1F,0x00}, // 'V'
    {0x3F,0x40,0x38,0x40,0x3F,0x00}, // 'W'
    {0x63,0x14,0x08,0x14,0x63,0x00}, // 'X'
    {0x07,0x08,0x70,0x08,0x07,0x00}, // 'Y'
    {0x61,0x51,0x49,0x45,0x43,0x00}, // 'Z'
    {0x00,0x7F,0x41,0x41,0x00,0x00}, // '['
    {0x02,0x04,0x08,0x10,0x20,0x00}, // '\\'
    {0x00,0x41,0x41,0x7F,0x00,0x00}, // ']'
    {0x04,0x02,0x01,0x02,0x04,0x00}, // '^'
    {0x40,0x40,0x40,0x40,0x40,0x00}, // '_'
    {0x00,0x01,0x02,0x04,0x00,0x00}, // '`'
    {0x20,0x54,0x54,0x54,0x78,0x00}, // 'a'
    {0x7F,0x48,0x44,0x44,0x38,0x00}, // 'b'
    {0x38,0x44,0x44,0x44,0x20,0x00}, // 'c'
    {0x38,0x44,0x44,0x48,0x7F,0x00}, // 'd'
    {0x38,0x54,0x54,0x54,0x18,0x00}, // 'e'
    {0x08,0x7E,0x09,0x01,0x02,0x00}, // 'f'
    {0x0C,0x52,0x52,0x52,0x3E,0x00}, // 'g'
    {0x7F,0x08,0x04,0x04,0x78,0x00}, // 'h'
    {0x00,0x44,0x7D,0x40,0x00,0x00}, // 'i'
    {0x20,0x40,0x44,0x3D,0x00,0x00}, // 'j'
    {0x7F,0x10,0x28,0x44,0x00,0x00}, // 'k'
    {0x00,0x41,0x7F,0x40,0x00,0x00}, // 'l'
    {0x7C,0x04,0x18,0x04,0x78,0x00}, // 'm'
    {0x7C,0x08,0x04,0x04,0x78,0x00}, // 'n'
    {0x38,0x44,0x44,0x44,0x38,0x00}, // 'o'
    {0x7C,0x14,0x14,0x14,0x08,0x00}, // 'p'
    {0x08,0x14,0x14,0x18,0x7C,0x00}, // 'q'
    {0x7C,0x08,0x04,0x04,0x08,0x00}, // 'r'
    {0x48,0x54,0x54,0x54,0x20,0x00}, // 's'
    {0x04,0x3F,0x44,0x40,0x20,0x00}, // 't'
    {0x3C,0x40,0x40,0x40,0x7C,0x00}, // 'u'
    {0x1C,0x20,0x40,0x20,0x1C,0x00}, // 'v'
    {0x3C,0x40,0x30,0x40,0x3C,0x00}, // 'w'
    {0x44,0x28,0x10,0x28,0x44,0x00}, // 'x'
    {0x0C,0x50,0x50,0x50,0x3C,0x00}, // 'y'
    {0x44,0x64,0x54,0x4C,0x44,0x00}, // 'z'
    {0x00,0x08,0x36,0x41,0x00,0x00}, // '{'
    {0x00,0x00,0x7F,0x00,0x00,0x00}, // '|'
    {0x00,0x41,0x36,0x08,0x00,0x00}, // '}'
    {0x08,0x08,0x2A,0x1C,0x08,0x00}, // '~'
    {0x00,0x00,0x00,0x00,0x00,0x00}, // DEL
};

// ── Draw primitives ───────────────────────────────────────────────────────────

static const catcall_display_t *s_d = NULL;

static void bp_fill(int x, int y, int w, int h, uint16_t col)
{
    if (s_d && s_d->fill_rect) s_d->fill_rect(x, y, w, h, col);
}

static void bp_char(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    if (!s_d || !s_d->push_pixels) return;
    if (c < 32 || c > 127) c = '?';
    const uint8_t *col = BP_FONT6x8[(uint8_t)c - 32];
    uint16_t buf[CHAR_W * CHAR_H];
    for (int cx = 0; cx < CHAR_W; cx++) {
        uint8_t bits = col[cx];
        for (int row = 0; row < CHAR_H; row++)
            buf[row * CHAR_W + cx] = (bits & (1 << row)) ? fg : bg;
    }
    s_d->push_pixels(x, y, CHAR_W, CHAR_H, buf);
}

static void bp_str(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    while (*s) { bp_char(x, y, *s++, fg, bg); x += CHAR_W; }
}

static void bp_strf(int x, int y, uint16_t fg, uint16_t bg, const char *fmt, ...)
{
    char buf[64];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    bp_str(x, y, buf, fg, bg);
}

// Centered string inside a rect
static void bp_str_center(int rx, int ry, int rw, int rh,
                           const char *s, uint16_t fg, uint16_t bg)
{
    int len = (int)strlen(s);
    int tw  = len * CHAR_W;
    int tx  = rx + (rw - tw) / 2;
    int ty  = ry + (rh - CHAR_H) / 2;
    if (tx < rx) tx = rx;
    // fill background first so text sits cleanly
    bp_fill(rx, ry, rw, rh, bg);
    // clip str if it doesn't fit
    for (int i = 0; i < len && tx + CHAR_W <= rx + rw; i++, tx += CHAR_W)
        bp_char(tx, ty, s[i], fg, bg);
}

// ── Shell state ───────────────────────────────────────────────────────────────

static int s_sel  = 0;   // selected app index
static int s_page = 0;   // page offset (multiples of BP_COLS*BP_ROWS)

static int s_app_count = 0;
#define BP_MAX_APPS 64
static app_entry_t s_apps[BP_MAX_APPS];

// ── Grid drawing ──────────────────────────────────────────────────────────────

static void cell_xy(int idx, int *cx, int *cy)
{
    int col = idx % BP_COLS;
    int row = (idx / BP_COLS) % BP_ROWS;
    *cx = col * BP_CELL_W;
    *cy = BP_GRID_Y + row * BP_CELL_H;
}

static void draw_cell(int idx, bool selected)
{
    int cx, cy;
    cell_xy(idx, &cx, &cy);

    uint16_t bg  = selected ? BP_COL_SEL    : BP_COL_BG;
    uint16_t acc = selected ? BP_COL_ACCENT  : BP_COL_DIM;
    uint16_t fg  = BP_COL_TEXT;

    bp_fill(cx, cy, BP_CELL_W, BP_CELL_H, bg);

    // Accent bar at top of cell
    bp_fill(cx + 2, cy + 2, BP_CELL_W - 4, 2, acc);

    // App name (2 lines max, wrap at ~12 chars per line)
    int page_base = s_page * BP_COLS * BP_ROWS;
    int app_idx   = page_base + idx;
    if (app_idx < s_app_count) {
        const char *name = s_apps[app_idx].name;
        char line1[14] = {0}, line2[14] = {0};
        int len = (int)strlen(name);
        if (len <= 13) {
            strncpy(line1, name, 13);
        } else {
            strncpy(line1, name, 13);
            strncpy(line2, name + 13, 13);
        }
        bp_str_center(cx + 1, cy + 8,  BP_CELL_W - 2, CHAR_H, line1, fg, bg);
        if (line2[0])
            bp_str_center(cx + 1, cy + 18, BP_CELL_W - 2, CHAR_H, line2, fg, bg);

        // Tier badge bottom-right
        const char *tier = "";
        if (s_apps[app_idx].tier == APP_TIER_MEOW)      tier = "lua";
        else if (s_apps[app_idx].tier == APP_TIER_PAWS)  tier = "pws";
        else if (s_apps[app_idx].tier == APP_TIER_CLAW)  tier = "clw";
        bp_str(cx + BP_CELL_W - 19, cy + BP_CELL_H - 10, tier, acc, bg);
    }

    // Cell border
    bp_fill(cx,                  cy,                  BP_CELL_W, 1,         BP_COL_DIM);
    bp_fill(cx,                  cy,                  1,         BP_CELL_H, BP_COL_DIM);
    bp_fill(cx + BP_CELL_W - 1,  cy,                  1,         BP_CELL_H, BP_COL_DIM);
    bp_fill(cx,                  cy + BP_CELL_H - 1,  BP_CELL_W, 1,         BP_COL_DIM);
}

static void draw_grid(void)
{
    int per_page = BP_COLS * BP_ROWS;
    int base = s_page * per_page;
    for (int i = 0; i < per_page; i++) {
        int app_idx = base + i;
        bool has_app = app_idx < s_app_count;
        if (has_app) {
            draw_cell(i, i == s_sel);
        } else {
            int cx, cy;
            cell_xy(i, &cx, &cy);
            bp_fill(cx, cy, BP_CELL_W, BP_CELL_H, BP_COL_BG);
            bp_fill(cx, cy, BP_CELL_W, 1, BP_COL_DIM);
            bp_fill(cx, cy, 1, BP_CELL_H, BP_COL_DIM);
        }
    }
}

// ── Status bar ────────────────────────────────────────────────────────────────

static void draw_status(uint32_t uptime_s)
{
    bp_fill(0, 0, BP_LCD_W, BP_STATUS_H, BP_COL_SURFACE);

    // Brand
    bp_str(4, 4, "PURR OS", BP_COL_ACCENT, BP_COL_SURFACE);

    // Uptime centre
    char ubuf[20];
    uint32_t h = uptime_s / 3600, m = (uptime_s % 3600) / 60, s = uptime_s % 60;
    snprintf(ubuf, sizeof(ubuf), "%02lu:%02lu:%02lu", (unsigned long)h,
             (unsigned long)m, (unsigned long)s);
    bp_str(BP_LCD_W / 2 - (int)(strlen(ubuf) * CHAR_W) / 2, 4,
           ubuf, BP_COL_TEXT, BP_COL_SURFACE);

    // Page indicator right
    int pages = (s_app_count + BP_COLS * BP_ROWS - 1) / (BP_COLS * BP_ROWS);
    if (pages < 1) pages = 1;
    char pbuf[24];
    snprintf(pbuf, sizeof(pbuf), "%d/%d", s_page + 1, pages);
    bp_str(BP_LCD_W - (int)(strlen(pbuf) + 1) * CHAR_W, 4,
           pbuf, BP_COL_DIM, BP_COL_SURFACE);

    // Divider
    bp_fill(0, BP_STATUS_H, BP_LCD_W, 1, BP_COL_DIM);
}

// ── Bottom silkscreen bar — Palm OS-style Home / Menu / Find ─────────────────
// Three equal-width zones, same row the physical silkscreen buttons sat
// under on a real Palm device. Touch-only for now (Home resets to page 0;
// Menu/Find are stubbed — logged, no UI yet — until there's an actual
// context menu / search target to wire them to).

static void draw_bottom(void)
{
    int y = BP_LCD_H - BP_BOTTOM_H;
    int zone_w = BP_LCD_W / 3;
    bp_fill(0, y, BP_LCD_W, BP_BOTTOM_H, BP_COL_SURFACE);
    bp_fill(0, y, BP_LCD_W, 1, BP_COL_DIM);

    bp_str_center(0,           y + 1, zone_w, BP_BOTTOM_H - 1, "Home", BP_COL_TEXT, BP_COL_SURFACE);
    bp_str_center(zone_w,      y + 1, zone_w, BP_BOTTOM_H - 1, "Menu", BP_COL_TEXT, BP_COL_SURFACE);
    bp_str_center(zone_w * 2,  y + 1, BP_LCD_W - zone_w * 2, BP_BOTTOM_H - 1, "Find", BP_COL_TEXT, BP_COL_SURFACE);

    bp_fill(zone_w,     y, 1, BP_BOTTOM_H, BP_COL_DIM);
    bp_fill(zone_w * 2, y, 1, BP_BOTTOM_H, BP_COL_DIM);
}

static void go_home(void)
{
    if (s_page == 0 && s_sel == 0) return;
    s_page = 0;
    s_sel  = 0;
    draw_grid();
    draw_bottom();
}

// ── Move selection ────────────────────────────────────────────────────────────

static void move_sel(int dr, int dc)
{
    int per_page = BP_COLS * BP_ROWS;
    int old_sel  = s_sel;

    int row = s_sel / BP_COLS + dr;
    int col = s_sel % BP_COLS + dc;

    // Horizontal wrap
    if (col < 0) { col = BP_COLS - 1; row--; }
    if (col >= BP_COLS) { col = 0; row++; }

    // Page boundary
    if (row < 0) {
        if (s_page > 0) { s_page--; row = BP_ROWS - 1; draw_grid(); }
        else row = 0;
    } else if (row >= BP_ROWS) {
        int total_pages = (s_app_count + per_page - 1) / per_page;
        if (s_page < total_pages - 1) { s_page++; row = 0; draw_grid(); }
        else row = BP_ROWS - 1;
    }

    s_sel = row * BP_COLS + col;

    // Clamp to valid app on current page
    int max_on_page = s_app_count - s_page * per_page;
    if (max_on_page > per_page) max_on_page = per_page;
    if (s_sel >= max_on_page && max_on_page > 0) s_sel = max_on_page - 1;

    if (s_sel != old_sel) {
        draw_cell(old_sel, false);
        draw_cell(s_sel,   true);
        draw_bottom();
    }
}

// ── Launch selected app ───────────────────────────────────────────────────────

static void launch_selected(void)
{
    int idx = s_page * BP_COLS * BP_ROWS + s_sel;
    if (idx >= s_app_count) return;
    ESP_LOGI(TAG, "launching: %s", s_apps[idx].name);
    app_manager_launch_idx(idx);
}

// ── Public API ────────────────────────────────────────────────────────────────

void blackpurr_shell_init(void)
{
    s_d = purr_kernel_display();
    if (!s_d) { ESP_LOGE(TAG, "no display catcall"); return; }

    // Scan apps
    s_app_count = 0;
    int n = app_manager_count();
    if (n > BP_MAX_APPS) n = BP_MAX_APPS;
    for (int i = 0; i < n; i++) {
        const app_entry_t *e = app_manager_get(i);
        if (e) s_apps[s_app_count++] = *e;
    }

    // Clear screen
    bp_fill(0, 0, BP_LCD_W, BP_LCD_H, BP_COL_BG);
    draw_status(0);
    draw_grid();
    draw_bottom();

    ESP_LOGI(TAG, "shell ready — %d apps", s_app_count);
}

void blackpurr_shell_tick(uint32_t uptime_s)
{
    draw_status(uptime_s);
}

void blackpurr_shell_on_key(uint8_t keycode)
{
    switch (keycode) {
        case 0x0D: // Enter / OK
        case '\n':
            launch_selected();
            break;
        case 0x1B: // Escape — keyboard equivalent of tapping Home
            go_home();
            break;
        default:
            // Type-to-jump: find first app whose name starts with this char
            if (keycode >= 'a' && keycode <= 'z') {
                char up = (char)(keycode - 32);
                int n = s_app_count;
                for (int i = 0; i < n; i++) {
                    char fc = s_apps[i].name[0];
                    if (fc == keycode || fc == up) {
                        int per = BP_COLS * BP_ROWS;
                        int old_sel = s_sel;
                        int old_page = s_page;
                        s_page = i / per;
                        s_sel  = i % per;
                        if (s_page != old_page) draw_grid();
                        else { draw_cell(old_sel, false); draw_cell(s_sel, true); }
                        draw_bottom();
                        break;
                    }
                }
            }
            break;
    }
}

void blackpurr_shell_on_pointer(int16_t dx, int16_t dy)
{
    // Trackball: accumulate deltas, move selection when threshold reached
    static int16_t ax = 0, ay = 0;
    ax += dx; ay += dy;
    const int16_t THRESH = 3;
    if (ay < -THRESH) { ay = 0; move_sel(-1, 0); }
    else if (ay >  THRESH) { ay = 0; move_sel( 1, 0); }
    if (ax < -THRESH) { ax = 0; move_sel(0, -1); }
    else if (ax >  THRESH) { ax = 0; move_sel(0,  1); }
}

void blackpurr_shell_on_click(void)
{
    launch_selected();
}

void blackpurr_shell_on_touch(uint16_t x, uint16_t y)
{
    // GT911 outputs screen-native coords (0-319, 0-239) — direct hit test
    if (y >= BP_LCD_H - BP_BOTTOM_H) {
        int zone_w = BP_LCD_W / 3;
        int zone = (int)x / zone_w;
        if (zone == 0) go_home();
        else if (zone == 1) ESP_LOGI(TAG, "Menu tapped (stub — no context menu yet)");
        else                ESP_LOGI(TAG, "Find tapped (stub — no search yet)");
        return;
    }
    if (y < BP_GRID_Y) return;

    int col = (int)x / BP_CELL_W;
    int row = ((int)y - BP_GRID_Y) / BP_CELL_H;
    if (col < 0 || col >= BP_COLS || row < 0 || row >= BP_ROWS) return;

    int new_sel = row * BP_COLS + col;
    int per     = BP_COLS * BP_ROWS;
    int max_on  = s_app_count - s_page * per;
    if (max_on > per) max_on = per;
    if (new_sel >= max_on) return;

    int old_sel = s_sel;
    s_sel = new_sel;
    draw_cell(old_sel, false);
    draw_cell(s_sel,   true);
    draw_bottom();
    launch_selected();
}
