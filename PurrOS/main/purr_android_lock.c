/* SPDX-License-Identifier: Apache-2.0
 * The Android 2.2 (Froyo) style shell is licensed under the Apache License, Version 2.0, the licence the Android Open Source Project uses, in respect of that project.
 * The licence text is in UI/android-assets/NOTICE; the rest of PURR OS is under the GNU GPL version 3 (LICENSE). See THIRD_PARTY.md. */
/*
 * purr_android_lock.c - the lock screen and the sign-in screen of the Android style shell (purr_android.c, plan7.md).
 *
 * The lock screen is Froyo's, in its landscape form: the time and date at the left with two status lines under them (the LoRa radio's last-heard
 * bars, in place of a carrier, and the battery), a vertical slider at the right edge that is dragged up, and "Slide to unlock" under it.
 * It is only a lock, not a sign-in. The first unlock of a boot is the sign-in screen: the accounts as a list, then the password for
 * the one picked. Nothing else is reachable until it succeeds, and nothing is started for a user before then (the runtime's exec-once apps wait
 * for it too). The password check is a long PBKDF2 hash, so it runs on its own task and the screen stays live.
 *
 * Both sit on the top layer beneath the status strip; the lock screen hides the key strip (and_keys_visible), so it has the whole screen and the slider is how it unlocks. A portrait
 * screen gets a centred clock and a horizontal slider.
 */
#include "purr_android.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "commands.h"
#include "login.h"
#include "purr_resetnote_os.h"
#include "purr_android_priv.h"
#include "purr_battery.h"
#include "purr_brand.h"
#include "purr_lvgl.h"
#include "purr_radio_bars.h"
#include "purr_settings.h"
#include "purr_ui_service.h"
#include "purr_w10m_theme.h"
#include "purr_w10m_widgets.h"

static const char *TAG = "android-lock";

#define CARD_TITLE_H 20
#define ROW_H        34
#define ORANGE       0xFFA21F

static void backdrop(lv_obj_t *o)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(0x1D6F78), 0);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(0x071722), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

static void fmt_hm(const struct tm *tm, char *buf, size_t cap)
{
    if (purr_settings_kv_int("time.clock24", 1)) {
        snprintf(buf, cap, "%02d:%02d", tm->tm_hour, tm->tm_min);
    } else {
        int h = tm->tm_hour % 12;
        snprintf(buf, cap, "%d:%02d %s", h == 0 ? 12 : h, tm->tm_min, tm->tm_hour < 12 ? "AM" : "PM");
    }
}

/* The time and the date as two strings; "--:--" and a note while the clock has not been set. */
static void clock_strings(char *tb, size_t tcap, char *db, size_t dcap)
{
    time_t now = time(NULL);
    if (now > 1000000000) {
        struct tm tm;
        localtime_r(&now, &tm);
        fmt_hm(&tm, tb, tcap);
        size_t n = strftime(db, dcap, "%A, %B ", &tm);
        snprintf(db + n, dcap - n, "%d", tm.tm_mday);
    } else {
        snprintf(tb, tcap, "--:--");
        snprintf(db, dcap, "Clock not set");
    }
}

/* ---------------------------------------------------------------- the lock screen */

static lv_obj_t *s_lock, *s_lock_time, *s_lock_date, *s_lock_s1, *s_lock_s2, *s_lock_hint;
#define LOCK_NOTES 3
static lv_obj_t *s_lock_notes;
static lv_obj_t *s_lk_track, *s_lk_end, *s_lk_end_glyph, *s_lk_knob, *s_lk_chev[3];
static int s_lock_on;

/* The two status lines: the LoRa radio's last-heard bars (nothing at all without a radio), and the battery as Froyo wrote it. */
static void lock_status_refresh(void)
{
    char buf[48];
    int bars = purr_radio_bars();
    if (bars < 0) {
        buf[0] = '\0';
    } else if (bars == 0) {
        snprintf(buf, sizeof(buf), "LoRa mesh: nothing heard yet");
    } else {
        snprintf(buf, sizeof(buf), "LoRa mesh: %d of 4 bars", bars);
    }
    lv_label_set_text(s_lock_s1, buf);

    int pct = 0;
    int have = purr_battery_get(&pct, NULL);
    if (have && purr_battery_charging()) {
        snprintf(buf, sizeof(buf), pct >= 100 ? "Charged" : "Charging (%d%%)", pct);
        lv_obj_set_style_text_color(s_lock_s2, lv_color_hex(0xB6F08A), 0);
    } else if (have && pct <= 20) {
        snprintf(buf, sizeof(buf), "Low battery (%d%%)", pct);
        lv_obj_set_style_text_color(s_lock_s2, lv_color_hex(0xFFB0A0), 0);
    } else {
        buf[0] = '\0';
    }
    lv_label_set_text(s_lock_s2, buf);
}

