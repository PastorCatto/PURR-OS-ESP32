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
#define Y_MARK    48        /* centre of the cat's eye */
#define Y_WORD    74
#define Y_VER     98
#define Y_ROWS    124       /* the entry rows, in the middle of the screen */
#define ROW_STEP  18
#define Y_ABOVE   188       /* stage text, or the countdown */
#define Y_BAR     204
#define Y_CHIPS   216       /* the status chips sit under the bar */
#else
#define Y_MARK    24
#define Y_WORD    44
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

static int slen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* Half-widths of the lens, from its middle row outward; the pupil is a black slit narrowing toward the ends. */
static const uint8_t k_lens[13] = {26, 26, 25, 24, 24, 22, 21, 19, 17, 15, 12, 8, 0};

static void mark(void)
{
    int cx = LCD_W / 2;
    for (int dy = -12; dy <= 12; dy++) {
        int a = dy < 0 ? -dy : dy;
        hw_fill(cx - k_lens[a], Y_MARK + dy, k_lens[a] * 2, 1, UI_ORANGE);
    }
    for (int dy = -11; dy <= 11; dy++) {
        int a = dy < 0 ? -dy : dy;
        int half = a < 7 ? 3 : (a < 10 ? 2 : 1);
        hw_fill(cx - half, Y_MARK + dy, half * 2, 1, UI_BLACK);
    }
}

/* Letter-spaced text, centred: hw_text has no spacing, so one call per character. */
static void spaced(int y, const char *s, uint16_t fg, int scale, int gap)
{
    int n = slen(s), cell = 8 * scale;
    int x = (LCD_W - (n * cell + (n - 1) * gap)) / 2;
    for (int i = 0; i < n; i++) {
        char c[2] = {s[i], 0};
        hw_text(x, y, c, fg, UI_BLACK, scale);
        x += cell + gap;
    }
}

static void centred(int y, const char *s, uint16_t fg)
{
    hw_text((LCD_W - hw_text_width(s, 1)) / 2, y, s, fg, UI_BLACK, 1);
}

void ui_frame(void)
{
    hw_fill(0, 0, LCD_W, LCD_H, UI_BLACK);
    mark();
    spaced(Y_WORD, "PURR OS", UI_WHITE, 2, 5);
#ifdef BOARD_CODENAME
    centred(Y_VER, BOARD_CODENAME "   " PKG_VERSION, UI_GREY);
#else
    centred(Y_VER, PKG_VERSION, UI_GREY);
#endif
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
