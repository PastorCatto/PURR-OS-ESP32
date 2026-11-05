/*
 * purr_android.c - an Android 2.2 (Froyo) style shell, the core (purr_android.h, plan7.md "Android shell build-out").
 *
 * The same job as purr_w10m.c and the same contract (purr_w10m_priv.h): the shared Settings, Developer Portal, task manager, permission
 * prompt and the windows of the `ui` catcall call the w10m_* functions below and draw themselves with the theme roles, so they run in this
 * shell with no change of their own. This file builds the shell, the status strip with its Froyo icons, the key strip (Menu, Home, Back),
 * and keeps the banner and busy states, brightness and orientation. The rest of the shell is in purr_android_home.c (home, dock, drawer,
 * options menu), purr_android_shade.c (the notification shade) and purr_android_lock.c (lock and sign-in).
 *
 * Everything here runs on the UI service task, the only place LVGL may be called from. The entry points other tasks use hop over with
 * purr_ui_service_run().
 *
 * Keys: Menu, Home and Back are three touch keys under the screen. Home is also a hold of the trackball's button (0.6 s; a setting
 * later), Back is also Esc. Menu opens the options menu on the home screen and unlocks a locked screen, as Froyo's Menu key did.
 */
#include "purr_brand.h"
#include "esp_attr.h"
#include "purr_android.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"

#include "commands.h"
#include "login.h"
#include "purr_android_priv.h"
#include "purr_appmgr.h"
#include "purr_battery.h"
#include "purr_radio_bars.h"
#include "purr_catrt.h"
#include "purr_catrt_game.h"
#include "purr_catrt_ui.h"
#include "purr_console.h"
#include "purr_kernel.h"
#include "purr_lvgl.h"
#include "purr_power_plat.h"
#include "purr_settings.h"
#include "purr_statusicons.h"
#include "purr_ui_assets.h"
#include "purr_ui_config.h"
#include "purr_ui_service.h"
#include "purr_w10m_theme.h"
#include "purr_wallpaper.h"

static const char *TAG = "android";

#define HOLD_HOME_MS 600                /* a trackball hold that means Home */
#define BANNER_MS    5000

/* ---------------------------------------------------------------- assets as LVGL images */

typedef struct {
    char name[24];
    lv_image_dsc_t dsc;
    int ok;
} img_slot_t;

static EXT_RAM_BSS_ATTR img_slot_t s_imgs[96];
static int s_nimgs;

/* The icon pack (UI/android-assets), as packed: RGB565 with an alpha plane. The pixels stay in the mapped flash partition; only this small
 * descriptor is built. NULL if the asset is missing, in which case callers draw without it. */
const lv_image_dsc_t *w10m_img(const char *name)
{
    for (int i = 0; i < s_nimgs; i++) {
        if (strcmp(s_imgs[i].name, name) == 0) {
            return s_imgs[i].ok ? &s_imgs[i].dsc : NULL;
        }
    }
    if (s_nimgs >= (int)(sizeof(s_imgs) / sizeof(s_imgs[0]))) {
        return NULL;
    }
    img_slot_t *s = &s_imgs[s_nimgs++];
    snprintf(s->name, sizeof(s->name), "%s", name);
    const purr_asset_t *a = purr_ui_assets_find(name);
    if (a == NULL) {
        s->ok = 0;
        return NULL;
    }
    int alpha = a->color_format == PURR_ASSET_RGB565A8;
    s->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s->dsc.header.cf = alpha ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565;
    s->dsc.header.flags = 0;
    s->dsc.header.w = a->width;
    s->dsc.header.h = a->height;
    s->dsc.header.stride = (uint32_t)a->width * 2;
    s->dsc.data_size = (uint32_t)a->width * a->height * (alpha ? 3 : 2);
    s->dsc.data = (const uint8_t *)a->pixels;
    s->ok = 1;
    return &s->dsc;
}

static int disp_w(void) { return (int)lv_display_get_horizontal_resolution(NULL); }
static int disp_h(void) { return (int)lv_display_get_vertical_resolution(NULL); }

int w10m_disp_w(void) { return disp_w(); }
int w10m_disp_h(void) { return disp_h(); }

/* ---------------------------------------------------------------- shell state */