static void lock_clock_refresh(void)
{
    if (s_lock == NULL) {
        return;
    }
    char tb[16], db[32];
    clock_strings(tb, sizeof(tb), db, sizeof(db));
    lv_label_set_text(s_lock_time, tb);
    lv_label_set_text(s_lock_date, db);
    lock_status_refresh();
}

/* The sign-in screen shows the same clock; it is refreshed by the same timer. */
static void si_clock_refresh(void);

static void lock_tick(lv_timer_t *t)
{
    (void)t;
    if (s_lock_on) {
        lock_clock_refresh();
    }
    si_clock_refresh();
}

int and_lock_is_on(void) { return s_lock_on; }

void and_lock_unlock(void)
{
    if (s_lock == NULL || !s_lock_on) {
        return;
    }
    s_lock_on = 0;
    lv_obj_add_flag(s_lock, LV_OBJ_FLAG_HIDDEN);
    and_keys_visible(1);                                  /* the key strip comes back with the home screen */
}

/* ---- the unlock slider of the draft: a pill, an open padlock at the far end, three chevrons, and a padlock knob dragged to the end. It goes green while it
 * is held, the open padlock lights up near the end, and letting go past 85 percent of the travel unlocks. Vertical when the screen is wide. */

static int s_lk_horiz, s_lk_p0, s_lk_max, s_lk_pos;

