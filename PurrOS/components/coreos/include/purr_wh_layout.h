/*
 * purr_wh_layout.h - how the Click Wheel shell sizes itself for a display (plan7.md, "The Click Wheel shell").
 *
 * The shell is drawn once and adapts: it asks this file for a set of measures (the height of the status bar, of a list row, the font sizes, the size of the
 * cover, whether a list has a side panel) and lays everything out from those, never from a literal. Two ways to get them:
 *
 *   - a PROFILE: a pre-made set for a display the project knows (the T-Deck's and Playtime's 320 x 240 colour panel, the Waveshare 1.54 inch 200 x 200 e-ink),
 *     looked up by the exact size and the kind of panel, tuned by hand against the design;
 *   - COMPUTED: for any other size, the same measures worked out from the shorter side, so an unknown panel still gets a usable layout.
 *
 * Plain C, no dependencies, so the host tests cover every branch. The shell turns the font tiers into real fonts, and the measures into LVGL calls.
 */
#ifndef PURR_WH_LAYOUT_H
#define PURR_WH_LAYOUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PURR_WH_WIDE = 0,       /* wider than tall by a quarter or more (320 x 240): lists have a side panel, the lock screen has two columns */
    PURR_WH_SQUARE = 1,     /* about as wide as tall (200 x 200): full-width lists, the lock screen stacks */
    PURR_WH_TALL = 2        /* taller than wide (a rotated 240 x 320): the same as square, with more rows */
} purr_wh_shape_t;

typedef enum {
    PURR_WH_FROM_PROFILE = 0,
    PURR_WH_FROM_RULES = 1
} purr_wh_source_t;

/* Font tiers: the shell maps them to its typefaces (small: 12 to 14 px, medium: 16 px, large: 20 px). */
#define PURR_WH_FONT_SMALL  0
#define PURR_WH_FONT_MEDIUM 1
#define PURR_WH_FONT_LARGE  2

typedef struct {
    int w, h;               /* the display, in pixels, as it is turned now */
    uint8_t mono;           /* 1: a black and white panel (no gradients, no shadows, no accent colour, no reflection) */
    uint8_t shape;          /* purr_wh_shape_t */
    uint8_t source;         /* purr_wh_source_t */
    uint8_t font_row;       /* the names in a list */
    uint8_t font_title;     /* the status bar's title and the track on the lock screen */
    uint8_t font_small;     /* labels, times, the "2 new messages" line */
    uint8_t clock_tier;     /* 0, 1, 2: the clock on the lock screen and in the side panel (small, medium, large) */
    uint8_t split;          /* 1: a list has a side panel (cover and clock) next to it; 0: it uses the whole width */
    uint8_t reflection;     /* 1: the cover has a reflection under it (colour and wide only) */
    int bar_h;              /* the status bar */
    int row_h;              /* a row of a list */
    int rows;               /* whole rows that fit under the status bar */
    int pad;                /* the edge padding of a row, and the gap between things */
    int icon;               /* the status bar's icons, and the tile of an app in a list */
    int cover;              /* the side of the cover on the playing screens */
    int panel_w;            /* the width of the side panel when split, else 0 */
} purr_wh_metrics_t;

/* Fills `m` for a w x h display (`mono` for a black and white panel). 0, or -1 for a size that is not a display (a side under 80 pixels, or over 4096). */
int purr_wh_layout(int w, int h, int mono, purr_wh_metrics_t *m);

/* 1 if (w, h, mono) has a profile of its own (what `purr_wh_layout` will return with source PURR_WH_FROM_PROFILE). */
int purr_wh_has_profile(int w, int h, int mono);

/* "320x240 colour wide, profile, 8 rows of 26" for the log. */
void purr_wh_describe(const purr_wh_metrics_t *m, char *out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* PURR_WH_LAYOUT_H */
