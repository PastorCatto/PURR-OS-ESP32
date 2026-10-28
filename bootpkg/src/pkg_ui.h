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

#define UI_BLACK  0x0000
#define UI_WHITE  0xFFFF
#define UI_GREY   0x94B2
#define UI_ORANGE 0xFDA0
#define UI_DIM    0x39E7       /* the empty part of the bar */
#define UI_GREEN  0x564F

/* Chip kinds. */
#define UI_CHIP_OK    0        /* grey outline, green dot: fine */
#define UI_CHIP_PLAIN 1        /* grey outline, no dot: just information */
#define UI_CHIP_WARN  2        /* orange outline and text: look at this */
#define UI_CHIP_HOT   3        /* filled orange: cannot be missed (development mode) */

typedef struct {
    const char *label;
    uint8_t kind;
} ui_chip_t;

/* Clears the screen and draws the mark, wordmark and version line. */
void ui_frame(void);
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
