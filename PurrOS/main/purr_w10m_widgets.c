/* purr_w10m_widgets.c - see purr_w10m_widgets.h. These are the Settings app's helpers, moved here unchanged. */
#include "purr_w10m_widgets.h"

#include "purr_w10m_theme.h"

lv_obj_t *w10m_text(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    purr_theme_apply(l, PURR_TH_TEXT);
    return l;
}

lv_obj_t *w10m_dim(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    purr_theme_apply(l, PURR_TH_DIM);
    return l;
}

lv_obj_t *w10m_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    if (purr_theme_type_look()) {                       /* ink, spaced, with a short rule in the accent under it (the accent as text may not read on paper) */
        purr_theme_apply(l, PURR_TH_TEXT);
        lv_obj_set_style_text_letter_space(l, 2, 0);
        lv_obj_set_style_border_side(l, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(l, 3, 0);
        lv_obj_set_style_border_color(l, purr_theme_accent_color(), 0);
        lv_obj_set_style_pad_bottom(l, 2, 0);
        return l;
    }
    purr_theme_apply(l, PURR_TH_ACCENT_TEXT);
    return l;
}

void w10m_space(lv_obj_t *parent, int h)
{
    lv_obj_t *s = lv_obj_create(parent);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, 1, h);
}

lv_obj_t *w10m_switch(lv_obj_t *parent, const char *text, int on, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), purr_theme_type_look() ? 36 : 30);
    purr_theme_row_divider(row);
    lv_obj_t *l = w10m_text(row, text);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 44, 22);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

lv_obj_t *w10m_slider(lv_obj_t *parent, int value, int min, int max, lv_event_code_t when, lv_event_cb_t cb)
{
    lv_obj_t *sl = lv_slider_create(parent);
    lv_obj_set_width(sl, LV_PCT(92));
    lv_obj_set_height(sl, 8);
    lv_slider_set_range(sl, min, max);
    lv_slider_set_value(sl, value, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, cb, when, NULL);
    lv_obj_set_ext_click_area(sl, 8);
    return sl;
}

lv_obj_t *w10m_dropdown(lv_obj_t *parent, const char *options, int selected, int width, lv_event_cb_t cb)
{
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_selected(dd, (uint32_t)selected);
    lv_obj_set_width(dd, width);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return dd;
}

static lv_obj_t *s_last_button;

static void last_button_cb(lv_event_t *e)
{
    lv_obj_t *b = lv_event_get_target(e);
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        if (s_last_button == b) {
            s_last_button = NULL;
        }
    } else {
        s_last_button = b;
    }
}

lv_obj_t *w10m_last_button(void)
{
    return s_last_button;
}

lv_obj_t *w10m_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_add_event_cb(b, last_button_cb, LV_EVENT_ALL, NULL);               /* so an error can be shown right under the button that failed */
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

void w10m_button_text(lv_obj_t *b, const char *text)
{
    lv_label_set_text(lv_obj_get_child(b, 0), text);
}

/* A one-line text box on the hardware keyboard; every box on a page joins the page's keypad group. */
lv_obj_t *w10m_input(lv_obj_t *parent, lv_group_t *group, const char *placeholder, int secret)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_password_mode(ta, secret != 0);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, 0);
    if (group != NULL) {
        lv_group_add_obj(group, ta);
    }
    return ta;
}

/* A tappable row: title over a dim line, optional right-hand text. */
lv_obj_t *w10m_row(lv_obj_t *parent, const char *title, const char *sub, int enabled, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 38);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
    if (cb != NULL && enabled) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        purr_theme_apply(row, PURR_TH_ROW_PRESSED);
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, ud);
    }
    lv_obj_set_style_opa(row, enabled ? LV_OPA_COVER : LV_OPA_40, 0);
    int type = purr_theme_type_look();
    if (type) {                                         /* the Type layout: taller, the title larger, a hairline under it and a chevron when it opens something */
        lv_obj_set_height(row, 46);
        purr_theme_row_divider(row);
    }
    lv_obj_t *t = w10m_text(row, title);
    if (type) {
        lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    }
    lv_obj_align(t, LV_ALIGN_LEFT_MID, type ? 6 : 2, sub ? (type ? -9 : -8) : 0);
    if (sub != NULL) {
        lv_obj_t *d = lv_label_create(row);
        lv_label_set_text(d, sub);
        lv_obj_set_style_text_font(d, &lv_font_montserrat_12, 0);
        purr_theme_apply(d, PURR_TH_DIM);
        lv_obj_align(d, LV_ALIGN_LEFT_MID, type ? 6 : 2, type ? 10 : 9);
    }
    if (type && cb != NULL && enabled) {
        lv_obj_t *c = lv_label_create(row);
        lv_label_set_text(c, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_font(c, &lv_font_montserrat_14, 0);
        lv_obj_align(c, LV_ALIGN_RIGHT_MID, -6, 0);
    }
    return row;
}

