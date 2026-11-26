/*
 * purr_wh_fonts.h - the Click Wheel shell's typeface: Source Sans 3 (Bold, Semibold, Regular), the face of the design boards. Bitmap fonts in LVGL's format, made by
 * tools/ui/gen_lvgl_font.py from the OFL-licensed typeface (UI/fonts/README.md); one C file per size in whfonts/.
 */
#ifndef PURR_WH_FONTS_H
#define PURR_WH_FONTS_H

#include "lvgl.h"

extern const lv_font_t wh_font_clock_xl;       /* 66 px Bold digits, colon and dash: the lock screen's clock */
extern const lv_font_t wh_font_clock_md;       /* 44 px Bold digits, colon and dash: the clock beside a cover, and in a list's side panel */
extern const lv_font_t wh_font_title;          /* 20 px Bold, ASCII: a track's name */
extern const lv_font_t wh_font_row;            /* 16 px Semibold, ASCII: the names in a list */
extern const lv_font_t wh_font_bar;            /* 13 px Bold, ASCII: the status bar's title */
extern const lv_font_t wh_font_date;           /* 14 px Semibold, ASCII: dates and sub lines */
extern const lv_font_t wh_font_small;          /* 12 px Regular, ASCII: labels, times, hints */

#endif /* PURR_WH_FONTS_H */
