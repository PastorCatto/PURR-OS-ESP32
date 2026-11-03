/*
 * purr_w10m_tasks.c - the task manager: every running app in a list, with what it holds and an End button (plan7.md, "Multitasking").
 *
 * Opened by the Tasks tile in the control center. A row shows the app's name, whether it is in front, paused or a background service, and the main
 * memory it holds; End closes it (the runtime saves its state first). Tapping a row brings that app to the front. The top shows how much fast (internal)
 * RAM and main memory are free: the internal RAM is what limits how many apps can run (plan6.md), so it is the number to watch. Home steps the app in
 * front back to the start screen without closing it. The list redraws when the set of apps changes and the numbers refresh every two seconds. UI task only.
 */
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "lvgl.h"

#include "purr_catrt.h"
#include "purr_runtime.h"
#include "purr_w10m_priv.h"
#include "purr_w10m_theme.h"

#define ROW_H 40
#define ROW_GAP 5
#define MAX_TASKS 12

static lv_obj_t *s_view, *s_list, *s_empty, *s_title, *s_home, *s_summary;
static lv_timer_t *s_tick;
static uint32_t s_sig;                                              /* what the list was last built from */

static void rebuild(void);

static void y_cb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

int w10m_tasks_is_open(void)
{
    return s_view != NULL && !lv_obj_has_flag(s_view, LV_OBJ_FLAG_HIDDEN);
}

void w10m_tasks_close(void)
{
    if (s_view != NULL) {
        lv_obj_add_flag(s_view, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_tick != NULL) {
        lv_timer_pause(s_tick);
    }
}

static void row_clicked(lv_event_t *e)
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

static void end_clicked(lv_event_t *e)
{
    purr_catrt_close_slot((int)(intptr_t)lv_event_get_user_data(e));
    lv_timer_t *t = lv_timer_create(refresh_cb, 500, NULL);         /* the app is gone a moment later: draw the list again */
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

static void fmt_bytes(char *out, size_t cap, uint32_t bytes)
{
    if (bytes >= 1024u * 1024u) {
        snprintf(out, cap, "%u.%u MB", (unsigned)(bytes >> 20), (unsigned)(((bytes & 0xFFFFFu) * 10u) >> 20));
    } else {
        snprintf(out, cap, "%u KB", (unsigned)((bytes + 512u) >> 10));
    }
}

static void make_row(const purr_catrt_task_t *t, int w)
{
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, w, ROW_H);
    purr_theme_apply(row, PURR_TH_SURFACE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if (!t->service) {                                              /* a service has no screen to bring forward */
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)t->slot);
    }
    if (t->foreground) {
        lv_obj_set_style_outline_width(row, 2, 0);
        lv_obj_set_style_outline_color(row, purr_theme_accent_color(), 0);
    }

    char name[32];
    snprintf(name, sizeof(name), "%s", t->name);
    if (name[0] >= 'a' && name[0] <= 'z') {
        name[0] = (char)(name[0] - 32);
    }
    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, name);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, w - 84);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    purr_theme_apply(label, PURR_TH_TEXT);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 8, 3);

    char mem[16], detail[64];
    fmt_bytes(mem, sizeof(mem), t->memory);
    const char *state = t->foreground ? "In front" : t->service ? "Service" : t->frozen ? "Paused" : "In the background";
    snprintf(detail, sizeof(detail), "%s  -  %s", state, mem);
    lv_obj_t *sub = lv_label_create(row);
    lv_label_set_text(sub, detail);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sub, w - 84);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    purr_theme_apply(sub, PURR_TH_DIM);
    lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 8, -4);

    lv_obj_t *end = lv_obj_create(row);                             /* End: a wide tap area, the whole right edge of the row */
    lv_obj_remove_style_all(end);
    lv_obj_set_size(end, 64, ROW_H - 10);
    purr_theme_apply(end, PURR_TH_ACCENT_BG);
    lv_obj_clear_flag(end, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(end, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(end, LV_ALIGN_RIGHT_MID, -5, 0);
    lv_obj_add_event_cb(end, end_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)t->slot);
    lv_obj_t *end_label = lv_label_create(end);
    lv_label_set_text(end_label, "End");
    lv_obj_set_style_text_font(end_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(end_label, lv_color_white(), 0);
    lv_obj_center(end_label);
}

static void layout(void)
{
    int w = w10m_disp_w(), h = w10m_disp_h();
    int vh = h - W10M_STATUS_H;
    lv_obj_set_size(s_view, w, vh);
    lv_obj_set_pos(s_view, 0, W10M_STATUS_H);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 10, 6);
    lv_obj_align(s_home, LV_ALIGN_TOP_RIGHT, -10, 4);
    lv_obj_set_width(s_summary, w - 20);
    lv_obj_align(s_summary, LV_ALIGN_TOP_LEFT, 10, 34);
    lv_obj_set_size(s_list, w - 20, vh - 56 - 6);
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 10, 56);
    lv_obj_align(s_empty, LV_ALIGN_CENTER, 0, 10);
}

