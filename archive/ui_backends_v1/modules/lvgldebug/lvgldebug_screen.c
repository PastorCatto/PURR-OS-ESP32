// lvgldebug_screen.c — the actual diagnostic UI
//
// Top: live readout of raw touch coords, mapped coords, and press state.
// A small dot follows the touch position exactly where LVGL thinks it is —
// if that dot doesn't appear under your actual finger, the mapping is
// wrong, visually obvious immediately instead of read from serial logs.
// Below that: a 3x3 grid of labeled buttons so LVGL's own hit-testing can
// confirm whether taps land on the widget you actually touched.

#include "lvgldebug.h"
#include "lvgl.h"
#include <stdio.h>

static lv_obj_t *s_readout_lbl;
static lv_obj_t *s_history_lbl;
static lv_obj_t *s_last_tap_lbl;
static lv_obj_t *s_dot;
static lv_timer_t *s_timer;

static void grid_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    char buf[32];
    snprintf(buf, sizeof(buf), "Last tap: button %d", idx + 1);
    lv_label_set_text(s_last_tap_lbl, buf);
}

// Cut from 3x3 to a single row of 3 — frees the rest of the screen for the
// true-raw history readout (see lvgldebug_hal_get_touch_history()).
static void build_grid(lv_obj_t *parent, int top_y, int area_h)
{
    lv_coord_t sw = lv_disp_get_hor_res(NULL);
    int cell_w = sw / 3;
    int cell_h = area_h;

    for (int col = 0; col < 3; col++) {
        int idx = col;
        lv_obj_t *btn = lv_btn_create(parent);
        lv_obj_set_size(btn, cell_w - 4, cell_h - 4);
        lv_obj_set_pos(btn, col * cell_w + 2, top_y + 2);

        lv_obj_t *lbl = lv_label_create(btn);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", idx + 1);
        lv_label_set_text(lbl, buf);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(btn, grid_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    }
}

static void update_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    uint16_t raw_x = 0, raw_y = 0;
    int32_t mapped_x = 0, mapped_y = 0;
    bool pressed = false;
    lvgldebug_hal_get_touch_debug(&raw_x, &raw_y, &mapped_x, &mapped_y, &pressed);

    char buf[80];
    snprintf(buf, sizeof(buf), "out=(%u,%u)  mapped=(%ld,%ld)  %s",
             raw_x, raw_y, (long)mapped_x, (long)mapped_y,
             pressed ? "PRESSED" : "released");
    lv_label_set_text(s_readout_lbl, buf);

    if (pressed) {
        lv_obj_clear_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_dot, (lv_coord_t)mapped_x - 4, (lv_coord_t)mapped_y - 4);
    } else {
        lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    }

    // True-raw history, straight off the chip, oldest first — same data
    // that's being logged to serial, just visible on-device too.
    uint16_t hx[LVD_HISTORY_LEN], hy[LVD_HISTORY_LEN];
    uint8_t n = lvgldebug_hal_get_touch_history(hx, hy);
    char hist_buf[200];
    int off = 0;
    off += snprintf(hist_buf + off, sizeof(hist_buf) - off, "true_raw history:\n");
    for (uint8_t i = 0; i < n && off < (int)sizeof(hist_buf) - 20; i++) {
        off += snprintf(hist_buf + off, sizeof(hist_buf) - off, "(%u,%u) ", hx[i], hy[i]);
    }
    lv_label_set_text(s_history_lbl, hist_buf);
}

void lvgldebug_screen_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_readout_lbl = lv_label_create(scr);
    lv_obj_set_style_text_color(s_readout_lbl, lv_color_white(), 0);
    lv_label_set_text(s_readout_lbl, "raw=(0,0)  mapped=(0,0)  released");
    lv_obj_set_pos(s_readout_lbl, 4, 4);

    s_last_tap_lbl = lv_label_create(scr);
    lv_obj_set_style_text_color(s_last_tap_lbl, lv_color_make(0x40, 0xE0, 0x40), 0);
    lv_label_set_text(s_last_tap_lbl, "Last tap: (none)");
    lv_obj_set_pos(s_last_tap_lbl, 4, 20);

    s_history_lbl = lv_label_create(scr);
    lv_obj_set_style_text_color(s_history_lbl, lv_color_make(0xFF, 0xC0, 0x40), 0);
    lv_label_set_long_mode(s_history_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_history_lbl, lvgldebug_hal_width() - 8);
    lv_label_set_text(s_history_lbl, "true_raw history:");
    lv_obj_set_pos(s_history_lbl, 4, 36);

    // Single row of 3 buttons pinned to the bottom — most of the screen is
    // now given to the history readout above.
    int grid_h   = 40;
    int grid_top = lvgldebug_hal_height() - grid_h;
    build_grid(scr, grid_top, grid_h);

    // Dot drawn on the top layer so it always sits above the grid buttons.
    s_dot = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, 8, 8);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_make(0xFF, 0x00, 0x00), 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);

    s_timer = lv_timer_create(update_timer_cb, 50, NULL);
}
