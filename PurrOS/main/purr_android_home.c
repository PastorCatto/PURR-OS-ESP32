/* SPDX-License-Identifier: Apache-2.0
 * The Android 2.2 (Froyo) style shell is licensed under the Apache License, Version 2.0, the licence the Android Open Source Project uses, in respect of that project.
 * The licence text is in UI/android-assets/NOTICE; the rest of PURR OS is under the GNU GPL version 3 (LICENSE). See THIRD_PARTY.md. */
/*
 * purr_android_home.c - the home screen of the Android style shell (purr_android.c, plan7.md "Android shell build-out").
 *
 * What is on screen, bottom to top: the wallpaper (a gradient, or a picture when the asset pack has it), pages of app icons that scroll
 * sideways and snap, page dots, and a dock of four apps around the Apps button. The Apps button opens the app drawer: every installed app,
 * in alphabetical order. Menu on the home screen opens the options menu.
 *
 * Which apps are on the home pages is the user's choice, as on Froyo: a long press on an icon (in the drawer or on a page) adds it to the
 * home screen or takes it off. The choice is the file /home/<you>/.config/home.pins (app names, comma separated, in order; the first four are
 * the dock), so each user has their own home screen. Until something is pinned, the first eight installed apps are. Reordering is not built yet.
 *
 * The app list is the same as the Windows style shell's: the shell's own Settings, then every installed app that verifies for the
 * signed-in user (an unsigned one only while developer mode allows it), then the Developer Portal and the developer apps in developer mode.
 *
 * Objects are kept few (UI/SHELL-OPTIMIZATION.md): an icon is three objects, the pages hold only the pinned apps, and the drawer is built
 * when it opens and deleted when it closes.
 */
#include "purr_android.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "commands.h"
#include "login.h"
#include "purr_android_priv.h"
#include "purr_appmgr.h"
#include "purr_catrt.h"
#include "purr_fs.h"
#include "purr_ui_service.h"
#include "purr_w10m_theme.h"
#include "purr_wallpaper.h"

static const char *TAG = "android-home";

#define MAX_APPS   40
#define MAX_PINS   12
#define DOCK_APPS  4                    /* the first four pins are the dock, two each side of the Apps button */
#define CELL_W     80
#define CELL_H     66
#define DOCK_H     56
#define MENU_ROW_H 38

typedef struct {
    char name[PURR_APP_NAME_LEN];
    uint8_t system;                     /* 1 = Settings, 2 = the Developer Portal */
    uint8_t dev;                        /* a developer app: shown with a DEV mark */
} app_t;

static app_t s_apps[MAX_APPS];
static int s_napps;

static char s_pins[MAX_PINS][PURR_APP_NAME_LEN];
static int s_npins;
static int s_pins_set;                  /* the user has chosen; otherwise the defaults stand in */

static lv_obj_t *s_home;                /* everything on the home screen, below the strips */
static lv_obj_t *s_wall_grad, *s_wall_img;
static lv_obj_t *s_pages;               /* the pages, scrolling sideways */
static lv_obj_t *s_dots;
static lv_obj_t *s_dock;
static lv_obj_t *s_drawer;              /* NULL while closed */
static lv_obj_t *s_menu;
static int s_npages = 1;

static int disp_w(void) { return w10m_disp_w(); }
static int area_h(void) { return w10m_disp_h() - AND_STATUS_H - AND_KEYS_H; }
static int pages_h(void) { return area_h() - DOCK_H; }
static int cols(void) { int c = disp_w() / CELL_W; return c < 3 ? 3 : c; }
static int rows(void) { int r = (pages_h() - 14) / CELL_H; return r < 1 ? 1 : r; }

/* ---------------------------------------------------------------- the app list */

