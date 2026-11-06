/* SPDX-License-Identifier: Apache-2.0
 * The Android 2.2 (Froyo) style shell is licensed under the Apache License, Version 2.0, the licence the Android Open Source Project uses, in respect of that project.
 * The licence text is in UI/android-assets/NOTICE; the rest of PURR OS is under the GNU GPL version 3 (LICENSE). See THIRD_PARTY.md. */
/*
 * purr_android_shade.c - the notification shade of the Android style shell (purr_android.c, plan7.md "Android shell build-out").
 *
 * Froyo's window shade: a grey panel that comes down from the status strip over a dimmed screen. A header with the product name and the date,
 * an Ongoing section (the task manager, and developer mode when it is on), a Notifications section with a Clear notifications button, and
 * a grip bar at the bottom. Tapping a notification dismisses it. The dim area below the panel, the grip, Back and Menu close it.
 *
 * Notifications come from purr_shell_notify, w10m_note_post and the shell itself; a new one also ticks across the status strip for five
 * seconds (w10m_banner) and leaves a small icon on the strip until it is dismissed. At most eight are kept; the oldest goes first.
 */
#include "purr_android.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "lvgl.h"

#include "commands.h"
#include "purr_android_priv.h"
#include "purr_brand.h"
#include "purr_catrt.h"
#include "purr_w10m_theme.h"

#define MAX_NOTES 8
#define HEAD_H    18
#define LABEL_H   17
#define ROW_H     33
#define GRIP_H    14

static lv_obj_t *s_shade;               /* the dimmed area, between the two strips */
static lv_obj_t *s_panel;               /* the grey panel that slides down */
static lv_obj_t *s_date;
static lv_obj_t *s_task_sub;
static lv_obj_t *s_dev_row;
static lv_obj_t *s_notes;               /* the notification rows, newest first */
static lv_obj_t *s_empty;
static lv_obj_t *s_clear;
static int s_open;

static int area_h(void) { return w10m_disp_h() - AND_STATUS_H - AND_KEYS_H; }

/* ---------------------------------------------------------------- pieces */

static lv_obj_t *bar(lv_obj_t *parent, int h, uint32_t top, uint32_t bottom)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_PCT(100), h);
    lv_obj_set_style_bg_color(b, lv_color_hex(top), 0);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(bottom), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    return b;
}

static lv_obj_t *bar_label(lv_obj_t *b, const char *text, uint32_t color, lv_align_t align, int x)
{
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_align(l, align, x, 0);
    return l;
}