/* A padlock drawn from two plain shapes: the body, and a half-circle shackle that is cut short when it is open. */
static lv_obj_t *lock_glyph(lv_obj_t *parent, int open, uint32_t color)
{
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, 20, 20);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *body = lv_obj_create(g);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 13, 9);
    lv_obj_set_style_radius(body, 2, 0);
    lv_obj_set_style_border_width(body, 2, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(color), 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(body, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_t *sh = lv_arc_create(g);
    lv_obj_remove_style(sh, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(sh, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(sh, 9, 9);
    lv_arc_set_bg_angles(sh, 180, open ? 320 : 360);
    lv_obj_set_style_arc_width(sh, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_color(sh, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(sh, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_align(sh, LV_ALIGN_TOP_MID, open ? 2 : 0, open ? 1 : 3);
    return g;
}

static void lk_set_pos(int pos)
{
    if (pos < 0) {
        pos = 0;
    } else if (pos > s_lk_max) {
        pos = s_lk_max;
    }
    s_lk_pos = pos;
    if (s_lk_horiz) {
        lv_obj_set_style_translate_x(s_lk_knob, pos, 0);
    } else {
        lv_obj_set_style_translate_y(s_lk_knob, -pos, 0);
    }
    int armed = s_lk_max > 0 && pos * 100 > s_lk_max * 85;
    lv_obj_set_style_bg_color(s_lk_end, armed ? lv_color_hex(0x5FAE2E) : lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_lk_end, armed ? LV_OPA_COVER : LV_OPA_30, 0);
    lv_obj_set_style_border_color(s_lk_end, armed ? lv_color_hex(0xBDF08A) : lv_color_white(), 0);
    lv_obj_set_style_border_opa(s_lk_end, armed ? LV_OPA_COVER : LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(s_lk_end, armed ? 10 : 0, 0);
}

static void lk_point(lv_point_t *p)
{
    lv_indev_t *in = lv_indev_active();
    p->x = p->y = 0;
    if (in != NULL) {
        lv_indev_get_point(in, p);
    }
}

static void lk_knob_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t p;
    if (code == LV_EVENT_PRESSED) {
        lk_point(&p);
        s_lk_p0 = s_lk_horiz ? p.x : p.y;
    } else if (code == LV_EVENT_PRESSING) {
        lk_point(&p);
        lk_set_pos(s_lk_horiz ? p.x - s_lk_p0 : s_lk_p0 - p.y);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        int done = s_lk_max > 0 && s_lk_pos * 100 > s_lk_max * 85;
        lk_set_pos(0);                                   /* the knob goes back either way */
        if (done) {
            and_lock_unlock();
        }
    }
}

static void lk_reset(void)
{
    if (s_lk_knob != NULL) {
        lk_set_pos(0);
    }
}

static void lk_build(void)
{
    s_lk_track = lv_obj_create(s_lock);
    lv_obj_remove_style_all(s_lk_track);
    lv_obj_set_style_radius(s_lk_track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_lk_track, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_lk_track, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_lk_track, 1, 0);
    lv_obj_set_style_border_color(s_lk_track, lv_color_white(), 0);
    lv_obj_set_style_border_opa(s_lk_track, LV_OPA_20, 0);
    lv_obj_remove_flag(s_lk_track, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_lk_end = lv_obj_create(s_lk_track);                /* where it unlocks: an open padlock, lit when the knob is near */
    lv_obj_remove_style_all(s_lk_end);
    lv_obj_set_size(s_lk_end, 32, 32);
    lv_obj_set_style_radius(s_lk_end, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_lk_end, 1, 0);
    lv_obj_set_style_shadow_color(s_lk_end, lv_color_hex(0x82DC3C), 0);
    lv_obj_set_style_shadow_spread(s_lk_end, 1, 0);
    lv_obj_remove_flag(s_lk_end, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_lk_end_glyph = lock_glyph(s_lk_end, 1, 0x8FBF6A);
    lv_obj_center(s_lk_end_glyph);

    for (int i = 0; i < 3; i++) {
        s_lk_chev[i] = lv_label_create(s_lk_track);
        lv_label_set_text(s_lk_chev[i], LV_SYMBOL_UP);
        lv_obj_set_style_text_font(s_lk_chev[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(s_lk_chev[i], lv_color_white(), 0);
        lv_obj_set_style_text_opa(s_lk_chev[i], LV_OPA_40, 0);
    }

    s_lk_knob = lv_obj_create(s_lk_track);               /* the grey padlock knob, green while it is held */
    lv_obj_remove_style_all(s_lk_knob);
    lv_obj_set_size(s_lk_knob, 34, 34);
    lv_obj_set_style_radius(s_lk_knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_lk_knob, lv_color_hex(0x7B7B7B), 0);
    lv_obj_set_style_bg_grad_color(s_lk_knob, lv_color_hex(0x2C2C2C), 0);
    lv_obj_set_style_bg_grad_dir(s_lk_knob, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_lk_knob, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_lk_knob, 1, 0);
    lv_obj_set_style_border_color(s_lk_knob, lv_color_black(), 0);
    lv_obj_set_style_bg_color(s_lk_knob, lv_color_hex(0x8BD24F), LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_color(s_lk_knob, lv_color_hex(0x3F8A1A), LV_STATE_PRESSED);
    lv_obj_remove_flag(s_lk_knob, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_lk_knob, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *g = lock_glyph(s_lk_knob, 0, 0xFFFFFF);
    lv_obj_center(g);
    lv_obj_add_event_cb(s_lk_knob, lk_knob_cb, LV_EVENT_ALL, NULL);
}

/* Where the pieces go: along the right edge when the screen is wide (the knob starts at the bottom and goes up), along the bottom when it is tall. */
static void lk_layout(int w, int h, int top, int landscape)
{
    s_lk_horiz = !landscape;
    int track_w = landscape ? 42 : w * 2 / 3, track_h = landscape ? h - top - 12 : 42;
    lv_obj_set_size(s_lk_track, track_w, track_h);
    if (landscape) {
        lv_obj_align(s_lk_track, LV_ALIGN_TOP_RIGHT, -12, top);
        lv_obj_align(s_lk_end, LV_ALIGN_TOP_MID, 0, 4);
        lv_obj_align(s_lk_knob, LV_ALIGN_BOTTOM_MID, 0, -3);
        s_lk_max = track_h - 34 - 8;
    } else {
        lv_obj_align(s_lk_track, LV_ALIGN_BOTTOM_MID, 0, -40);
        lv_obj_align(s_lk_end, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_align(s_lk_knob, LV_ALIGN_LEFT_MID, 3, 0);
        s_lk_max = track_w - 34 - 8;
    }
    for (int i = 0; i < 3; i++) {                        /* the three arrows between the knob and the open padlock */
        lv_label_set_text(s_lk_chev[i], landscape ? LV_SYMBOL_UP : LV_SYMBOL_RIGHT);
        if (landscape) {
            lv_obj_align(s_lk_chev[i], LV_ALIGN_CENTER, 0, (i - 1) * 16);
        } else {
            lv_obj_align(s_lk_chev[i], LV_ALIGN_CENTER, (i - 1) * 16, 0);
        }
    }
    lk_set_pos(0);
}

/* The newest notifications under the clock, as rows in the same look as the shade's (a light grey bar, the small app icon, a title and a line): three at most, the last says how
 * many more there are. Shown only while the screen is locked. */
void and_lock_notes_refresh(void)
{
    if (s_lock == NULL || s_lock_notes == NULL) {
        return;
    }
    lv_obj_clean(s_lock_notes);
    int n = and_shade_count();
    for (int i = 0; i < LOCK_NOTES && i < n; i++) {
        char t[40], x[64];
        if (and_shade_peek(i, t, sizeof(t), x, sizeof(x)) != 0) {
            break;
        }
        if (i == LOCK_NOTES - 1 && n > LOCK_NOTES) {
            snprintf(x, sizeof(x), "+%d more in the shade", n - LOCK_NOTES + 1);
        }
        and_note_row(s_lock_notes, t, x);
    }
    if (n > 0) {
        lv_obj_remove_flag(s_lock_notes, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_lock_notes, LV_OBJ_FLAG_HIDDEN);
    }
}

void and_lock_show(void)
{
    if (s_lock == NULL) {
        return;
    }
    and_shade_close();
    w10m_tasks_close();
    and_home_reset();
    s_lock_on = 1;
    and_keys_visible(0);                                  /* the lock screen has no key strip: the whole screen is the picture, and the slider unlocks */
    lock_clock_refresh();
    lk_reset();
    and_lock_notes_refresh();
    lv_obj_remove_flag(s_lock, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *lock_label(const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(s_lock);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

void and_lock_build(void)
{
    s_lock = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_lock);
    backdrop(s_lock);
    lv_obj_clear_flag(s_lock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_lock, LV_OBJ_FLAG_CLICKABLE);       /* it swallows touches meant for what is beneath */
    lv_obj_add_flag(s_lock, LV_OBJ_FLAG_HIDDEN);

    s_lock_time = lock_label(&lv_font_montserrat_40, 0xFFFFFF);
    s_lock_date = lock_label(&lv_font_montserrat_14, 0xE3F0F2);
    s_lock_s1 = lock_label(&lv_font_montserrat_12, 0xD4E4E6);
    s_lock_s2 = lock_label(&lv_font_montserrat_12, 0xD4E4E6);
    s_lock_hint = lock_label(&lv_font_montserrat_12, 0xE3F0F2);
    lv_label_set_text(s_lock_hint, LV_SYMBOL_LIST "  Slide to unlock");
    s_lock_notes = lv_obj_create(s_lock);                 /* the notification rows (and_lock_notes_refresh) */
    lv_obj_remove_style_all(s_lock_notes);
    lv_obj_set_size(s_lock_notes, LV_PCT(60), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_lock_notes, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_lock_notes, 2, 0);
    lv_obj_clear_flag(s_lock_notes, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_lock_notes, LV_OBJ_FLAG_HIDDEN);

    lk_build();

    lv_timer_create(lock_tick, 1000, NULL);
    and_lock_layout();
}

void and_lock_layout(void)
{
    if (s_lock == NULL) {
        return;
    }
    int w = w10m_disp_w(), h = w10m_disp_h();
    int top = AND_STATUS_H + 6, bottom = 0;             /* no key strip beneath the lock screen (and_keys_visible) */
    lv_obj_set_size(s_lock, w, h);
    lv_obj_set_pos(s_lock, 0, 0);
    if (w >= h) {                                        /* landscape: the clock at the left, the slider up the right edge */
        lv_obj_align(s_lock_time, LV_ALIGN_TOP_LEFT, 14, top + 4);
        lv_obj_align_to(s_lock_date, s_lock_time, LV_ALIGN_OUT_BOTTOM_LEFT, 2, 0);
        lv_obj_align_to(s_lock_s1, s_lock_date, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
        lv_obj_align_to(s_lock_s2, s_lock_s1, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);
        lv_obj_set_width(s_lock_notes, w * 64 / 100);
        lv_obj_align_to(s_lock_notes, s_lock_s2, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
        lk_layout(w, h, top, 1);
        lv_obj_align(s_lock_hint, LV_ALIGN_BOTTOM_LEFT, 14, -(bottom + 8));
    } else {                                             /* portrait: centred, with the slider along the bottom */
        lv_obj_align(s_lock_time, LV_ALIGN_TOP_MID, 0, h / 7);
        lv_obj_align_to(s_lock_date, s_lock_time, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
        lv_obj_align_to(s_lock_s1, s_lock_date, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
        lv_obj_align_to(s_lock_s2, s_lock_s1, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
        lv_obj_set_width(s_lock_notes, w - 24);
        lv_obj_align_to(s_lock_notes, s_lock_s2, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
        lk_layout(w, h, top, 0);
        lv_obj_align(s_lock_hint, LV_ALIGN_BOTTOM_MID, 0, -(bottom + 8));
    }
    if (!s_lock_on) {
        lv_obj_add_flag(s_lock, LV_OBJ_FLAG_HIDDEN);
    }
    lock_clock_refresh();
}

/* ---------------------------------------------------------------- the sign-in screen */

/* Two states: the accounts as a list (tap one), then the password for it. Every account on the device is listed. */
enum { SI_LIST = 0, SI_PASSWORD = 1 };

static lv_obj_t *s_si, *s_si_time, *s_si_date, *s_si_card, *s_si_title, *s_si_list, *s_si_who, *s_si_who_av, *s_si_who_name, *s_si_who_role;
static lv_obj_t *s_si_pw, *s_si_pass, *s_si_msg, *s_si_back, *s_si_go;
static lv_group_t *s_si_group;
static int s_si_busy, s_si_state;
static char s_si_name[PURR_USER_NAME_LEN];
static lv_obj_t *s_si_rows[PURR_USER_MAX];
static int s_si_nrows;

typedef struct {
    char name[PURR_USER_NAME_LEN];
    char pass[64];
    purr_login_result_t result;
    uint32_t wait_s;
} signin_job_t;

static void si_clock_refresh(void)
{
    if (s_si == NULL || lv_obj_has_flag(s_si, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    char tb[16], db[32];
    clock_strings(tb, sizeof(tb), db, sizeof(db));
    lv_label_set_text(s_si_time, tb);
    lv_label_set_text(s_si_date, db);
}

static void si_set_busy(int busy)
{
    s_si_busy = busy;
    if (busy) {
        lv_obj_add_state(s_si_pass, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(s_si_pass, LV_STATE_DISABLED);
    }
}

static void si_say(const char *text, uint32_t color)
{
    lv_label_set_text(s_si_msg, text);
    lv_obj_set_style_text_color(s_si_msg, lv_color_hex(color), 0);
}

void and_signin_hidden_check(void)
{
    if (s_si != NULL) {
        lv_obj_add_flag(s_si, LV_OBJ_FLAG_HIDDEN);
    }
    lv_indev_t *kb = purr_lvgl_keypad();
    if (kb != NULL) {
        lv_indev_set_group(kb, NULL);                     /* the keyboard is free for apps */
    }
}

static void si_show_state(int state);

static void *signin_done_cb(void *arg)
{
    signin_job_t *j = arg;
    si_set_busy(0);
    if (j->result == PURR_LOGIN_OK) {
        and_signed_in_set();
        lv_textarea_set_text(s_si_pass, "");
        s_si_state = SI_LIST;
        and_signin_hidden_check();
        ESP_LOGI(TAG, "signed in as %s", j->name);
        purr_theme_load_user(j->name);
        purr_session_started();
        and_home_reload();
        {
            char dn[PURR_USER_NAME_LEN];
            if (purr_login_default_account(dn, sizeof(dn)) && strcmp(j->name, dn) == 0) {
                and_shade_add("Change the default password", "This account uses the password MEOW. Change it in Settings > Accounts.");
            }
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "Signed in as %s.", j->name);
        and_shade_add(PURR_BRAND_PRODUCT, msg);
        char why[400];
        if (purr_resetnote_pending(NULL, 0, why, sizeof(why))) {
            and_shade_add("Your data was reset", why);
        }
    } else if (j->result == PURR_LOGIN_WAIT) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Too many tries. Wait %u s.", (unsigned)j->wait_s);
        si_say(msg, 0xFFB900);
        lv_textarea_set_text(s_si_pass, "");
    } else {
        si_say("That password is wrong. Try again.", 0xFF7B6B);
        lv_textarea_set_text(s_si_pass, "");
        lv_group_focus_obj(s_si_pass);
    }
    memset(j, 0, sizeof(*j));
    free(j);
    return NULL;
}

static void signin_task(void *arg)
{
    signin_job_t *j = arg;
    j->result = purr_login_try(j->name, j->pass, &j->wait_s);
    memset(j->pass, 0, sizeof(j->pass));
    purr_ui_service_run(signin_done_cb, j, NULL);         /* frees j on the UI task */
    vTaskDelete(NULL);
}

static void si_submit(void)
{
    if (s_si_busy || s_si_state != SI_PASSWORD) {
        return;
    }
    signin_job_t *j = calloc(1, sizeof(*j));
    if (j == NULL) {
        si_say("Out of memory.", 0xFFB900);
        return;
    }
    snprintf(j->name, sizeof(j->name), "%s", s_si_name);
    snprintf(j->pass, sizeof(j->pass), "%s", lv_textarea_get_text(s_si_pass));
    si_set_busy(1);
    si_say("Signing in...", 0xFFFFFF);
    if (xTaskCreate(signin_task, "signin", 6144, j, 4, NULL) != pdPASS) {
        si_set_busy(0);
        si_say("Could not start the sign-in.", 0xFFB900);
        free(j);
    }
}

static void si_pass_ready(lv_event_t *e)                  /* Enter in the password field */
{
    (void)e;
    si_submit();
}

static void si_go_clicked(lv_event_t *e)
{
    (void)e;
    si_submit();
}

static void si_back_clicked(lv_event_t *e)
{
    (void)e;
    if (!s_si_busy) {
        si_show_state(SI_LIST);
    }
}

/* Back (Esc): from the password to the list. On the list there is nowhere to go; the lock screen is not behind it. */
void and_signin_back(void)
{
    if (s_si != NULL && !lv_obj_has_flag(s_si, LV_OBJ_FLAG_HIDDEN) && s_si_state == SI_PASSWORD && !s_si_busy) {
        si_show_state(SI_LIST);
    }
}

static const char *role_text(purr_user_role_t r)
{
    return r == PURR_ROLE_USER_ADMIN ? "Administrator" : "Standard user";
}

static lv_obj_t *avatar_make(lv_obj_t *parent, char initial)
{
    lv_obj_t *av = lv_obj_create(parent);
    lv_obj_remove_style_all(av);
    lv_obj_set_style_bg_color(av, lv_color_hex(0x58A0B0), 0);
    lv_obj_set_style_bg_grad_color(av, lv_color_hex(0x1F5F72), 0);
    lv_obj_set_style_bg_grad_dir(av, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(av, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(av, 1, 0);
    lv_obj_set_style_border_color(av, lv_color_hex(0x0C2A33), 0);
    lv_obj_set_style_radius(av, 4, 0);
    lv_obj_set_size(av, 24, 24);
    lv_obj_clear_flag(av, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = lv_label_create(av);
    char s[2] = {initial >= 'a' && initial <= 'z' ? (char)(initial - 32) : initial, 0};
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    return av;
}

static void si_pick(int i)
{
    const purr_user_list_t *ul = purr_login_users();
    if (i < 0 || i >= ul->count) {
        return;
    }
    snprintf(s_si_name, sizeof(s_si_name), "%s", ul->users[i].name);
    lv_obj_delete(s_si_who_av);
    s_si_who_av = avatar_make(s_si_who, ul->users[i].name[0] ? ul->users[i].name[0] : '?');
    lv_obj_align(s_si_who_av, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_text(s_si_who_name, ul->users[i].name);
    lv_label_set_text(s_si_who_role, role_text(ul->users[i].role));
    si_show_state(SI_PASSWORD);
}

static void row_clicked(lv_event_t *e)
{
    si_pick((int)(intptr_t)lv_event_get_user_data(e));
}

/* Lists the accounts. Rebuilt each time the list is shown: accounts can be added while the screen is up (a setup that just made one). */
static void si_build_rows(void)
{
    lv_obj_clean(s_si_list);
    s_si_nrows = 0;
    const purr_user_list_t *ul = purr_login_users();
    for (int i = 0; i < ul->count && i < PURR_USER_MAX; i++) {
        lv_obj_t *row = lv_obj_create(s_si_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), ROW_H);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x3A3A3A), 0);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(ORANGE), LV_STATE_FOCUSED);        /* the trackball's choice, and the finger's */
        lv_obj_set_style_bg_color(row, lv_color_hex(ORANGE), LV_STATE_PRESSED);
        lv_obj_t *av = avatar_make(row, ul->users[i].name[0] ? ul->users[i].name[0] : '?');
        lv_obj_align(av, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, ul->users[i].name);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(nm, lv_color_white(), 0);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 32, -7);
        lv_obj_t *rl = lv_label_create(row);
        lv_label_set_text(rl, role_text(ul->users[i].role));
        lv_obj_set_style_text_font(rl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(rl, lv_color_hex(0xAAAAAA), 0);
        lv_obj_align(rl, LV_ALIGN_LEFT_MID, 32, 8);
        lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_si_rows[s_si_nrows++] = row;
    }
}

static void si_show_state(int state)
{
    s_si_state = state;
    si_say("", 0xFFFFFF);
    lv_group_remove_all_objs(s_si_group);
    if (state == SI_LIST) {
        char dn[PURR_USER_NAME_LEN], reset[96];
        if (purr_login_store_unreadable()) {
            si_say("Can't read the account list. Your files are safe. Restart to try again.", 0xFFB900);
        } else if (purr_resetnote_pending(reset, sizeof(reset), NULL, 0)) {
            si_say(reset, 0xFFB900);                     /* why data was wiped or replaced, until it is dismissed in Settings > About */
        } else if (purr_login_default_account(dn, sizeof(dn))) {
            si_say("Default account: MEOW, password MEOW. Change it in Settings.", 0xFFB900);
        }
        si_build_rows();
        lv_obj_remove_flag(s_si_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_si_who, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_si_pw, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_si_title, "Sign in");
        for (int i = 0; i < s_si_nrows; i++) {
            lv_group_add_obj(s_si_group, s_si_rows[i]);
        }
        if (s_si_nrows > 0) {
            lv_group_focus_obj(s_si_rows[0]);
        }
    } else {
        lv_obj_add_flag(s_si_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_si_who, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_si_pw, LV_OBJ_FLAG_HIDDEN);
        lv_textarea_set_text(s_si_pass, "");
        lv_group_add_obj(s_si_group, s_si_pass);
        lv_group_focus_obj(s_si_pass);
    }
    and_signin_layout();
}

void and_signin_build(void)
{
    s_si = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_si);
    backdrop(s_si);
    lv_obj_clear_flag(s_si, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_si, LV_OBJ_FLAG_CLICKABLE);         /* nothing beneath it can be touched */

    s_si_time = lv_label_create(s_si);
    lv_obj_set_style_text_font(s_si_time, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_si_time, lv_color_white(), 0);
    s_si_date = lv_label_create(s_si);
    lv_obj_set_style_text_font(s_si_date, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_si_date, lv_color_hex(0xCFE3E6), 0);

    /* the card: a grey title bar, then either the accounts or the chosen account with its password */
    s_si_card = lv_obj_create(s_si);
    lv_obj_remove_style_all(s_si_card);
    lv_obj_clear_flag(s_si_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_si_card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_si_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_si_card, 1, 0);
    lv_obj_set_style_border_color(s_si_card, lv_color_hex(0x555555), 0);
    lv_obj_set_style_radius(s_si_card, 5, 0);
    lv_obj_set_style_clip_corner(s_si_card, true, 0);

    lv_obj_t *tb = lv_obj_create(s_si_card);
    lv_obj_remove_style_all(tb);
    purr_theme_apply(tb, PURR_TH_TITLE_BAR);
    lv_obj_set_size(tb, LV_PCT(100), CARD_TITLE_H);
    lv_obj_align(tb, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(tb, LV_OBJ_FLAG_SCROLLABLE);
    s_si_title = lv_label_create(tb);
    lv_obj_set_style_text_font(s_si_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_si_title, lv_color_white(), 0);
    lv_obj_align(s_si_title, LV_ALIGN_LEFT_MID, 8, 0);

    s_si_list = lv_obj_create(s_si_card);
    lv_obj_remove_style_all(s_si_list);
    lv_obj_set_flex_flow(s_si_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_si_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_si_list, LV_DIR_VER);

    s_si_who = lv_obj_create(s_si_card);                  /* the chosen account, above the password */
    lv_obj_remove_style_all(s_si_who);
    lv_obj_set_size(s_si_who, LV_PCT(100), ROW_H);
    lv_obj_clear_flag(s_si_who, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_hor(s_si_who, 8, 0);
    lv_obj_set_style_border_width(s_si_who, 1, 0);
    lv_obj_set_style_border_side(s_si_who, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(s_si_who, lv_color_hex(0x3A3A3A), 0);
    s_si_who_av = avatar_make(s_si_who, '?');
    lv_obj_align(s_si_who_av, LV_ALIGN_LEFT_MID, 0, 0);
    s_si_who_name = lv_label_create(s_si_who);
    lv_obj_set_style_text_font(s_si_who_name, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_si_who_name, lv_color_white(), 0);
    lv_obj_align(s_si_who_name, LV_ALIGN_LEFT_MID, 32, -7);
    s_si_who_role = lv_label_create(s_si_who);
    lv_obj_set_style_text_font(s_si_who_role, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_si_who_role, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(s_si_who_role, LV_ALIGN_LEFT_MID, 32, 8);

    s_si_pw = lv_obj_create(s_si_card);                   /* the password, the message under it, and Back / Sign in */
    lv_obj_remove_style_all(s_si_pw);
    lv_obj_clear_flag(s_si_pw, LV_OBJ_FLAG_SCROLLABLE);
    s_si_group = lv_group_create();
    s_si_pass = w10m_input(s_si_pw, s_si_group, "Password", 1);
    lv_textarea_set_max_length(s_si_pass, 63);
    lv_obj_add_event_cb(s_si_pass, si_pass_ready, LV_EVENT_READY, NULL);
    s_si_msg = lv_label_create(s_si_pw);
    lv_label_set_text(s_si_msg, "");
    lv_obj_set_style_text_font(s_si_msg, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(s_si_msg, LV_LABEL_LONG_WRAP);
    s_si_back = w10m_button(s_si_pw, "Back", si_back_clicked, NULL);
    s_si_go = w10m_button(s_si_pw, "Sign in", si_go_clicked, NULL);

    lv_indev_t *kb = purr_lvgl_keypad();
    if (kb != NULL) {
        lv_indev_set_group(kb, s_si_group);
    }
    si_show_state(SI_LIST);                               /* the clock is refreshed by the lock screen's own timer */
}

void and_signin_layout(void)
{
    if (s_si == NULL) {
        return;
    }
    int w = w10m_disp_w(), h = w10m_disp_h();
    int cw = w > 260 ? w - 28 : w - 16;
    int top = h > w ? 60 : 8;
    lv_obj_set_size(s_si, w, h);
    lv_obj_set_pos(s_si, 0, 0);
    lv_obj_align(s_si_time, LV_ALIGN_TOP_MID, 0, top);
    lv_obj_align_to(s_si_date, s_si_time, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    int ctop = top + 52;
    int body_h;
    if (s_si_state == SI_LIST) {
        int rows = s_si_nrows > 4 ? 4 : s_si_nrows;
        body_h = (rows > 0 ? rows : 1) * ROW_H;
        lv_obj_set_size(s_si_list, LV_PCT(100), body_h);
        lv_obj_align(s_si_list, LV_ALIGN_TOP_LEFT, 0, CARD_TITLE_H);
    } else {
        int pw_h = 4 + 28 + 2 + 15 + 26 + 6;               /* the field, the message line, the two buttons */
        int fw = cw - 18;
        lv_obj_align(s_si_who, LV_ALIGN_TOP_LEFT, 0, CARD_TITLE_H);
        lv_obj_set_size(s_si_pw, LV_PCT(100), pw_h);
        lv_obj_align(s_si_pw, LV_ALIGN_TOP_LEFT, 0, CARD_TITLE_H + ROW_H);
        lv_obj_set_size(s_si_pass, fw, 28);
        lv_obj_align(s_si_pass, LV_ALIGN_TOP_MID, 0, 4);
        lv_obj_set_width(s_si_msg, fw);
        lv_obj_align(s_si_msg, LV_ALIGN_TOP_MID, 0, 4 + 28 + 2);
        int bw = (cw - 18) / 2;
        lv_obj_set_size(s_si_back, bw, 26);
        lv_obj_align(s_si_back, LV_ALIGN_BOTTOM_LEFT, 6, -4);
        lv_obj_set_size(s_si_go, bw, 26);
        lv_obj_align(s_si_go, LV_ALIGN_BOTTOM_RIGHT, -6, -4);
        body_h = ROW_H + pw_h;
    }
    lv_obj_set_size(s_si_card, cw, CARD_TITLE_H + body_h + 2);
    lv_obj_align(s_si_card, LV_ALIGN_TOP_MID, 0, ctop);
    si_clock_refresh();
}
