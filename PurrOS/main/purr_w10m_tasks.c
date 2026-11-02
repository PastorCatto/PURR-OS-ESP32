/*
 * purr_w10m_tasks.c - the task view: swipe up from the bottom edge of the screen and every running app is a card (plan7.md, "Multitasking").
 *
 * A card is a square with the app's initial and name and a close button in its corner. Tapping a card brings that app to the front; the close button
 * closes it (the runtime saves its state first). The app in front has an accent outline. Apps with no screen (services, such as the Store's companion)
 * are not cards: they are listed in one line along the bottom. Tapping the background, or swiping up again, goes back. UI task only.
 */
#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#include "purr_catrt.h"
#include "purr_runtime.h"
#include "purr_w10m_priv.h"
#include "purr_w10m_theme.h"

#define GAP 8
#define MAX_TASKS 8

static lv_obj_t *s_view, *s_row, *s_empty, *s_services, *s_title, *s_home;

static void rebuild(void);

static void y_cb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

void w10m_tasks_close(void)
{
    if (s_view != NULL) {
        lv_obj_add_flag(s_view, LV_OBJ_FLAG_HIDDEN);
    }
}

int w10m_tasks_is_open(void)
{
    return s_view != NULL && !lv_obj_has_flag(s_view, LV_OBJ_FLAG_HIDDEN);
}

static void card_clicked(lv_event_t *e)
{
    purr_catrt_focus_slot((int)(intptr_t)lv_event_get_user_data(e));
    w10m_tasks_close();
}

static void refresh_cb(lv_timer_t *t)
{
    (void)t;
    if (w10m_tasks_is_open()) {
        rebuild();
    }
}

static void close_clicked(lv_event_t *e)
{
    purr_catrt_close_slot((int)(intptr_t)lv_event_get_user_data(e));
    lv_timer_t *t = lv_timer_create(refresh_cb, 500, NULL);         /* the app is gone a moment later: draw the cards again */
    lv_timer_set_repeat_count(t, 1);
}

static void home_clicked(lv_event_t *e)
{
    (void)e;
    purr_catrt_home();                                              /* the app in front steps back; it is not closed */
    w10m_tasks_close();
}

static void background_clicked(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_view) {
        w10m_tasks_close();
    }
}

static void make_card(const purr_catrt_task_t *t, int side)
{
    lv_obj_t *card = lv_obj_create(s_row);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, side, side);
    purr_theme_apply(card, PURR_TH_SURFACE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    if (t->foreground) {
        lv_obj_set_style_outline_width(card, 2, 0);
        lv_obj_set_style_outline_color(card, purr_theme_accent_color(), 0);
    }
    lv_obj_add_event_cb(card, card_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)t->slot);

    char initial[2] = {t->name[0] >= 'a' && t->name[0] <= 'z' ? (char)(t->name[0] - 32) : t->name[0], '\0'};
    lv_obj_t *letter = lv_label_create(card);
    lv_label_set_text(letter, initial);
    lv_obj_set_style_text_font(letter, &lv_font_montserrat_40, 0);
    purr_theme_apply(letter, PURR_TH_ACCENT_TEXT);
    lv_obj_align(letter, LV_ALIGN_CENTER, 0, -8);

    char name[32];
    snprintf(name, sizeof(name), "%s", t->name);
    if (name[0] >= 'a' && name[0] <= 'z') {
        name[0] = (char)(name[0] - 32);
    }
    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, name);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, side - 8);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    purr_theme_apply(label, PURR_TH_TEXT);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -6);

    lv_obj_t *x = lv_obj_create(card);                              /* the close button: a larger tap area than its glyph */
    lv_obj_remove_style_all(x);
    lv_obj_set_size(x, 32, 32);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_flag(x, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(x, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(x, close_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)t->slot);
    lv_obj_t *glyph = lv_label_create(x);
    lv_label_set_text(glyph, LV_SYMBOL_CLOSE);
    purr_theme_apply(glyph, PURR_TH_TEXT);
    lv_obj_center(glyph);
}