/* The shell's Settings, then the installed apps that verify (an unsigned app is never listed, so it can never be started from here either). */
static void load_apps(void)
{
    static purr_app_registry_t reg;                     /* ~1.6 KB: not for the UI task's stack */
    s_napps = 0;
    app_t *d = &s_apps[s_napps++];
    memset(d, 0, sizeof(*d));
    snprintf(d->name, sizeof(d->name), "Settings");
    d->system = 1;

    memset(&reg, 0, sizeof(reg));
    if (purr_login_current()->name[0] != '\0') {
        purr_apps_scan_current_user(&reg);
    }
    for (int i = 0; i < reg.count && s_napps < MAX_APPS; i++) {
        const purr_app_entry_t *a = &reg.apps[i];
        if (!a->chip_ok || !(a->verified || purr_security_unsigned_apps_allowed()) || strcmp(a->name, "Settings") == 0) {
            continue;
        }
        d = &s_apps[s_napps++];
        memset(d, 0, sizeof(*d));
        snprintf(d->name, sizeof(d->name), "%s", a->name);
        d->dev = !a->verified;
    }

    /* Developer mode: the Developer Portal is an app of its own, and what it installed (the devapps folder) is listed with a DEV mark. A developer
     * app wins over an installed one of the same name, as it does when it is opened. */
    if (purr_security_dev_active() && purr_login_current()->name[0] != '\0' && s_napps < MAX_APPS) {
        d = &s_apps[s_napps++];
        memset(d, 0, sizeof(*d));
        snprintf(d->name, sizeof(d->name), "Developer Portal");
        d->system = 2;
        d->dev = 1;
        memset(&reg, 0, sizeof(reg));
        purr_devapps_scan(&reg);
        for (int i = 0; i < reg.count; i++) {
            const purr_app_entry_t *a = &reg.apps[i];
            if (!a->chip_ok || !(a->verified || purr_security_unsigned_apps_allowed())) {
                continue;
            }
            app_t *slot = NULL;
            for (int k = 0; k < s_napps; k++) {
                if (strcmp(s_apps[k].name, a->name) == 0) {
                    slot = &s_apps[k];
                }
            }
            if (slot == NULL && s_napps < MAX_APPS) {
                slot = &s_apps[s_napps++];
            }
            if (slot != NULL) {
                memset(slot, 0, sizeof(*slot));
                snprintf(slot->name, sizeof(slot->name), "%s", a->name);
                slot->dev = 1;
            }
        }
    }
}