static void update_summary(int apps)
{
    char fast[16], main_mem[16], line[96];
    fmt_bytes(fast, sizeof(fast), (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    fmt_bytes(main_mem, sizeof(main_mem), (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    snprintf(line, sizeof(line), "%d running  -  %s fast RAM free  -  %s memory free", apps, fast, main_mem);
    lv_label_set_text(s_summary, line);
}

static void rebuild(void)
{
    purr_catrt_task_t all[PURR_RT_MAX_APPS], t[PURR_RT_MAX_APPS];
    int n = purr_catrt_tasks(all, PURR_RT_MAX_APPS);
    int k = 0;                                                      /* in front first, then the rest, services last */
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < n && k < MAX_TASKS; i++) {
            int rank = all[i].foreground ? 0 : all[i].service ? 2 : 1;
            if (rank == pass) {
                t[k++] = all[i];
            }
        }
    }
    update_summary(k);

    uint32_t sig = 2166136261u;                                     /* the list only redraws when it would look different, so a scroll is not reset */
    for (int i = 0; i < k; i++) {
        sig = (sig ^ (uint32_t)t[i].id) * 16777619u;
        sig = (sig ^ (uint32_t)(t[i].foreground | (t[i].frozen << 1) | (t[i].service << 2))) * 16777619u;
        sig = (sig ^ (t[i].memory >> 10)) * 16777619u;
    }
    if (sig == s_sig && lv_obj_get_child_count(s_list) == (uint32_t)k) {
        layout();
        return;
    }
    s_sig = sig;
    lv_obj_clean(s_list);
    int w = w10m_disp_w() - 20;
    for (int i = 0; i < k; i++) {
        make_row(&t[i], w);
    }
    if (k == 0) {
        lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
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
    lv_label_set_text(s_title, "Task Manager");
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

    s_summary = lv_label_create(s_view);
    lv_label_set_long_mode(s_summary, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_summary, &lv_font_montserrat_12, 0);
    purr_theme_apply(s_summary, PURR_TH_DIM);

    s_list = lv_obj_create(s_view);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, ROW_GAP, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_CLICKABLE);              /* a tap on empty space reaches the view behind it and goes back; rows still scroll the list */

    s_empty = lv_label_create(s_view);
    lv_label_set_text(s_empty, "No apps running");
    lv_obj_set_style_text_font(s_empty, &lv_font_montserrat_16, 0);
    purr_theme_apply(s_empty, PURR_TH_DIM);

    s_tick = lv_timer_create(refresh_cb, 2000, NULL);               /* the free-memory numbers move; the list only redraws when it changes */
    lv_timer_pause(s_tick);
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
        w10m_tasks_close();                                         /* opening it again goes back */
        return;
    }
    s_sig = 0;
    rebuild();
    lv_obj_remove_flag(s_view, LV_OBJ_FLAG_HIDDEN);
    lv_timer_resume(s_tick);
    lv_anim_t a;                                                    /* slides up from the bottom */
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_view);
    lv_anim_set_exec_cb(&a, y_cb);
    lv_anim_set_values(&a, w10m_disp_h(), W10M_STATUS_H);
    lv_anim_set_duration(&a, 160);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}
