/*
 * purr_w10m_widgets.h - the Settings app's widget helpers, shared (plan7.md; the OOBE uses them too). Each builds one themed
 * LVGL widget on a parent: the look comes from purr_w10m_theme.h, so everything made with them follows the accent, the
 * light or dark base and the transparency setting. UI service task only.
 */
#ifndef PURR_W10M_WIDGETS_H
#define PURR_W10M_WIDGETS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *w10m_text(lv_obj_t *parent, const char *text);                    /* 14 pt, normal */
lv_obj_t *w10m_dim(lv_obj_t *parent, const char *text);                     /* 12 pt, dim, wraps */
lv_obj_t *w10m_title(lv_obj_t *parent, const char *text);                   /* 16 pt, accent */
void w10m_space(lv_obj_t *parent, int h);
lv_obj_t *w10m_switch(lv_obj_t *parent, const char *text, int on, lv_event_cb_t cb);
lv_obj_t *w10m_slider(lv_obj_t *parent, int value, int min, int max, lv_event_code_t when, lv_event_cb_t cb);
lv_obj_t *w10m_dropdown(lv_obj_t *parent, const char *options, int selected, int width, lv_event_cb_t cb);
lv_obj_t *w10m_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud);
void w10m_button_text(lv_obj_t *b, const char *text);
/* A one-line text box; it joins `group` (the page's keypad group) so the hardware keyboard types into it. */
lv_obj_t *w10m_input(lv_obj_t *parent, lv_group_t *group, const char *placeholder, int secret);
/* A tappable row: title over a dim line. */
lv_obj_t *w10m_row(lv_obj_t *parent, const char *title, const char *sub, int enabled, lv_event_cb_t cb, void *ud);

#ifdef __cplusplus
}
#endif

#endif /* PURR_W10M_WIDGETS_H */