/* A notification or ongoing row: a light grey bar with a small icon, a title and a line of text. Orange while pressed. */
static lv_obj_t *make_row(lv_obj_t *parent, const char *title, const char *text, lv_event_cb_t cb, void *ud, lv_obj_t **sub_out)
{
    lv_obj_t *r = bar(parent, ROW_H, 0xE9E9E9, 0xCFCFCF);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(0x8A8A8A), 0);
    lv_obj_set_style_bg_color(r, lv_color_lighten(purr_theme_accent_color(), LV_OPA_30), LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_color(r, purr_theme_accent_color(), LV_STATE_PRESSED);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    if (cb != NULL) {
        lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
    }
    const lv_image_dsc_t *d = w10m_img("fr_s_app");
    if (d != NULL) {
        lv_obj_t *im = lv_image_create(r);
        lv_image_set_src(im, d);
        lv_obj_align(im, LV_ALIGN_LEFT_MID, 5, 0);
        lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t *a = lv_label_create(r);
    lv_label_set_text(a, title);
    lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
    lv_obj_set_width(a, w10m_disp_w() - 34);
    lv_obj_set_style_text_font(a, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(a, lv_color_black(), 0);
    lv_obj_align(a, LV_ALIGN_TOP_LEFT, 26, 2);
    lv_obj_t *b = lv_label_create(r);
    lv_label_set_text(b, text);
    lv_label_set_long_mode(b, LV_LABEL_LONG_DOT);
    lv_obj_set_width(b, w10m_disp_w() - 34);
    lv_obj_set_style_text_font(b, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(b, lv_color_hex(0x333333), 0);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 26, 16);
    if (sub_out != NULL) {
        *sub_out = b;
    }
    return r;
}

/* ---------------------------------------------------------------- opening and closing */

static void panel_y_cb(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }

static void closed_cb(lv_anim_t *a)
{
    (void)a;
    if (!s_open) {
        lv_obj_add_flag(s_shade, LV_OBJ_FLAG_HIDDEN);                   /* nothing to draw while it is shut */
    }
}

static void slide(int open)
{
    lv_obj_update_layout(s_panel);
    int ph = lv_obj_get_height(s_panel);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_panel);
    lv_anim_set_exec_cb(&a, panel_y_cb);
    lv_anim_set_values(&a, lv_obj_get_y(s_panel), open ? 0 : -ph);
    lv_anim_set_duration(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, closed_cb);
    lv_anim_start(&a);
}

static void refresh_ongoing(void)
{
    static purr_catrt_task_t tasks[16];
    int n = purr_catrt_tasks(tasks, 16);
    char line[64];
    snprintf(line, sizeof(line), "%d running, %u KB fast RAM free", n, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    lv_label_set_text(s_task_sub, line);
    if (purr_security_dev_active()) {
        lv_obj_remove_flag(s_dev_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_dev_row, LV_OBJ_FLAG_HIDDEN);
    }
    time_t now = time(NULL);
    if (now > 1000000000) {
        struct tm tm;
        char db[32];
        localtime_r(&now, &tm);
        strftime(db, sizeof(db), "%a, %B %d", &tm);
        lv_label_set_text(s_date, db);
    } else {
        lv_label_set_text(s_date, "clock not set");
    }
}

void and_shade_toggle(void)
{
    if (s_open) {
        and_shade_close();
        return;
    }
    and_menu_close();
    s_open = 1;
    refresh_ongoing();
    lv_obj_remove_flag(s_shade, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(s_panel);
    lv_obj_set_y(s_panel, -lv_obj_get_height(s_panel));
    slide(1);
}

void and_shade_close(void)
{
    if (s_shade == NULL || !s_open) {
        return;
    }
    s_open = 0;
    slide(0);
}

int and_shade_is_open(void) { return s_open; }

/* ---------------------------------------------------------------- notifications */

int and_shade_count(void) { return s_notes != NULL ? (int)lv_obj_get_child_count(s_notes) : 0; }

/* A notification row in the shade's own look (a light grey bar, the small app icon, a title and a line), for the lock screen; not clickable there. */
lv_obj_t *and_note_row(lv_obj_t *parent, const char *title, const char *text)
{
    lv_obj_t *r = make_row(parent, title, text, NULL, NULL, NULL);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(r, LV_PCT(100));
    return r;
}

/* A notification arrived (purr_notify.c, on the UI task): it goes in the shade, whose rows the lock screen shows too. */
void purr_shell_notified(const char *title, const char *text) { and_shade_add(title, text); }

int and_shade_peek(int i, char *title, size_t tcap, char *text, size_t xcap)
{
    if (s_notes == NULL || i < 0 || i >= and_shade_count()) {
        return -1;
    }
    lv_obj_t *row = lv_obj_get_child(s_notes, i);
    int seen = 0;
    title[0] = text[0] = '\0';
    for (uint32_t k = 0; row != NULL && k < lv_obj_get_child_count(row); k++) {       /* a row is an icon, then the title label, then the text label */
        lv_obj_t *c = lv_obj_get_child(row, k);
        if (lv_obj_check_type(c, &lv_label_class)) {
            snprintf(seen == 0 ? title : text, seen == 0 ? tcap : xcap, "%s", lv_label_get_text(c));
            if (++seen == 2) {
                break;
            }
        }
    }
    return seen > 0 ? 0 : -1;
}

static void note_changed(void)
{
    int n = and_shade_count();
    if (n == 0) {
        lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_clear, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_clear, LV_OBJ_FLAG_HIDDEN);
    }
    and_status_notes(n);
    and_lock_notes_refresh();
}

static void note_clicked(lv_event_t *e)
{
    lv_obj_delete(lv_event_get_target(e));
    note_changed();
}

void and_shade_add(const char *title, const char *text)
{
    if (s_notes == NULL) {
        return;
    }
    while (and_shade_count() >= MAX_NOTES) {
        lv_obj_delete(lv_obj_get_child(s_notes, and_shade_count() - 1));
    }
    lv_obj_t *r = make_row(s_notes, title, text, note_clicked, NULL, NULL);
    lv_obj_move_to_index(r, 0);
    note_changed();
    char line[96];
    snprintf(line, sizeof(line), "%s: %s", title, text);               /* a new notification takes the strip for 5 seconds */
    w10m_banner(line, 0);
}

static void clear_clicked(lv_event_t *e)
{
    (void)e;
    lv_obj_clean(s_notes);
    note_changed();
}

static void tasks_clicked(lv_event_t *e)
{
    (void)e;
    and_shade_close();
    w10m_tasks_open();
}

static void dev_clicked(lv_event_t *e)
{
    (void)e;
    and_shade_close();
    and_open_app("Developer Portal", 2);
}

static void scrim_clicked(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_shade) {
        and_shade_close();
    }
}

static void grip_clicked(lv_event_t *e)
{
    (void)e;
    and_shade_close();
}

/* ---------------------------------------------------------------- build and layout */

void and_shade_build(void)
{
    s_shade = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_shade);
    lv_obj_set_style_bg_color(s_shade, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_shade, LV_OPA_60, 0);
    lv_obj_clear_flag(s_shade, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_shade, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_shade, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_shade, scrim_clicked, LV_EVENT_CLICKED, NULL);

    s_panel = lv_obj_create(s_shade);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_width(s_panel, LV_PCT(100));
    lv_obj_set_height(s_panel, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x2C2C2C), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *head = bar(s_panel, HEAD_H, 0x3A3A3A, 0x2A2A2A);
    bar_label(head, PURR_BRAND_PRODUCT, 0xC8C8C8, LV_ALIGN_LEFT_MID, 6);
    s_date = bar_label(head, "", 0xC8C8C8, LV_ALIGN_RIGHT_MID, -6);

    lv_obj_t *ongoing = bar(s_panel, LABEL_H, 0x5A5A5A, 0x3F3F3F);
    bar_label(ongoing, "Ongoing", 0xE8E8E8, LV_ALIGN_LEFT_MID, 6);
    make_row(s_panel, "Task manager", "", tasks_clicked, NULL, &s_task_sub);
    s_dev_row = make_row(s_panel, "Developer mode", "Open the Developer Portal", dev_clicked, NULL, NULL);

    lv_obj_t *notif = bar(s_panel, LABEL_H, 0x5A5A5A, 0x3F3F3F);
    bar_label(notif, "Notifications", 0xE8E8E8, LV_ALIGN_LEFT_MID, 6);
    s_clear = lv_obj_create(notif);                                     /* the Clear notifications button */
    lv_obj_remove_style_all(s_clear);
    purr_theme_apply(s_clear, PURR_TH_BUTTON);
    lv_obj_set_size(s_clear, 122, LABEL_H - 3);
    lv_obj_align(s_clear, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_clear_flag(s_clear, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_clear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_clear, clear_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(s_clear);
    lv_label_set_text(cl, "Clear notifications");
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(cl, purr_theme_button_text_color(), 0);
    lv_obj_center(cl);

    s_notes = lv_obj_create(s_panel);
    lv_obj_remove_style_all(s_notes);
    lv_obj_set_width(s_notes, LV_PCT(100));
    lv_obj_set_height(s_notes, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_notes, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_notes, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_notes, LV_SCROLLBAR_MODE_OFF);

    s_empty = bar(s_panel, ROW_H, 0xE9E9E9, 0xCFCFCF);
    lv_obj_t *el = bar_label(s_empty, "No notifications", 0x444444, LV_ALIGN_CENTER, 0);
    (void)el;

    lv_obj_t *grip = bar(s_panel, GRIP_H, 0x4A4A4A, 0x202020);          /* the handle at the bottom: tap it to close */
    lv_obj_add_flag(grip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(grip, grip_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bump = lv_obj_create(grip);
    lv_obj_remove_style_all(bump);
    lv_obj_set_size(bump, 40, 4);
    lv_obj_set_style_radius(bump, 2, 0);
    lv_obj_set_style_bg_color(bump, lv_color_hex(0x151515), 0);
    lv_obj_set_style_bg_opa(bump, LV_OPA_COVER, 0);
    lv_obj_center(bump);
    lv_obj_remove_flag(bump, LV_OBJ_FLAG_CLICKABLE);

    note_changed();
    and_shade_layout();
}

void and_shade_layout(void)
{
    if (s_shade == NULL) {
        return;
    }
    int w = w10m_disp_w();
    lv_obj_set_size(s_shade, w, area_h());
    lv_obj_set_pos(s_shade, 0, AND_STATUS_H);
    lv_obj_set_style_max_height(s_notes, area_h() - (HEAD_H + 2 * LABEL_H + 2 * ROW_H + GRIP_H + ROW_H), 0);
    if (!s_open) {
        lv_obj_add_flag(s_shade, LV_OBJ_FLAG_HIDDEN);
    }
}
