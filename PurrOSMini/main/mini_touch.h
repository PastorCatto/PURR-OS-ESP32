/* mini_touch.h - the CST816S capacitive touch panel of the CYD (I2C), and its calibration. The panel reports in its own portrait grid (about 240 x 320); how that maps onto the landscape screen differs
 * between boards, so the Mini measures it: four taps on crosses at the corners (mini_ui_calibrate) give the swap, the flips and the scale, which are kept in NVS. */
#ifndef MINI_TOUCH_H
#define MINI_TOUCH_H

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    bool down;
    int x, y;                            /* in the screen's pixels, through the calibration */
} mini_touch_t;

/* axes after the optional swap: x' = swap ? raw_y : raw_x, y' = swap ? raw_x : raw_y; the screen x is 20 at x' = ax0 and 300 at ax1, the screen y is 20 at y' = ay0 and 220 at ay1 */
typedef struct {
    int valid;
    int swap;
    int ax0, ax1, ay0, ay1;
} mini_cal_t;

#define MINI_CAL_INSET 20

esp_err_t mini_touch_init(void);
/* Reads the panel now. Always fills *t (down = false when nothing touches or the panel does not answer: it sleeps between touches). */
void mini_touch_read(mini_touch_t *t);
/* The panel's own numbers, no calibration. */
void mini_touch_read_raw(bool *down, int *x, int *y);

bool mini_touch_cal_load(void);          /* from NVS: true if there was one */
void mini_touch_cal_set(const mini_cal_t *c);   /* use it and keep it in NVS */
void mini_touch_cal_clear(void);

#endif
