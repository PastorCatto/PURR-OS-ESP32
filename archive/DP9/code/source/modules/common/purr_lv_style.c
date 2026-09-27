// purr_lv_style.c — see purr_lv_style.h for the full picture.
#include "purr_lv_style.h"

void purr_lv_style_tray(lv_obj_t *obj)
{
    if (!obj) return;
    // RGB(20,20,20): dark enough to read as "black/grey", with enough
    // headroom above pure black for icon labels drawn over it to still
    // show real anti-aliasing instead of crushing to solid edges.
    lv_obj_set_style_bg_color(obj, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
}

void purr_lv_style_shade_bg(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_set_style_bg_color(obj, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
}

void purr_lv_style_dim_text(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_set_style_text_color(obj, lv_color_make(0xA0, 0xA0, 0xA0), 0);
}

void purr_lv_style_row_text(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_set_style_text_color(obj, lv_color_white(), 0);
}

void purr_lv_style_handle(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_set_style_bg_color(obj, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 3, 0);
}

void purr_lv_style_border(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_set_style_border_color(obj, lv_color_make(0x55, 0x55, 0x55), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
}

void purr_lv_style_card(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_set_style_bg_color(obj, lv_color_make(40, 40, 40), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 6, 0);
}

void purr_lv_style_white_bg(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
}

void purr_lv_style_strip_text(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_set_style_text_color(obj, lv_color_black(), 0);
}