static int find_app(const char *name)
{
    for (int i = 0; i < s_napps; i++) {
        if (strcmp(s_apps[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* The picture for an app: fr_<name in lower case letters and digits>, with a few apps sharing another's, or the generic app icon. */
static const char *icon_for(const app_t *a, char *buf, size_t cap)
{
    if (a->system == 1) {
        return "fr_settings";
    }
    if (a->system == 2) {
        return "fr_app";
    }
    static const struct { const char *app, *icon; } alias[] = {
        {"files", "fr_folder"}, {"devices", "fr_settings"}, {"notes", "fr_app"}, {"store", "fr_app"},
        {"photos", "fr_gallery"}, {"gallery", "fr_gallery"}, {"messages", "fr_messaging"}, {"sms", "fr_messaging"},     /* the stock Froyo Gallery and Messaging art (AOSP, Apache 2.0: THIRD_PARTY.md) */
    };
    size_t n = snprintf(buf, cap, "fr_");
    for (const char *s = a->name; *s != '\0' && n + 1 < cap; s++) {
        if (isalnum((unsigned char)*s)) {
            buf[n++] = (char)tolower((unsigned char)*s);
        }
    }
    buf[n] = '\0';
    for (size_t i = 0; i < sizeof(alias) / sizeof(alias[0]); i++) {
        if (strcmp(buf + 3, alias[i].app) == 0) {
            return alias[i].icon;
        }
    }
    return w10m_img(buf) != NULL ? buf : "fr_app";
}

/* ---------------------------------------------------------------- the pins */

static void pins_default(void)
{
    s_npins = 0;
    for (int i = 0; i < s_napps && s_npins < 8; i++) {
        snprintf(s_pins[s_npins++], PURR_APP_NAME_LEN, "%s", s_apps[i].name);
    }
}

typedef struct {
    char *buf;
    uint32_t cap, len;
} rd_t;

static int rd_cb(void *ctx, const void *data, uint32_t len)
{
    rd_t *r = ctx;
    if (r->len + len > r->cap) {
        len = r->cap - r->len;
    }
    memcpy(r->buf + r->len, data, len);
    r->len += len;
    return 0;
}

/* Not the shared settings file: that one holds 768 bytes for every setting on the device, and a list of apps would not fit beside them. */
static int pins_path(char *out, size_t cap)
{
    const char *user = purr_login_current()->name;
    if (user[0] == '\0') {
        return -1;
    }
    snprintf(out, cap, "/home/%s/.config/home.pins", user);
    return 0;
}

static void pins_load(void)
{
    char path[96], buf[(MAX_PINS * PURR_APP_NAME_LEN) + MAX_PINS];
    s_npins = 0;
    s_pins_set = 0;
    purr_fs_t *fs = purr_login_fs();
    if (pins_path(path, sizeof(path)) == 0 && fs != NULL && purr_fs_mounted(fs)) {
        rd_t r = {buf, sizeof(buf) - 1, 0};
        if (purr_fs_read(fs, path, rd_cb, &r) >= 0) {
            buf[r.len] = '\0';
            s_pins_set = 1;                                 /* the file exists: the user's choice, even if it is empty */
            for (char *p = strtok(buf, ",\r\n"); p != NULL && s_npins < MAX_PINS; p = strtok(NULL, ",\r\n")) {
                if (p[0] != '\0') {
                    snprintf(s_pins[s_npins++], PURR_APP_NAME_LEN, "%s", p);
                }
            }
        }
    }
    if (!s_pins_set) {
        pins_default();
    }
}

static void pins_save(void)
{
    char path[96], buf[(MAX_PINS * PURR_APP_NAME_LEN) + MAX_PINS];
    buf[0] = '\0';
    for (int i = 0; i < s_npins; i++) {
        size_t n = strlen(buf);
        snprintf(buf + n, sizeof(buf) - n, "%s%s", i ? "," : "", s_pins[i]);
    }
    s_pins_set = 1;
    purr_fs_t *fs = purr_login_fs();
    if (pins_path(path, sizeof(path)) != 0 || fs == NULL || !purr_fs_mounted(fs)) {
        return;
    }
    for (size_t i = 1; path[i] != '\0'; i++) {              /* make /home/<you>/.config; an existing directory is just an ignored error */
        if (path[i] == '/') {
            char dir[96];
            memcpy(dir, path, i);
            dir[i] = '\0';
            purr_fs_mkdir(fs, dir);
        }
    }
    if (purr_fs_write(fs, path, buf, (uint32_t)strlen(buf)) < 0) {
        ESP_LOGW(TAG, "could not save %s", path);
    }
}

static int pin_index(const char *name)
{
    for (int i = 0; i < s_npins; i++) {
        if (strcmp(s_pins[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

/* The apps on the home screen, in pin order: indexes into s_apps (a pinned app that is no longer installed is skipped). */
static int pinned_apps(int *out)
{
    int n = 0;
    for (int i = 0; i < s_npins; i++) {
        int a = find_app(s_pins[i]);
        if (a >= 0) {
            out[n++] = a;
        }
    }
    return n;
}

static void toggle_pin(int app)
{
    const char *name = s_apps[app].name;
    int at = pin_index(name);
    if (at >= 0) {
        for (int i = at; i + 1 < s_npins; i++) {
            memcpy(s_pins[i], s_pins[i + 1], PURR_APP_NAME_LEN);
        }
        s_npins--;
        w10m_banner("Removed from Home", 0);
    } else if (s_npins < MAX_PINS) {
        snprintf(s_pins[s_npins++], PURR_APP_NAME_LEN, "%s", name);
        w10m_banner("Added to Home", 0);
    } else {
        w10m_banner("The home screen is full", 1);
        return;
    }
    pins_save();
}

/* ---------------------------------------------------------------- opening an app */

typedef struct {
    char name[PURR_APP_NAME_LEN];
} open_job_t;

static void open_task(void *arg)
{
    open_job_t *j = arg;
    char line[PURR_APP_NAME_LEN + 8], out[160];
    snprintf(line, sizeof(line), "open %s", j->name);
    out[0] = '\0';
    int rc = purr_catrt_dispatch(line, out, sizeof(out));
    if (rc != 0) {
        char msg[200];
        snprintf(msg, sizeof(msg), "%s", out[0] ? out : "could not start");
        purr_android_notify(j->name, msg);
    }
    free(j);
    vTaskDelete(NULL);
}

static void drawer_close(void);

void and_open_app(const char *name, int system)
{
    drawer_close();
    and_menu_close();
    if (system == 2) {
        w10m_devportal_open();
        return;
    }
    if (system == 1) {
        w10m_settings_open();
        return;
    }
    open_job_t *j = malloc(sizeof(*j));
    if (j == NULL) {
        w10m_banner("Not enough memory to open the app", 0);
        return;
    }
    snprintf(j->name, sizeof(j->name), "%s", name);
    if (xTaskCreate(open_task, "and-open", 4096, j, 4, NULL) != pdPASS) {      /* the task's stack is internal RAM: with none to spare, say so instead of doing nothing */
        free(j);
        w10m_banner("Not enough memory to open the app", 0);
    }
}

/* ---------------------------------------------------------------- icons */

static void icon_clicked(lv_event_t *e)
{
    int a = (int)(intptr_t)lv_event_get_user_data(e);
    if (a >= 0 && a < s_napps) {
        and_open_app(s_apps[a].name, s_apps[a].system);
    }
}

static void fill_home(void);

/* The pages are rebuilt after the press has finished: a long press may be on an icon that the rebuild deletes. The drawer stays open. */
static void pins_changed_cb(void *arg)
{
    (void)arg;
    fill_home();
}

/* Add or remove an icon on the home screen: hold it, in the drawer, on a page or in the dock. */
static void icon_long(lv_event_t *e)
{
    int a = (int)(intptr_t)lv_event_get_user_data(e);
    if (a >= 0 && a < s_napps) {
        int before = s_npins;
        toggle_pin(a);
        if (s_npins != before) {
            lv_async_call(pins_changed_cb, NULL);
        }
    }
}

/* An icon: the picture, and under it the name unless it is in the dock. Three objects (two in the dock). */
static lv_obj_t *make_icon(lv_obj_t *parent, int app, int x, int y, int w, int h, int label)
{
    char buf[40];
    const app_t *a = &s_apps[app];
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(o, 6, 0);
    lv_obj_set_style_bg_color(o, lv_color_darken(purr_theme_accent_color(), LV_OPA_30), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(o, icon_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)app);
    lv_obj_add_event_cb(o, icon_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)app);

    const lv_image_dsc_t *d = w10m_img(icon_for(a, buf, sizeof(buf)));
    if (d != NULL) {
        lv_obj_t *im = lv_image_create(o);
        lv_image_set_src(im, d);
        lv_obj_align(im, label ? LV_ALIGN_TOP_MID : LV_ALIGN_CENTER, 0, label ? 3 : 0);
        lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
    }
    if (label) {
        lv_obj_t *n = lv_label_create(o);
        char text[PURR_APP_NAME_LEN + 6];                   /* the name, with a star in front when it is a developer app */
        snprintf(text, sizeof(text), "%s%s", a->dev ? "* " : "", a->name);
        lv_label_set_text(n, text);
        lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
        lv_obj_set_width(n, w - 2);
        lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(n, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(n, a->dev ? lv_color_hex(0xFFB900) : lv_color_white(), 0);
        lv_obj_align(n, LV_ALIGN_BOTTOM_MID, 0, -1);
    }
    return o;
}

/* ---------------------------------------------------------------- the home pages and the dock */

static void dots_update(void)
{
    int w = disp_w();
    int page = w > 0 ? (lv_obj_get_scroll_x(s_pages) + w / 2) / w : 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(s_dots); i++) {
        lv_obj_set_style_bg_color(lv_obj_get_child(s_dots, (int32_t)i), (int)i == page ? lv_color_white() : lv_color_hex(0x7A8A90), 0);
    }
}

static void pages_scrolled(lv_event_t *e)
{
    (void)e;
    dots_update();
}

static void apps_button_clicked(lv_event_t *e)
{
    (void)e;
    and_drawer_toggle();
}

static void fill_home(void)
{
    int w = disp_w(), ph = pages_h();
    int idx[MAX_PINS];
    int n = pinned_apps(idx);
    lv_obj_clean(s_pages);
    lv_obj_clean(s_dock);
    lv_obj_clean(s_dots);

    int per = cols() * rows();
    int rest = n > DOCK_APPS ? n - DOCK_APPS : 0;
    s_npages = rest > 0 ? (rest + per - 1) / per : 1;
    int x0 = (w - cols() * CELL_W) / 2;
    for (int p = 0; p < s_npages; p++) {
        lv_obj_t *page = lv_obj_create(s_pages);
        lv_obj_remove_style_all(page);
        lv_obj_set_size(page, w, ph);
        lv_obj_set_pos(page, p * w, 0);
        lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(page, LV_OBJ_FLAG_SNAPPABLE);
        for (int i = 0; i < per; i++) {
            int k = DOCK_APPS + p * per + i;
            if (k >= n) {
                break;
            }
            make_icon(page, idx[k], x0 + (i % cols()) * CELL_W, 4 + (i / cols()) * CELL_H, CELL_W, CELL_H, 1);
        }
    }

    /* the dock: two apps, the Apps button, two apps */
    int slot_w = w / 5;
    for (int s = 0; s < 5; s++) {
        int pin = s < 2 ? s : s - 1;                    /* dock slots 0 1 [Apps] 3 4 hold pins 0 1 2 3 */
        if (s == 2) {
            lv_obj_t *b = lv_obj_create(s_dock);
            lv_obj_remove_style_all(b);
            lv_obj_set_size(b, slot_w, DOCK_H - 2);
            lv_obj_set_pos(b, s * slot_w, 1);
            lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_radius(b, 6, 0);
            lv_obj_set_style_bg_color(b, lv_color_darken(purr_theme_accent_color(), LV_OPA_30), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
            lv_obj_add_event_cb(b, apps_button_clicked, LV_EVENT_CLICKED, NULL);
            const lv_image_dsc_t *d = w10m_img("fr_dock_apps");
            if (d != NULL) {
                lv_obj_t *im = lv_image_create(b);
                lv_image_set_src(im, d);
                lv_obj_center(im);
                lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
            } else {
                lv_obj_t *l = lv_label_create(b);
                lv_label_set_text(l, LV_SYMBOL_LIST);
                lv_obj_center(l);
            }
        } else if (pin < n) {
            make_icon(s_dock, idx[pin], s * slot_w, 1, slot_w, DOCK_H - 2, 0);
        }
    }

    /* the dots: only when there is more than one page */
    if (s_npages > 1) {
        for (int p = 0; p < s_npages; p++) {
            lv_obj_t *dot = lv_obj_create(s_dots);
            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, 6, 6);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        }
        dots_update();
    }
    lv_obj_scroll_to_x(s_pages, 0, LV_ANIM_OFF);
}

/* ---------------------------------------------------------------- the app drawer */

static int s_sorted[MAX_APPS];

static void drawer_close(void)
{
    if (s_drawer != NULL) {
        lv_obj_delete(s_drawer);
        s_drawer = NULL;
    }
}

static void drawer_open(void)
{
    if (s_drawer != NULL) {
        return;
    }
    for (int i = 0; i < s_napps; i++) {                 /* alphabetical, case blind */
        int k = i;
        while (k > 0 && strcasecmp(s_apps[s_sorted[k - 1]].name, s_apps[i].name) > 0) {
            s_sorted[k] = s_sorted[k - 1];
            k--;
        }
        s_sorted[k] = i;
    }
    int w = disp_w();
    s_drawer = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_drawer);
    purr_theme_apply(s_drawer, PURR_TH_SHEET);
    lv_obj_set_size(s_drawer, w, area_h());
    lv_obj_set_pos(s_drawer, 0, AND_STATUS_H);
    lv_obj_set_scrollbar_mode(s_drawer, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_drawer, LV_DIR_VER);
    int c = cols(), x0 = (w - c * CELL_W) / 2;
    for (int i = 0; i < s_napps; i++) {
        make_icon(s_drawer, s_sorted[i], x0 + (i % c) * CELL_W, 4 + (i / c) * CELL_H, CELL_W, CELL_H, 1);
    }
    lv_obj_move_foreground(s_drawer);
}

int and_drawer_is_open(void) { return s_drawer != NULL; }

void and_drawer_toggle(void)
{
    if (s_drawer != NULL) {
        drawer_close();
    } else {
        and_menu_close();
        drawer_open();
    }
}

/* ---------------------------------------------------------------- the options menu (Menu on the home screen) */

static const struct {
    const char *text;
    int act;
} k_menu[] = {
    {LV_SYMBOL_IMAGE "\nWallpaper", 0},
    {LV_SYMBOL_BELL "\nNotifications", 1},
    {LV_SYMBOL_LIST "\nTasks", 2},
    {LV_SYMBOL_REFRESH "\nRotate", 3},
    {LV_SYMBOL_EYE_CLOSE "\nLock", 4},
    {LV_SYMBOL_SETTINGS "\nSettings", 5},
};

static void menu_clicked(lv_event_t *e)
{
    int act = (int)(intptr_t)lv_event_get_user_data(e);
    and_menu_close();
    switch (act) {
    case 0: {
        int next = (purr_wallpaper_get() + 1) % (PURR_WALLPAPER_BUILTIN + 1);
        if (purr_wallpaper_set(next) == 0) {
            w10m_banner(purr_wallpaper_name(next), 0);
        }
        break;
    }
    case 1: and_shade_toggle(); break;
    case 2: w10m_tasks_open(); break;
    case 3: and_rotate_next(); break;
    case 4: and_lock_show(); break;
    case 5: and_open_app("Settings", 1); break;
    }
}

static void build_menu(lv_obj_t *screen)
{
    s_menu = lv_obj_create(screen);
    lv_obj_remove_style_all(s_menu);
    lv_obj_set_style_bg_color(s_menu, lv_color_hex(0x0D0D0D), 0);
    lv_obj_set_style_bg_opa(s_menu, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(s_menu, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(s_menu, 2, 0);
    lv_obj_set_style_border_color(s_menu, lv_color_hex(0x5A5A5A), 0);
    lv_obj_clear_flag(s_menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 6; i++) {
        lv_obj_t *b = lv_obj_create(s_menu);
        lv_obj_remove_style_all(b);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x1A1A1A), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x3B4A55), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_black(), 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, menu_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)k_menu[i].act);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, k_menu[i].text);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xDDDDDD), 0);
        lv_obj_center(l);
    }
}

static void menu_layout(void)
{
    int w = disp_w(), h = w10m_disp_h();
    int cw = w / 3;
    lv_obj_set_size(s_menu, w, 2 * MENU_ROW_H + 2);
    lv_obj_set_pos(s_menu, 0, h - AND_KEYS_H - (2 * MENU_ROW_H + 2));
    for (int i = 0; i < 6; i++) {
        lv_obj_t *b = lv_obj_get_child(s_menu, i);
        lv_obj_set_size(b, i % 3 == 2 ? w - 2 * cw : cw, MENU_ROW_H);
        lv_obj_set_pos(b, (i % 3) * cw, 2 + (i / 3) * MENU_ROW_H);
    }
}

int and_menu_is_open(void) { return s_menu != NULL && !lv_obj_has_flag(s_menu, LV_OBJ_FLAG_HIDDEN); }

void and_menu_close(void)
{
    if (s_menu != NULL) {
        lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
    }
}

void and_menu_toggle(void)
{
    if (and_menu_is_open()) {
        and_menu_close();
    } else {
        lv_obj_move_foreground(s_menu);
        lv_obj_remove_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------------------------------------------------------------- wallpaper, layout, entry points */

void and_wallpaper_apply(void)
{
    if (s_wall_img == NULL) {
        return;
    }
    const lv_image_dsc_t *d = w10m_img(purr_wallpaper_asset(PURR_WP_START));
    if (d != NULL) {
        lv_image_set_src(s_wall_img, d);
        lv_image_set_inner_align(s_wall_img, LV_IMAGE_ALIGN_CENTER);     /* cropped to the screen, not scaled */
        lv_obj_remove_flag(s_wall_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_wall_img, LV_OBJ_FLAG_HIDDEN);                 /* the gradient shows */
    }
}

void and_home_layout(void)
{
    if (s_home == NULL) {
        return;
    }
    drawer_close();
    and_menu_close();
    int w = disp_w(), h = w10m_disp_h();
    lv_obj_set_size(s_home, w, h);                      /* the home screen is the full screen so the wallpaper reaches both strips */
    lv_obj_set_pos(s_home, 0, 0);
    lv_obj_set_size(s_wall_grad, w, h);
    lv_obj_set_size(s_wall_img, w, h);
    lv_obj_set_size(s_pages, w, pages_h());
    lv_obj_set_pos(s_pages, 0, AND_STATUS_H);
    lv_obj_set_size(s_dock, w, DOCK_H);
    lv_obj_set_pos(s_dock, 0, h - AND_KEYS_H - DOCK_H);
    lv_obj_set_size(s_dots, w, 8);
    lv_obj_set_pos(s_dots, 0, h - AND_KEYS_H - DOCK_H - 10);
    menu_layout();
    fill_home();
}

void and_home_reload(void)
{
    if (s_home == NULL) {
        return;
    }
    load_apps();
    pins_load();
    drawer_close();
    fill_home();
    ESP_LOGI(TAG, "%d apps, %d pinned", s_napps, s_npins);
}

void and_home_reset(void)
{
    drawer_close();
    and_menu_close();
    lv_obj_scroll_to_x(s_pages, 0, LV_ANIM_OFF);
    dots_update();
}

int and_home_back(void)
{
    if (and_menu_is_open()) {
        and_menu_close();
        return 1;
    }
    if (s_drawer != NULL) {
        drawer_close();
        return 1;
    }
    return 0;
}

void and_home_build(lv_obj_t *screen)
{
    s_home = lv_obj_create(screen);
    lv_obj_remove_style_all(s_home);
    lv_obj_clear_flag(s_home, LV_OBJ_FLAG_SCROLLABLE);

    s_wall_grad = lv_obj_create(s_home);                /* the default wallpaper: a deep teal fading to night, drawn with no picture at all */
    lv_obj_remove_style_all(s_wall_grad);
    lv_obj_set_style_bg_color(s_wall_grad, lv_color_hex(0x1D6F78), 0);
    lv_obj_set_style_bg_grad_color(s_wall_grad, lv_color_hex(0x071722), 0);
    lv_obj_set_style_bg_grad_dir(s_wall_grad, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_wall_grad, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_wall_grad, LV_OBJ_FLAG_CLICKABLE);

    s_wall_img = lv_image_create(s_home);
    lv_obj_remove_flag(s_wall_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_wall_img, LV_OBJ_FLAG_HIDDEN);

    s_pages = lv_obj_create(s_home);
    lv_obj_remove_style_all(s_pages);
    lv_obj_set_scroll_dir(s_pages, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(s_pages, LV_SCROLL_SNAP_START);
    lv_obj_set_scrollbar_mode(s_pages, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_pages, pages_scrolled, LV_EVENT_SCROLL_END, NULL);

    s_dots = lv_obj_create(s_home);
    lv_obj_remove_style_all(s_dots);
    lv_obj_set_flex_flow(s_dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_dots, 5, 0);
    lv_obj_clear_flag(s_dots, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_dots, LV_OBJ_FLAG_CLICKABLE);

    s_dock = lv_obj_create(s_home);
    lv_obj_remove_style_all(s_dock);
    lv_obj_set_style_bg_color(s_dock, lv_color_hex(0x0E1519), 0);
    lv_obj_set_style_bg_opa(s_dock, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(s_dock, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(s_dock, 1, 0);
    lv_obj_set_style_border_color(s_dock, lv_color_hex(0x2C3A40), 0);
    lv_obj_clear_flag(s_dock, LV_OBJ_FLAG_SCROLLABLE);

    build_menu(screen);                                 /* a child of the screen, above the home screen and below the sheets */
    load_apps();
    pins_load();
}
