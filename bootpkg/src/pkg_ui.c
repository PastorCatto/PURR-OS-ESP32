/* pkg_ui.c - see pkg_ui.h. */
#include "pkg_ui.h"

#ifndef PKG_UI_SHIM                 /* the recovery loader supplies its own board macros and hw_*() (recovery_screen.c) */
#include "board.h"
#include "pkg_hw.h"
#endif

#ifndef PKG_VERSION
#define PKG_VERSION "0.0.0"
#endif

/* Two layouts: the 320x240 colour panel, and the 200x200 e-ink panel. Every y is fixed per layout. */
#if LCD_H >= 240
#define Y_LOGO    66        /* top of the 32 px high logo */
#define LOGO_SCALE 4
#define Y_VER     128
#define Y_ROWS    124       /* the entry rows, in the middle of the screen */
#define ROW_STEP  18
#define Y_ABOVE   188       /* stage text, or the countdown */
#define Y_BAR     204
#define Y_CHIPS   216       /* the status chips sit under the bar */
#else
#define Y_LOGO    24
#define LOGO_SCALE 2
#define Y_VER     64
#define Y_ROWS    86
#define ROW_STEP  16
#define Y_ABOVE   (LCD_H - 42)
#define Y_BAR     (LCD_H - 32)
#define Y_CHIPS   (LCD_H - 22)
#endif

#define BAR_X  (LCD_W / 8)
#define BAR_W  (LCD_W - 2 * BAR_X)
#define BAR_H  3

#ifdef PKG_UI_SHIM
#define UI_GLYPH(c) purr_font_glyph(c)
#else
#define UI_GLYPH(c) purr_font_glyph(c)
#include "purr_font.h"
#endif

/* Letter-spaced text, centred: hw_text has no spacing, so one call per character. (Kept for the e-ink layout.) */
static void centred(int y, const char *s, uint16_t fg)
{
    hw_text((LCD_W - hw_text_width(s, 1)) / 2, y, s, fg, UI_BLACK, 1);
}

void ui_centred(int y, const char *s, uint16_t fg) { centred(y, s, fg); }

void ui_left(int y, const char *s, uint16_t fg)
{
    hw_text(UI_MARGIN, y, s, fg, UI_BLACK, 1);
}

void ui_rule(int y)
{
    hw_fill(UI_MARGIN, y, LCD_W - 2 * UI_MARGIN, 1, UI_DIM);
}

void ui_clear_rect(int y, int h)
{
    hw_fill(0, y, LCD_W, h, UI_BLACK);
}

#ifdef PKG_BRAND_CODENAME                  /* a flavor names the board line itself */
#undef BOARD_CODENAME
#define BOARD_CODENAME PKG_BRAND_CODENAME
#endif

/* The wordmark (PKG_BRAND_LOGO, "PURR OS!" by default) in bold italic, drawn from the 8x8 font: every pixel row of every glyph row is a run of filled
 * cells, leaned to the right a little more the higher it is (italic), and each run is one pixel wider than the font's (bold). Characters before
 * PKG_BRAND_LOGO_ACCENT_FROM are in the text colour, the rest in the accent. A long wordmark shrinks to fit the line. */
static void logo(int y, int scale)
{
    static const char word[] = PKG_BRAND_LOGO;
    int n = (int)sizeof(word) - 1;
    while (scale > 1 && n * 8 * scale > LCD_W - 2 * UI_MARGIN) scale--;
    int cell = 8 * scale;
    int lean = 8 * scale - 1;
    int bold = scale >= 3 ? 2 : 1;
    int x0 = (LCD_W - n * cell) / 2;
    for (int ci = 0; ci < n; ci++) {
        const uint8_t *g = UI_GLYPH(word[ci]);
        uint16_t col = ci >= PKG_BRAND_LOGO_ACCENT_FROM ? UI_ORANGE : UI_WHITE;
        for (int r = 0; r < 8; r++) {
            for (int sy = 0; sy < scale; sy++) {
                int py = r * scale + sy;
                int shift = (lean - py) / 4;
                int c = 0;
                while (c < 8) {
                    if (!(g[r] & (0x80 >> c))) {
                        c++;
                        continue;
                    }
                    int e = c;
                    while (e < 8 && (g[r] & (0x80 >> e))) e++;
                    hw_fill(x0 + ci * cell + c * scale + shift, y + py, (e - c) * scale + bold, 1, col);
                    c = e;
                }
            }
        }
    }
}

void ui_frame(void)
{
    hw_fill(0, 0, LCD_W, LCD_H, UI_BLACK);
    logo(Y_LOGO, LOGO_SCALE);
#ifdef BOARD_CODENAME
    centred(Y_VER, BOARD_CODENAME "   " PKG_VERSION, UI_GREY);
#else
    centred(Y_VER, PKG_VERSION, UI_GREY);
#endif
}

void ui_frame_small(void)
{
    hw_fill(0, 0, LCD_W, LCD_H, UI_BLACK);
    logo(11, 2);
}

