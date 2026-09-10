#pragma once
// lvgldebug.h — LVGL Debug public API
//
// Not a real desktop shell — a minimal LVGL screen for diagnosing touch
// mapping on new hardware/kernel combos. Shows live raw + mapped touch
// coordinates as text, draws a dot exactly where a tap is detected, and
// lays out a 3x3 grid of labeled buttons so LVGL's own hit-testing can
// confirm whether taps land on the right widget.

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

int  lvgldebug_init(void);
void lvgldebug_deinit(void);

int      lvgldebug_hal_init(void);
uint16_t lvgldebug_hal_width(void);
uint16_t lvgldebug_hal_height(void);

// Last-seen touch sample, for the debug screen's live readout. raw_x/raw_y
// are exactly what catcall_touch_t::read_point() returned (no transform);
// mapped_x/mapped_y are after the swap+mirror+scale this HAL applies before
// handing LVGL the point.
void lvgldebug_hal_get_touch_debug(uint16_t *raw_x, uint16_t *raw_y,
                                   int32_t *mapped_x, int32_t *mapped_y,
                                   bool *pressed);

// True-raw history straight off the GT911 chip (bypasses all of this
// driver's swap/mirror/scale/outlier-rejection). out_x/out_y must each have
// room for 8 entries. Returns how many entries were filled, oldest first.
#define LVD_HISTORY_LEN 8
uint8_t lvgldebug_hal_get_touch_history(uint16_t *out_x, uint16_t *out_y);

void lvgldebug_screen_init(void);

#ifdef __cplusplus
}
#endif
