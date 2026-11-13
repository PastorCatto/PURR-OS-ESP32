/*
 * purr_tp_fonts.h - the Type shell's typefaces: Space Grotesk Bold for the clocks, dates and names, Space Mono Bold for the small capital labels. Bitmap fonts in
 * LVGL's format, made by tools/ui/gen_lvgl_font.py from the OFL-licensed typefaces (UI/fonts/README.md says where they are from); one C file per size in tpfonts/.
 */
#ifndef PURR_TP_FONTS_H
#define PURR_TP_FONTS_H

#include "lvgl.h"

extern const lv_font_t tp_font_clock_xl;       /* 92 px digits and the colon: the home screen's clock */
extern const lv_font_t tp_font_clock_lock;     /* 116 px digits: the lock screen's hours and minutes */
extern const lv_font_t tp_font_clock_panel;    /* 60 px digits: the clock in a list's side panel */
extern const lv_font_t tp_font_date_big;       /* 56 px digits: the day of the month on the lock screen */
extern const lv_font_t tp_font_bold_26;        /* 26 px, ASCII: the lock screen's weekday and month */
extern const lv_font_t tp_font_bold_20;        /* 20 px, ASCII: the names in a list */
extern const lv_font_t tp_font_bold_16;        /* 16 px, ASCII: headings and big tile text */
extern const lv_font_t tp_font_mono_14;        /* 14 px mono, ASCII */
extern const lv_font_t tp_font_mono_12;        /* 12 px mono, ASCII: labels, hints, the strip */

#endif /* PURR_TP_FONTS_H */