static lv_obj_t *s_wall;                /* the wallpaper gradient lives in purr_android_home.c; this is the screen's own background */
static lv_obj_t *s_strip;               /* the status strip, always on top */
static lv_obj_t *s_clock, *s_wifi, *s_sig, *s_bat, *s_ticker, *s_dev;
static lv_obj_t *s_nicon[4];            /* a small icon for each of the first four notifications */
static lv_obj_t *s_keys;                /* the key strip */
static int s_signed_in;
static int s_brightness = 255;

lv_obj_t *and_strip(void) { return s_strip; }
int and_signed_in(void) { return s_signed_in; }
void and_signed_in_set(void) { s_signed_in = 1; }

/* ---------------------------------------------------------------- the status strip */

/* 24-hour or 12-hour, from the time.clock24 setting (Settings > Date and time). */
static void fmt_hm(const struct tm *tm, char *buf, size_t cap)
{
    if (purr_settings_kv_int("time.clock24", 1)) {
        snprintf(buf, cap, "%02d:%02d", tm->tm_hour, tm->tm_min);
    } else {
        int h = tm->tm_hour % 12;
        snprintf(buf, cap, "%d:%02d %s", h == 0 ? 12 : h, tm->tm_min, tm->tm_hour < 12 ? "AM" : "PM");
    }
}