void ui_menu_row(int y, const char *name, const char *tag, int tag_hot, int selected, int usable)
{
    int x0 = UI_MARGIN, w = LCD_W - 2 * UI_MARGIN;
    hw_fill(x0, y, w, 24, UI_BLACK);
    if (selected) {
        hw_fill(x0, y, w, 1, UI_ORANGE);
        hw_fill(x0, y + 23, w, 1, UI_ORANGE);
        hw_fill(x0, y, 1, 24, UI_ORANGE);
        hw_fill(x0 + w - 1, y, 1, 24, UI_ORANGE);
    }
    hw_text(x0 + 7, y + 8, name, usable ? UI_WHITE : UI_GREY, UI_BLACK, 1);
    if (tag && tag[0]) {
        hw_text(x0 + w - 7 - hw_text_width(tag, 1), y + 8, tag, tag_hot ? UI_ORANGE : UI_GREY, UI_BLACK, 1);
    }
}

static int chip_w(const ui_chip_t *c)
{
    return hw_text_width(c->label, 1) + 14 + (c->kind == UI_CHIP_OK ? 8 : 0);
}

static void chip(int x, int y, const ui_chip_t *c)
{
    int w = chip_w(c);
    if (c->kind == UI_CHIP_HOT) {
        hw_fill(x, y, w, 13, UI_ORANGE);
        hw_text(x + 7, y + 3, c->label, UI_BLACK, UI_ORANGE, 1);
        return;
    }
    uint16_t edge = c->kind == UI_CHIP_WARN ? UI_ORANGE : UI_GREY;
    hw_fill(x, y, w, 1, edge);
    hw_fill(x, y + 12, w, 1, edge);
    hw_fill(x, y, 1, 13, edge);
    hw_fill(x + w - 1, y, 1, 13, edge);
    int tx = x + 7;
    if (c->kind == UI_CHIP_OK) {
        hw_fill(x + 5, y + 5, 3, 3, UI_GREEN);
        tx += 8;
    }
    hw_text(tx, y + 3, c->label, c->kind == UI_CHIP_WARN ? UI_ORANGE : UI_WHITE, UI_BLACK, 1);
}

void ui_chips(const ui_chip_t *chips, int n)
{
    int total;
    for (;;) {                                  /* drop the last chip until the row fits */
        total = 6 * (n > 0 ? n - 1 : 0);
        for (int i = 0; i < n; i++) total += chip_w(&chips[i]);
        if (total <= LCD_W - 12 || n <= 1) break;
        n--;
    }
    int x = (LCD_W - total) / 2;
    hw_fill(0, Y_CHIPS, LCD_W, 13, UI_BLACK);
    for (int i = 0; i < n; i++) {
        chip(x, Y_CHIPS, &chips[i]);
        x += chip_w(&chips[i]) + 6;
    }
}

void ui_clear_band(void)
{
    hw_fill(0, Y_ROWS - 6, LCD_W, Y_ABOVE - (Y_ROWS - 6), UI_BLACK);      /* the rows area only; bar and chips stay */
}

void ui_row(int index, const char *name, const char *tag, int selected, int usable)
{
    int y = Y_ROWS + index * ROW_STEP;
    int x0 = BAR_X - 6, w = BAR_W + 12;
    hw_fill(x0, y - 3, w, ROW_STEP - 1, UI_BLACK);       /* clear the row, including an old border */
    if (selected) {                                      /* a border, not a fill: it reads on the e-ink panel */
        hw_fill(x0, y - 3, w, 1, UI_ORANGE);
        hw_fill(x0, y + ROW_STEP - 5, w, 1, UI_ORANGE);
        hw_fill(x0, y - 3, 1, ROW_STEP - 1, UI_ORANGE);
        hw_fill(x0 + w - 1, y - 3, 1, ROW_STEP - 1, UI_ORANGE);
    }
    hw_text(BAR_X + 4, y, name, usable ? UI_WHITE : UI_GREY, UI_BLACK, 1);
    if (tag && tag[0]) {
        int isdef = tag[0] == 'd';
        hw_text(BAR_X + BAR_W - hw_text_width(tag, 1) - 4, y, tag, isdef ? UI_ORANGE : UI_GREY, UI_BLACK, 1);
    }
}

void ui_bar(int per_mille)
{
    if (per_mille < 0) per_mille = 0;
    if (per_mille > 1000) per_mille = 1000;
    int fill = BAR_W * per_mille / 1000;
    hw_fill(BAR_X, Y_BAR, BAR_W, BAR_H, UI_DIM);
    if (fill > 0) hw_fill(BAR_X, Y_BAR, fill, BAR_H, UI_ORANGE);
}

void ui_above(const char *left, const char *right)
{
    hw_fill(BAR_X, Y_ABOVE, BAR_W, 8, UI_BLACK);
    if (left) hw_text(BAR_X, Y_ABOVE, left, UI_WHITE, UI_BLACK, 1);
    if (right && (!left || hw_text_width(left, 1) + hw_text_width(right, 1) + 8 <= BAR_W)) {   /* only if it fits */
        hw_text(BAR_X + BAR_W - hw_text_width(right, 1), Y_ABOVE, right, UI_GREY, UI_BLACK, 1);
    }
}

void ui_splash(const ui_chip_t *chips, int n, const char *stage, const char *right, int per_mille)
{
    ui_frame();
    ui_above(stage, right);
    ui_bar(per_mille);
    ui_chips(chips, n);
    hw_flush();
}
