/*
 * pkg_ui.h - the boot package's look: one frame, and a bottom band that changes (plan1.md, boot redesign).
 *
 * Every boot screen shares the same frame: the cat's-eye mark, the wordmark and the version line at the top, the
 * progress bar low on the screen with its line of text above it, and the small status chips under the bar. What
 * is between the version line and the bar changes (nothing while verifying, the entry rows in the menu). The bar
 * is always in the same place; it fills while something is verified or installed, and drains while the menu counts
 * down.
 *
 * Only hw_fill() and hw_text() are used, so the same code draws on the ST7789 and on the two-tone e-ink panel:
 * nothing depends on colour alone (a chip is an outline, a selection is a border).
 */
#ifndef PKG_UI_H
#define PKG_UI_H

#include <stdint.h>

#include "pkg_brand.h"

#define UI_BLACK  0x0000
#define UI_WHITE  PKG_BRAND_COLOUR_TEXT
#define UI_GREY   PKG_BRAND_COLOUR_DIM
#define UI_ORANGE PKG_BRAND_COLOUR_ACCENT   /* the accent, whatever the flavor makes it */
#define UI_DIM    0x39E7       /* the empty part of the bar */
#define UI_GREEN  PKG_BRAND_COLOUR_OK

/* Chip kinds. */
#define UI_CHIP_OK    0        /* grey outline, green dot: fine */
#define UI_CHIP_PLAIN 1        /* grey outline, no dot: just information */
#define UI_CHIP_WARN  2        /* orange outline and text: look at this */
#define UI_CHIP_HOT   3        /* filled orange: cannot be missed (development mode) */

typedef struct {
    const char *label;
    uint8_t kind;
} ui_chip_t;

/* Clears the screen and draws the logo ("PURR OS!", bold italic, the "!" in orange) and the version line. */
void ui_frame(void);
/* The same with a small logo at the top, for the menu and problem screens. */
void ui_frame_small(void);

/* Text on the 8x8 font at scale 1: centred, or from the left margin (x = UI_MARGIN). */
#define UI_MARGIN 20
void ui_centred(int y, const char *s, uint16_t fg);
void ui_left(int y, const char *s, uint16_t fg);
/* A thin dim rule across the text margins. */
void ui_rule(int y);
/* A clear band across the whole width, from y for h pixels. */
void ui_clear_rect(int y, int h);
/* A menu row, 24 px tall (rows at y, y+24, ...): name on the left, tag on the right. A selected row has an orange border. tag_hot draws the tag
 * orange; grey is for "unusable" and plain tags. usable 0 draws the name grey. */
void ui_menu_row(int y, const char *name, const char *tag, int tag_hot, int selected, int usable);
/* Draws the chips centred, dropping the last ones if they would not fit. */
void ui_chips(const ui_chip_t *chips, int n);
/* The bottom band below the chips, cleared. */
void ui_clear_band(void);

/* One menu row, 0-based. A selected row has a border. tag is drawn at the right (orange if "default"). */
void ui_row(int index, const char *name, const char *tag, int selected, int usable);

/* The bar: per_mille 0..1000, and the line above it (left text, right text; the right one only if it fits). The
 * status chips go under the bar. */
void ui_bar(int per_mille);
void ui_above(const char *left, const char *right);

/* A whole screen for a stage that has a name and progress: frame, stage line, bar, chips. */
void ui_splash(const ui_chip_t *chips, int n, const char *stage, const char *right, int per_mille);

#if BOARD_HAS_KEYBOARD
#define UI_BAR_STEPS 40        /* redraw the countdown bar this often over its length */
#else
#define UI_BAR_STEPS 3         /* a slow panel: only a few refreshes */
#endif

#endif /* PKG_UI_H */