static void set_icon(lv_obj_t *obj, const char *asset)
{
    const lv_image_dsc_t *d = obj != NULL && asset != NULL ? w10m_img(asset) : NULL;
    if (d != NULL) {
        lv_image_set_src(obj, d);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else if (obj != NULL) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/* The right side, from the clock leftwards: battery, service bars, Wi-Fi. Each hides when it has nothing to say. */
static void status_layout(void)
{
    int x = 40;                                         /* the clock takes the far right */
    lv_obj_t *icons[3] = {s_bat, s_sig, s_wifi};
    for (int i = 0; i < 3; i++) {
        if (icons[i] != NULL && !lv_obj_has_flag(icons[i], LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_align(icons[i], LV_ALIGN_RIGHT_MID, -x, 0);
            x += 19;
        }
    }
    lv_obj_set_width(s_ticker, disp_w() - x - 8);
}

void and_status_notes(int count)
{
    for (int i = 0; i < 4; i++) {
        if (s_nicon[i] == NULL) {
            continue;
        }
        if (i < count) {
            lv_obj_remove_flag(s_nicon[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_nicon[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ---- the ticker: a banner, or work in progress. Froyo's own status bar ticks a new notification's text across the left for a few seconds. */

static int s_work_on;                   /* work in progress (w10m_busy_begin) */
static char s_work_text[64];
static lv_timer_t *s_banner_timer, *s_done_timer;

static void ticker_show(const char *text, int bad)
{
    lv_label_set_text(s_ticker, text);
    lv_obj_set_style_text_color(s_ticker, bad ? lv_color_hex(0xFFB900) : lv_color_white(), 0);
    lv_obj_remove_flag(s_ticker, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 4; i++) {
        if (s_nicon[i] != NULL) {
            lv_obj_add_flag(s_nicon[i], LV_OBJ_FLAG_HIDDEN);        /* the text covers the notification icons while it is up */
        }
    }
}

static void ticker_hide(void)
{
    lv_obj_add_flag(s_ticker, LV_OBJ_FLAG_HIDDEN);
    and_status_notes(and_shade_count());
}

static void banner_end_cb(lv_timer_t *t)
{
    (void)t;
    s_banner_timer = NULL;
    if (s_work_on) {
        ticker_show(s_work_text, 0);                    /* the work is still going: its words come back */
    } else {
        ticker_hide();
    }
}

static void done_cb(lv_timer_t *t)
{
    (void)t;
    s_done_timer = NULL;
    if (s_banner_timer == NULL && !s_work_on) {
        ticker_hide();
    }
}

/* Any status, error or new notification takes over the left of the strip for 5 seconds (an error in amber). A second one replaces the first and
 * restarts the five seconds. Work in progress waits behind it and comes back. */
void w10m_banner(const char *msg, int bad)
{
    if (s_strip == NULL || msg == NULL || msg[0] == '\0') {
        return;
    }
    if (s_done_timer != NULL) {
        lv_timer_delete(s_done_timer);
        s_done_timer = NULL;
    }
    if (s_banner_timer != NULL) {
        lv_timer_delete(s_banner_timer);
    }
    ticker_show(msg, bad);
    s_banner_timer = lv_timer_create(banner_end_cb, BANNER_MS, NULL);
    lv_timer_set_repeat_count(s_banner_timer, 1);
}

typedef struct {
    char msg[96];
    int bad;
} banner_req_t;

static void *banner_ui(void *arg)
{
    banner_req_t *r = arg;
    w10m_banner(r->msg, r->bad);
    return NULL;
}

/* The same from any task (an app's catcall, a worker). */
void w10m_banner_post(const char *msg, int bad)
{
    banner_req_t r = {.bad = bad};
    snprintf(r.msg, sizeof(r.msg), "%s", msg != NULL ? msg : "");
    purr_ui_service_run(banner_ui, &r, NULL);
}

void w10m_busy_begin(const char *msg)
{
    if (s_strip == NULL) {
        return;
    }
    if (s_done_timer != NULL) {
        lv_timer_delete(s_done_timer);
        s_done_timer = NULL;
    }
    snprintf(s_work_text, sizeof(s_work_text), "%s", msg != NULL && msg[0] ? msg : "Working...");
    s_work_on = 1;
    if (s_banner_timer != NULL) {                       /* a banner is up: the work shows when it ends */
        return;
    }
    ticker_show(s_work_text, 0);
}

void w10m_busy_end(void)
{
    if (!s_work_on) {
        return;
    }
    s_work_on = 0;
    if (s_banner_timer != NULL) {                       /* a banner is on the strip: let it finish */
        return;
    }
    ticker_show("Done", 0);
    s_done_timer = lv_timer_create(done_cb, 500, NULL);
    lv_timer_set_repeat_count(s_done_timer, 1);
}

/* ---- DEV badge */

void w10m_dev_badge_update(void)
{
    if (s_dev == NULL) {
        return;
    }
    if (purr_security_unsigned_apps_allowed()) {
        lv_label_set_text(s_dev, "UNSIGNED APPS OK");
        lv_obj_remove_flag(s_dev, LV_OBJ_FLAG_HIDDEN);
    } else if (purr_security_dev_active()) {
        lv_label_set_text(s_dev, "DEV");
        lv_obj_remove_flag(s_dev, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_dev, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- the once-a-second tick: the clock, the battery and the Wi-Fi picture */

static void clock_tick(lv_timer_t *timer)
{
    (void)timer;
    char notice[64];
    if (purr_catrt_take_notice(notice, sizeof(notice))) {      /* the runtime closed an app for room, or refused a service */
        w10m_banner(notice, 0);
    }
    time_t now = time(NULL);
    char buf[12];
    if (now > 1000000000) {
        struct tm tm;
        localtime_r(&now, &tm);
        fmt_hm(&tm, buf, sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), "--:--");            /* the clock has not been set */
    }
    lv_label_set_text(s_clock, buf);

    int pct;
    if (purr_battery_get(&pct, NULL)) {
        if (purr_battery_charging() && purr_si_charge_start(pct) >= 0) {
            /* Froyo's charging animation: six frames from the one for this level; the first stays 2 s, the rest 1 s. The tick is one second,
             * so a frame is held for as many ticks as its time asks for. */
            static int s_cf = -1, s_cstart, s_chold;
            if (s_cf < 0 || s_cstart != purr_si_charge_start(pct)) {
                s_cstart = purr_si_charge_start(pct);
                s_cf = s_cstart;
                s_chold = 0;
            }
            set_icon(s_bat, purr_si_charge_asset(s_cf));
            if (++s_chold * 1000 >= purr_si_charge_ms(s_cf, s_cstart)) {
                s_cf = purr_si_charge_next(s_cf, s_cstart);
                s_chold = 0;
            }
        } else {
            set_icon(s_bat, purr_si_battery_asset(pct));
        }
    } else {
        set_icon(s_bat, purr_si_battery_unknown_asset());
    }

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        set_icon(s_wifi, purr_si_wifi_asset(purr_si_wifi_level(ap.rssi)));
    } else {
        lv_obj_add_flag(s_wifi, LV_OBJ_FLAG_HIDDEN);    /* not connected */
    }
    int bars = purr_radio_bars();                       /* service = the LoRa radio's last-heard signal; no radio, no icon */
    if (bars >= 0) {
        set_icon(s_sig, purr_si_service_asset(bars));
    } else {
        lv_obj_add_flag(s_sig, LV_OBJ_FLAG_HIDDEN);
    }
    status_layout();
}

/* Pull the shade down from the strip: a tap toggles it, a drag down opens it, a drag up closes it. */
static void strip_cb(lv_event_t *e)
{
    static int y0;
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (in == NULL) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (code == LV_EVENT_PRESSED) {
        y0 = p.y;
    } else if (code == LV_EVENT_RELEASED && !and_lock_is_on()) {
        int dy = p.y - y0;
        if (dy > 10 && !and_shade_is_open()) {
            and_shade_toggle();
        } else if (dy < -10 && and_shade_is_open()) {
            and_shade_close();
        } else if (dy >= -10 && dy <= 10) {
            and_shade_toggle();
        }
    }
}

static void build_status(void)
{
    s_strip = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_strip);
    lv_obj_set_size(s_strip, LV_PCT(100), AND_STATUS_H);
    lv_obj_align(s_strip, LV_ALIGN_TOP_MID, 0, 0);
    purr_theme_apply(s_strip, PURR_TH_STRIP);
    lv_obj_clear_flag(s_strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_strip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_strip, strip_cb, LV_EVENT_ALL, NULL);

    for (int i = 0; i < 4; i++) {                       /* notification icons, left to right */
        s_nicon[i] = lv_image_create(s_strip);
        const lv_image_dsc_t *d = w10m_img("fr_s_app");
        if (d != NULL) {
            lv_image_set_src(s_nicon[i], d);
        }
        lv_obj_align(s_nicon[i], LV_ALIGN_LEFT_MID, 3 + i * 18, 0);
        lv_obj_add_flag(s_nicon[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_nicon[i], LV_OBJ_FLAG_CLICKABLE);
    }

    s_ticker = lv_label_create(s_strip);
    lv_obj_set_style_text_font(s_ticker, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(s_ticker, LV_LABEL_LONG_DOT);
    lv_obj_align(s_ticker, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_add_flag(s_ticker, LV_OBJ_FLAG_HIDDEN);

    s_dev = lv_label_create(s_strip);
    lv_obj_set_style_text_font(s_dev, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_dev, lv_color_hex(0xFFB900), 0);
    lv_obj_align(s_dev, LV_ALIGN_CENTER, 0, 0);
    w10m_dev_badge_update();

    s_clock = lv_label_create(s_strip);
    lv_obj_set_style_text_font(s_clock, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_clock, lv_color_white(), 0);
    lv_obj_set_width(s_clock, 38);
    lv_obj_set_style_text_align(s_clock, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_clock, LV_ALIGN_RIGHT_MID, -3, 0);

    s_bat = lv_image_create(s_strip);
    s_sig = lv_image_create(s_strip);
    s_wifi = lv_image_create(s_strip);
    lv_obj_t *icons[3] = {s_bat, s_sig, s_wifi};
    for (int i = 0; i < 3; i++) {
        lv_obj_remove_flag(icons[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(icons[i], LV_OBJ_FLAG_HIDDEN);
    }
    clock_tick(NULL);
    lv_timer_create(clock_tick, 1000, NULL);
}

/* ---------------------------------------------------------------- the key strip: Menu, Home, Back */

typedef enum { KEY_MENU, KEY_HOME, KEY_BACK } and_key_t;

static void key_clicked(lv_event_t *e)
{
    switch ((and_key_t)(intptr_t)lv_event_get_user_data(e)) {
    case KEY_MENU: and_menu_key(); break;
    case KEY_HOME: and_go_home(); break;
    case KEY_BACK: and_back(); break;
    }
}

static void build_keys(void)
{
    s_keys = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_keys);
    lv_obj_set_size(s_keys, LV_PCT(100), AND_KEYS_H);
    lv_obj_align(s_keys, LV_ALIGN_BOTTOM_MID, 0, 0);
    purr_theme_apply(s_keys, PURR_TH_TITLE_BAR);
    lv_obj_set_style_border_side(s_keys, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(s_keys, lv_color_hex(0x4A5256), 0);
    lv_obj_clear_flag(s_keys, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_keys, LV_FLEX_FLOW_ROW);
    static const char *const glyph[3] = {"kb_menu", "kb_home", "kb_back"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_obj_create(s_keys);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, LV_PCT(33), LV_PCT(100));
        lv_obj_set_flex_grow(b, 1);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x3A4A55), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(b, key_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        const lv_image_dsc_t *d = w10m_img(glyph[i]);
        if (d != NULL) {
            lv_obj_t *im = lv_image_create(b);
            lv_image_set_src(im, d);
            lv_obj_set_style_image_recolor(im, lv_color_hex(0xAAB4B9), 0);
            lv_obj_set_style_image_recolor_opa(im, LV_OPA_COVER, 0);
            lv_obj_center(im);
            lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_t *l = lv_label_create(b);                   /* without the pack: words, so the keys still work */
            lv_label_set_text(l, i == KEY_MENU ? "Menu" : i == KEY_HOME ? "Home" : "Back");
            lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(0xAAB4B9), 0);
            lv_obj_center(l);
        }
    }
}

/* ---- what the three keys do */

/* Whether the home screen is what the user is looking at: no sheet, window, shade, menu or drawer over it. */
static int home_is_showing(void)
{
    return !w10m_settings_is_open() && !w10m_devportal_is_open() && !w10m_tasks_is_open() && !and_shade_is_open() &&
           !and_drawer_is_open();
}

void and_go_home(void)
{
    if (and_lock_is_on() || !s_signed_in || w10m_perm_is_open()) {
        return;
    }
    and_shade_close();
    w10m_tasks_close();
    for (int i = 0; i < 4 && w10m_settings_is_open(); i++) {
        w10m_settings_back();                           /* up a page at a time, the last one closes it */
    }
    for (int i = 0; i < 4 && w10m_devportal_is_open(); i++) {
        w10m_devportal_back();
    }
    purr_catrt_home();                                  /* the app in front steps back; it is not closed */
    and_home_reset();
}

void and_back(void)
{
    if (!s_signed_in && !and_lock_is_on()) {
        and_signin_back();                              /* the sign-in screen: from the password back to the accounts */
        return;
    }
    if (and_lock_is_on() || !s_signed_in || w10m_perm_is_open()) {
        return;
    }
    if (and_shade_is_open()) {
        and_shade_close();
    } else if (and_home_back()) {
        /* the options menu or the drawer closed */
    } else if (w10m_tasks_is_open()) {
        w10m_tasks_close();
    } else if (w10m_settings_is_open()) {
        w10m_settings_back();
    } else if (w10m_devportal_is_open()) {
        w10m_devportal_back();
    } else {
        purr_catrt_ui_back();                           /* an app's window goes back or closes; on the home screen there is nothing to do */
    }
}

void and_menu_key(void)
{
    if (and_lock_is_on()) {
        and_lock_unlock();                              /* Froyo's Menu key unlocks */
    } else if (!s_signed_in || w10m_perm_is_open()) {
        return;
    } else if (and_shade_is_open()) {
        and_shade_close();
    } else if (home_is_showing()) {
        and_menu_toggle();
    }
}

static void home_hold(void)
{
    and_go_home();
}

static int key_hook(uint32_t key)
{
    if (key == LV_KEY_ESC && s_signed_in) {
        and_back();
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- brightness and orientation */

void w10m_set_brightness(int level)
{
    level = level < 1 ? 1 : (level > 255 ? 255 : level);
    s_brightness = level;
    const purr_display_v2_t *d = purr_kernel_display();
    if (d != NULL && d->set_brightness != NULL) {
        d->set_brightness((uint8_t)level);
    }
}

int w10m_get_brightness(void) { return s_brightness; }

void w10m_save_display(void)
{
    char v[12];
    snprintf(v, sizeof(v), "%d", s_brightness);
    purr_settings_kv_set("display.brightness", v);
}

static void size_for_orientation(void)
{
    int w = disp_w(), h = disp_h();
    lv_obj_set_size(s_wall, w, h);
    lv_obj_align(s_wall, LV_ALIGN_TOP_LEFT, 0, 0);
}

static void relayout(void)
{
    size_for_orientation();
    and_home_layout();
    and_shade_layout();
    w10m_settings_layout();
    w10m_devportal_layout();
    purr_catrt_ui_layout();
    and_lock_layout();
    and_signin_layout();
    status_layout();
}

void w10m_apply_orientation(int degrees)
{
    w10m_tasks_close();
    if (purr_lvgl_set_rotation(degrees) == 0) {
        relayout();
        ESP_LOGI(TAG, "orientation %d: %dx%d", degrees, disp_w(), disp_h());
        char v[8];
        snprintf(v, sizeof(v), "%d", degrees);
        purr_settings_kv_set("display.rotation", v);
    }
}

void and_rotate_next(void)
{
    w10m_apply_orientation((purr_lvgl_get_rotation() + 90) % 360);
}

static void *orient_cb(void *arg)
{
    int deg = (int)(intptr_t)arg;
    if (purr_lvgl_set_rotation(deg) != 0) {
        return (void *)(intptr_t)-1;
    }
    w10m_apply_orientation(deg);                        /* relayout, and remember it */
    return NULL;
}

esp_err_t purr_android_set_orientation(int degrees)
{
    void *r = NULL;
    esp_err_t e = purr_ui_service_run(orient_cb, (void *)(intptr_t)degrees, &r);
    if (e != ESP_OK) {
        return e;
    }
    return r == NULL ? ESP_OK : ESP_ERR_INVALID_ARG;
}

/* ---------------------------------------------------------------- settings by key (display, shell) */

static int bright_get(char *o, size_t c) { snprintf(o, c, "%d", s_brightness); return 0; }

static int bright_set(const char *v, char *e, size_t c)
{
    int n = atoi(v);
    if (n < 1 || n > 255) {
        snprintf(e, c, "expected 1-255");
        return -1;
    }
    w10m_set_brightness(n);
    w10m_save_display();
    return 0;
}

static int rot_get(char *o, size_t c) { snprintf(o, c, "%d", purr_lvgl_get_rotation()); return 0; }

static int rot_set(const char *v, char *e, size_t c)
{
    int n = atoi(v);
    if (n < 0 || n > 270 || n % 90 != 0) {
        snprintf(e, c, "expected 0, 90, 180 or 270");
        return -1;
    }
    if (purr_android_set_orientation(n) != ESP_OK) {
        snprintf(e, c, "the screen is not running");
        return -1;
    }
    return 0;
}

static int shell_get(char *o, size_t c)
{
    purr_ui_mode_t m = purr_ui_mode_get();
    snprintf(o, c, "%s", m == PURR_UI_MODE_W10M ? "android" : m == PURR_UI_MODE_NAV ? "nav" : "card_shell");
    return 0;
}

static int shell_set(const char *v, char *e, size_t c)
{
    if (strcmp(v, "android") != 0 && strcmp(v, "nav") != 0 && strcmp(v, "card_shell") != 0) {
        snprintf(e, c, "expected android, nav or card_shell");
        return -1;
    }
    purr_fs_t *fs = purr_login_fs();
    char line[24];
    snprintf(line, sizeof(line), "%s\n", v);
    purr_fs_mkdir(fs, "/ui");
    if (purr_fs_write(fs, "/ui/mode.conf", line, (uint32_t)strlen(line)) < 0) {
        snprintf(e, c, "could not save");
        return -1;
    }
    return 0;                                           /* takes effect at the next boot */
}

static const purr_setting_t s_keys_table[] = {
    {"display.brightness", "backlight", "1-255", 0, bright_get, bright_set},
    {"display.rotation", "screen orientation in degrees", "0|90|180|270", 0, rot_get, rot_set},
    {"ui.shell", "which shell starts (next boot)", "android|nav|card_shell", 1, shell_get, shell_set},
};

/* ---------------------------------------------------------------- sheets, the wallpaper, apps */

void w10m_place_sheet(lv_obj_t *sheet)
{
    /* The lock screen, sign-in, shade, status strip and key strip are on the top layer, which is always above the screen. The sheets (Settings, the
     * portal, an app window) are children of the screen, so the newest on top is all that is needed. */
    lv_obj_move_foreground(sheet);
}

int w10m_is_locked(void)
{
    return and_lock_is_on() || !s_signed_in;
}

void w10m_apps_changed(void)
{
    and_home_reload();
}

static void *apps_changed_ui(void *arg)
{
    (void)arg;
    w10m_apps_changed();
    return NULL;
}

void w10m_apps_changed_post(void)
{
    purr_ui_service_run(apps_changed_ui, NULL, NULL);
}

typedef struct {
    char title[32], text[96];
} note_req_t;

static void *note_ui(void *arg)
{
    note_req_t *r = arg;
    and_shade_add(r->title, r->text);
    return NULL;
}

void w10m_note_post(const char *title, const char *text)
{
    note_req_t r;
    snprintf(r.title, sizeof(r.title), "%s", title != NULL ? title : "");
    snprintf(r.text, sizeof(r.text), "%s", text != NULL ? text : "");
    purr_ui_service_run(note_ui, &r, NULL);
}

/* The wallpaper choice is device-wide (purr_wallpaper.h); this shell only draws it. A change comes from whichever task made it. */
static void *wallpaper_apply_cb(void *arg)
{
    (void)arg;
    and_wallpaper_apply();
    return NULL;
}

static void wallpaper_changed(void)
{
    if (strcmp(pcTaskGetName(NULL), "ui_service") == 0) {
        wallpaper_apply_cb(NULL);
    } else {
        purr_ui_service_run(wallpaper_apply_cb, NULL, NULL);
    }
}

/* ---------------------------------------------------------------- start */

static void theme_changed(void)
{
    and_home_reload();                                  /* what was coloured by hand follows the accent */
}

static void *build_cb(void *arg)
{
    (void)arg;
    purr_theme_init();
    int saved_b = purr_settings_kv_int("display.brightness", 255);
    w10m_set_brightness(saved_b < 1 ? 255 : saved_b);
    int saved_r = purr_settings_kv_int("display.rotation", -1);
    if (saved_r >= 0 && saved_r <= 270 && saved_r % 90 == 0) {
        purr_lvgl_set_rotation(saved_r);                /* before anything is laid out */
    }
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_wall = lv_obj_create(scr);                        /* the screen's own backdrop; the home screen draws the wallpaper on it */
    lv_obj_remove_style_all(s_wall);
    lv_obj_set_style_bg_color(s_wall, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_wall, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_wall, LV_OBJ_FLAG_CLICKABLE);

    s_signed_in = purr_login_current()->name[0] != '\0';   /* a session already exists (first-time setup) */

    and_home_build(scr);
    w10m_settings_build();
    w10m_devportal_build();
    and_lock_build();                                   /* beneath the strips, which stay visible over it */
    and_shade_build();
    build_status();
    build_keys();
    and_signin_build();                                 /* last, so it covers everything until someone signs in */
    purr_theme_on_change(theme_changed);
    if (purr_login_autologin_used()) {
        char msg[80];
        snprintf(msg, sizeof(msg), "Signed in automatically as %s (developer mode).", purr_login_current()->name);
        and_shade_add(PURR_BRAND_PRODUCT, msg);
    }
    if (s_signed_in) {
        purr_theme_load_user(purr_login_current()->name);
        and_signin_hidden_check();
    }
    relayout();
    and_wallpaper_apply();
    ESP_LOGI(TAG, "shell up: %dx%d, accent %s", disp_w(), disp_h(), purr_theme_accent_at(purr_theme_accent())->name);
    return NULL;
}

esp_err_t purr_android_start(void)
{
    purr_lvgl_set_scroll_handler(purr_catrt_ui_scroll);              /* the trackball scrolls the app window on screen */
    purr_lvgl_set_key_hook(key_hook);                                /* Esc is Back */
    purr_lvgl_set_trackball_hold(home_hold, HOLD_HOME_MS);           /* holding the trackball's button is Home */
    purr_wallpaper_set_listener(wallpaper_changed);
    purr_wallpaper_register();
    purr_catrt_ui_init();
    purr_catrt_game_init();
    purr_power_sys_set_repaint(purr_ui_service_repaint);              /* the panel sleeps with the screen; wake draws the current picture first */
    w10m_perm_init();
    purr_console_set_muted(1);                                        /* the text console must not paint over the shell */
    for (size_t i = 0; i < sizeof(s_keys_table) / sizeof(s_keys_table[0]); i++) {
        purr_settings_register(&s_keys_table[i]);
    }
    return purr_ui_service_run(build_cb, NULL, NULL);
}

static void *refresh_cb(void *arg)
{
    (void)arg;
    and_home_reload();
    return NULL;
}

esp_err_t purr_android_refresh_apps(void)
{
    return purr_ui_service_run(refresh_cb, NULL, NULL);
}

typedef struct {
    char title[40];
    char text[120];
} note_args_t;

static void *notify_cb(void *arg)
{
    const note_args_t *n = arg;
    and_shade_add(n->title, n->text);
    return NULL;
}

esp_err_t purr_android_notify(const char *title, const char *text)
{
    note_args_t n;
    snprintf(n.title, sizeof(n.title), "%s", title ? title : "");
    snprintf(n.text, sizeof(n.text), "%s", text ? text : "");
    return purr_ui_service_run(notify_cb, &n, NULL);
}

static void *lock_cb(void *arg)
{
    (void)arg;
    if (s_signed_in) {
        and_lock_show();
    }
    return NULL;
}

esp_err_t purr_android_lock(void)
{
    return purr_ui_service_run(lock_cb, NULL, NULL);
}