static void layout(void)
{
    int w = w10m_disp_w(), h = w10m_disp_h();
    lv_obj_set_size(s_view, w, h - W10M_STATUS_H);
    lv_obj_set_pos(s_view, 0, W10M_STATUS_H);
    lv_obj_set_width(s_row, w);
    lv_obj_align(s_row, LV_ALIGN_LEFT_MID, 0, -4);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 10, 6);
    lv_obj_align(s_home, LV_ALIGN_TOP_RIGHT, -10, 4);
    lv_obj_align(s_empty, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_width(s_services, w - 20);
    lv_obj_align(s_services, LV_ALIGN_BOTTOM_LEFT, 10, -8);
}

static void rebuild(void)
{
    purr_catrt_task_t t[PURR_RT_MAX_APPS];
    int n = purr_catrt_tasks(t, PURR_RT_MAX_APPS);
    int w = w10m_disp_w(), h = w10m_disp_h();
    int across = 3;
    int side = (w - 2 * 10 - (across - 1) * GAP) / across;
    int room = h - W10M_STATUS_H - 70;                              /* the title above, the services line below */
    if (side > room) {
        side = room;
    }
    lv_obj_clean(s_row);
    char services[120] = "";
    int cards = 0;
    for (int i = 0; i < n && cards < MAX_TASKS; i++) {
        if (t[i].service) {
            size_t len = strlen(services);
            snprintf(services + len, sizeof(services) - len, "%s%s", len ? ", " : "", t[i].name);
            continue;
        }
        make_card(&t[i], side);
        cards++;
    }
    if (cards == 0) {
        lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    }
    if (services[0] != '\0') {
        char line[160];
        snprintf(line, sizeof(line), "Running in the background: %s", services);
        lv_label_set_text(s_services, line);
        lv_obj_remove_flag(s_services, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_services, LV_OBJ_FLAG_HIDDEN);
    }
    layout();
}

static void build(void)
{
    s_view = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_view);
    purr_theme_apply(s_view, PURR_TH_SHEET);
    lv_obj_clear_flag(s_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_view, background_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_move_to_index(s_view, 0);                                /* above the app windows, beneath the status strip, the control center and the lock screen */

    s_title = lv_label_create(s_view);
    lv_label_set_text(s_title, "Apps");
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_16, 0);
    purr_theme_apply(s_title, PURR_TH_ACCENT_TEXT);

    s_home = lv_obj_create(s_view);                                 /* leave the app in front running and go to the start screen */
    lv_obj_remove_style_all(s_home);
    lv_obj_set_size(s_home, 78, 26);
    purr_theme_apply(s_home, PURR_TH_ACCENT_BG);
    lv_obj_clear_flag(s_home, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_home, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_home, home_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *home_label = lv_label_create(s_home);
    lv_label_set_text(home_label, LV_SYMBOL_HOME "  Home");
    lv_obj_set_style_text_font(home_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(home_label, lv_color_white(), 0);
    lv_obj_center(home_label);

    s_row = lv_obj_create(s_view);
    lv_obj_remove_style_all(s_row);
    lv_obj_set_height(s_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_row, GAP, 0);
    lv_obj_set_style_pad_hor(s_row, 10, 0);
    lv_obj_set_scroll_dir(s_row, LV_DIR_HOR);
    lv_obj_remove_flag(s_row, LV_OBJ_FLAG_CLICKABLE);               /* a tap on empty space reaches the view behind it and goes back; cards still scroll the row */
    lv_obj_set_scrollbar_mode(s_row, LV_SCROLLBAR_MODE_OFF);

    s_empty = lv_label_create(s_view);
    lv_label_set_text(s_empty, "No apps open");
    lv_obj_set_style_text_font(s_empty, &lv_font_montserrat_16, 0);
    purr_theme_apply(s_empty, PURR_TH_DIM);

    s_services = lv_label_create(s_view);
    lv_label_set_long_mode(s_services, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_services, &lv_font_montserrat_12, 0);
    purr_theme_apply(s_services, PURR_TH_DIM);
}

void w10m_tasks_open(void)
{
    if (w10m_is_locked()) {
        return;                                                     /* nothing behind the lock screen is reachable */
    }
    if (s_view == NULL) {
        build();
    }
    if (w10m_tasks_is_open()) {
        w10m_tasks_close();                                         /* a second swipe goes back */
        return;
    }
    rebuild();
    lv_obj_remove_flag(s_view, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;                                                    /* slides up from the bottom */
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_view);
    lv_anim_set_exec_cb(&a, y_cb);
    lv_anim_set_values(&a, w10m_disp_h(), W10M_STATUS_H);
    lv_anim_set_duration(&a, 160);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}
