/*
 * purr_w10m_settings.c - the Settings app of the W10M shell (plan7.md), laid out after the
 * reference project's: a "Find a setting" box over a list of sections, each opening a page of
 * switches, sliders, drop-downs and colour swatches. Written for 320x240; a page is one scrolling
 * column, so it follows the orientation by itself.
 *
 * Everything is styled by the theme (purr_w10m_theme.h): the same helpers an app gets for its own
 * windows. Most pages are tied to the real thing behind them:
 *
 *   System          backlight, orientation, which shell starts, restart
 *   Devices         what the hardware reports
 *   Network         Wi-Fi status, scan and connect, saved networks (purr_net)
 *   Personalization accent colour, light or dark, transparency, tile opacity (the theme)
 *   Apps            installed apps from the app manager: open, stop, remove, install
 *   Accounts        add, remove, change a password or role (login.c, which enforces who may do what)
 *   Time & Language time zone and 12 or 24 hour clock
 *   Privacy & security  boot policy (enforce signatures), developer mode, unsigned apps (own folder only)
 *   About           version, security state, storage
 *
 * The same settings that have a key (purr_settings.h) can be changed from the shell with `settings`.
 * Work that takes a while (a scan, a Wi-Fi connection, hashing a password, fetching an app) runs on
 * a short worker task, one at a time, and reports back to the page that asked.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "purr_brand.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "commands.h"
#include "login.h"
#include "purr_resetnote_os.h"
#include "purr_ui_assets.h"
#include "purr_appmgr.h"
#include "purr_catrt.h"
#include "purr_battery.h"
#include "purr_clock.h"
#include "purr_board.h"
#include "purr_kbdlight.h"
#include "purr_catrt_power.h"
#include "purr_driver.h"
#include "purr_hwscan_plat.h"
#include "purr_hwsetup.h"
#include "purr_perm.h"
#include "purr_lvgl.h"
#include "purr_net.h"
#include "purr_settings.h"
#include "purr_sysupdate.h"
#include "purr_recovery_src.h"
#include "purr_repo.h"
#include "purr_trust_os.h"
#include "purr_shell.h"
#include "purr_wallpaper.h"
#include "purr_ui_service.h"
#include "purr_w10m_priv.h"
#include "purr_w10m_theme.h"
#include "purr_w10m_widgets.h"

static const char *TAG = "settings";

#define HEAD_H 30

typedef enum {
    PAGE_MAIN,
    PAGE_SYSTEM,
    PAGE_DEVICES,
    PAGE_NETWORK,
    PAGE_PERSONAL,
    PAGE_APPS,
    PAGE_ACCOUNTS,
    PAGE_ACCT_USER,                     /* one account: password, role, remove */
    PAGE_ACCT_ADD,
    PAGE_TIME,
    PAGE_SECURITY,
    PAGE_DEVELOPER,
    PAGE_ABOUT,
    PAGE_GROUP_DISPLAY,                 /* the Android layout: Sound & display, over Display and Appearance */
    PAGE_GROUP_APPS,                    /* the Android layout: Applications, over Manage applications and Development */
} page_t;

typedef struct {
    const char *title;
    const char *desc;
    const char *icon;
    page_t page;
    int available;                      /* 0 = the OS has nothing behind it yet: dimmed */
} section_t;

static const section_t s_sections[] = {
    {"System",          "Display, shell, restart",       "wp_system",          PAGE_SYSTEM,   1},
    {"Devices",         "Screen, input, battery",      "wp_devices",         PAGE_DEVICES,  1},
    {"Network",         "Wi-Fi and wired",               "wp_network",         PAGE_NETWORK,  1},
    {"Personalization", "Colours, transparency, tiles",  "wp_personalization", PAGE_PERSONAL, 1},
    {"Apps",            "Installed apps, install",       "wp_apps",            PAGE_APPS,     1},
    {"Accounts",        "Users and passwords",           "wp_account",         PAGE_ACCOUNTS, 1},
    {"Time & Language", "Time zone, clock",              "wp_time",            PAGE_TIME,     1},
    {"Privacy & security", "Boot policy, developer options", "wp_privacy",      PAGE_SECURITY, 1},
    {"About",           "Version, security, storage",    "wp_about",           PAGE_ABOUT,    1},
};
#define N_SECTIONS ((int)(sizeof(s_sections) / sizeof(s_sections[0])))

/* The Android layout (plan7.md, "Android shell build-out"): the same pages, grouped the way Froyo's Settings was. Every setting reachable in the
 * table above is reachable here; Developer options sit under Applications > Development. */
static const section_t s_sections_android[] = {
    {"Wireless & networks", "Wi-Fi and wired",                         NULL, PAGE_NETWORK,       1},
    {"Sound & display",     "Brightness, screen timeout, appearance", NULL, PAGE_GROUP_DISPLAY, 1},
    {"Applications",        "Manage applications, development",       NULL, PAGE_GROUP_APPS,    1},
    {"Accounts & sync",     "Users and passwords",                     NULL, PAGE_ACCOUNTS,      1},
    {"Location & security", "Boot policy, security status",            NULL, PAGE_SECURITY,      1},
    {"Date & time",         "Time zone and clock",                     NULL, PAGE_TIME,          1},
    {"Hardware",            "Screen, input, battery",                  NULL, PAGE_DEVICES,       1},
    {"About phone",         "Version, security, storage",              NULL, PAGE_ABOUT,         1},
};

static const section_t *sections(int *n)
{
    if (purr_theme_skin() == PURR_SKIN_ANDROID) {
        *n = (int)(sizeof(s_sections_android) / sizeof(s_sections_android[0]));
        return s_sections_android;
    }
    *n = N_SECTIONS;
    return s_sections;
}

static lv_obj_t *s_set;                 /* the whole sheet */
static lv_obj_t *s_head, *s_head_title;
static lv_obj_t *s_body;
static lv_obj_t *s_search;
static lv_obj_t *s_rows[16];            /* the main page's rows, for the filter */
static lv_obj_t *s_status;              /* the page's one-line result area, or NULL */
static lv_group_t *s_group;
static page_t s_page = PAGE_MAIN;
static int s_open;
static int s_gen;                       /* bumped on every page change: late results for an old page are dropped */
static lv_obj_t *s_armed;               /* a destructive button waiting for its second tap */

static void show_page(page_t page);

static int acting_admin(void)
{
    return purr_login_is_root() || purr_login_current()->role == PURR_ROLE_USER_ADMIN;
}

/* ---------------------------------------------------------------- the helpers (the reference's cm_*) */

/* The helpers are shared with the OOBE (purr_w10m_widgets.c); the old names stay for this file. */
#define cm_text w10m_text
#define cm_dim w10m_dim
#define cm_title w10m_title
#define cm_space w10m_space
#define cm_switch w10m_switch
#define cm_slider w10m_slider
#define cm_dropdown w10m_dropdown
#define cm_button w10m_button
#define cm_button_text w10m_button_text
#define cm_row w10m_row
static lv_obj_t *cm_input(lv_obj_t *parent, const char *placeholder, int secret)
{
    return w10m_input(parent, s_group, placeholder, secret);
}

/* The page's result line: good news in the accent colour, bad news in amber. */
static char s_say_text[64];
static uint32_t s_say_tick;                     /* when say() last spoke: a job started right after takes these words to the status bar */

static void say(const char *text, int good)
{
    snprintf(s_say_text, sizeof(s_say_text), "%s", text);
    s_say_tick = lv_tick_get();
    w10m_banner(text, !good);                   /* every status and error also takes the strip for 5 seconds */
    if (s_status == NULL) {
        return;
    }
    lv_label_set_text(s_status, text);
    lv_obj_set_style_text_color(s_status, good ? purr_theme_accent_color() : lv_color_hex(0xFFB900), 0);
    lv_obj_t *b = w10m_last_button();           /* an error sits right under the button that failed */
    if (!good && b != NULL && lv_obj_get_parent(b) != NULL && lv_obj_is_valid(s_status)) {
        lv_obj_t *par = lv_obj_get_parent(b);
        lv_obj_set_parent(s_status, par);
        lv_obj_move_to_index(s_status, lv_obj_get_index(b) + 1);
    }
}

static void make_status(void)
{
    s_status = lv_label_create(s_body);
    lv_label_set_text(s_status, "");
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status, LV_PCT(100));
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_12, 0);
}

/* A destructive button needs two taps: the first changes its text, the second does it. Returns 1 on
 * the second tap. */
static int confirm_tap(lv_obj_t *btn, const char *again)
{
    if (s_armed == btn) {
        s_armed = NULL;
        return 1;
    }
    s_armed = btn;
    cm_button_text(btn, again);
    return 0;
}

/* ---------------------------------------------------------------- work off the UI task
 *
 * One job at a time. run() executes on a worker task (it may block for seconds); done() runs back on
 * the UI task, only if the page that asked is still the page on screen. */

typedef struct job {
    int gen;
    int rc;
    char msg[112];
    char a[40], b[72], c[72];
    int flag;
    unsigned stack;                     /* 0 = the default */
    int (*run)(struct job *);
    void (*done)(struct job *);
} job_t;

static int s_busy;

static void *job_ui_cb(void *arg)
{
    job_t *j = arg;
    s_busy = 0;
    w10m_busy_end();                            /* the status bar's dots stop whether or not this page is still showing */
    if (j->gen == s_gen && j->done != NULL) {
        j->done(j);
    }
    free(j);
    return NULL;
}

static void job_task(void *arg)
{
    job_t *j = arg;
    j->rc = j->run(j);
    purr_ui_service_run(job_ui_cb, j, NULL);
    vTaskDelete(NULL);
}

static job_t *job_new(int (*run)(job_t *), void (*done)(job_t *))
{
    job_t *j = calloc(1, sizeof(*j));
    if (j != NULL) {
        j->gen = s_gen;
        j->run = run;
        j->done = done;
    }
    return j;
}

/* Takes ownership of j. Returns 0 if it started. */
static int job_start(job_t *j)
{
    if (j == NULL) {
        return -1;
    }
    if (s_busy) {
        say("Still working on the last request.", 0);
        free(j);
        return -1;
    }
    s_busy = 1;
    if (xTaskCreate(job_task, "set-job", j->stack ? j->stack : 7168, j, 3, NULL) != pdPASS) {
        s_busy = 0;
        free(j);
        say("Out of memory.", 0);
        return -1;
    }
    if (s_say_text[0] != '\0' && lv_tick_elaps(s_say_tick) < 400) {       /* "Looking...", "Adding...": the words that went with this action */
        w10m_busy_begin(s_say_text);
    }
    return 0;
}

/* ---------------------------------------------------------------- main page */

static void sec_clicked(lv_event_t *e)
{
    const section_t *sec = lv_event_get_user_data(e);
    if (sec->available) {
        show_page(sec->page);
    }
}

static int contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && ((hay[i] | 32) == (needle[i] | 32))) {
            i++;
        }
        if (i == n) {
            return 1;
        }
    }
    return 0;
}

static void search_changed(lv_event_t *e)
{
    (void)e;
    const char *q = lv_textarea_get_text(s_search);
    int n;
    const section_t *secs = sections(&n);
    for (int i = 0; i < n; i++) {
        if (s_rows[i] == NULL) {
            continue;
        }
        int hit = q[0] == '\0' || contains_ci(secs[i].title, q) || contains_ci(secs[i].desc, q);
        if (hit) {
            lv_obj_remove_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void page_main(void)
{
    s_search = cm_input(s_body, "Find a setting", 0);
    lv_obj_add_event_cb(s_search, search_changed, LV_EVENT_VALUE_CHANGED, NULL);

    memset(s_rows, 0, sizeof(s_rows));
    int nsec;
    const section_t *secs = sections(&nsec);
    for (int i = 0; i < nsec; i++) {
        const section_t *sec = &secs[i];
        lv_obj_t *row = lv_obj_create(s_body);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 40);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
        purr_theme_apply(row, PURR_TH_ROW_PRESSED);
        lv_obj_set_style_opa(row, sec->available ? LV_OPA_COVER : LV_OPA_40, 0);
        lv_obj_add_event_cb(row, sec_clicked, LV_EVENT_CLICKED, (void *)sec);
        s_rows[i] = row;

        const lv_image_dsc_t *ic = sec->icon ? w10m_img(sec->icon) : NULL;
        if (ic != NULL) {
            lv_obj_t *im = lv_image_create(row);
            lv_image_set_src(im, ic);
            lv_image_set_scale(im, 160);
            purr_theme_apply(im, PURR_TH_ICON);
            lv_obj_align(im, LV_ALIGN_LEFT_MID, 2, 0);
            lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
        }
        int tx = ic != NULL ? 44 : 8;                   /* the Android layout has no row icons */
        lv_obj_t *t = cm_text(row, sec->title);
        lv_obj_align(t, LV_ALIGN_LEFT_MID, tx, -8);
        lv_obj_t *d = lv_label_create(row);
        lv_label_set_text(d, sec->desc);
        lv_obj_set_style_text_font(d, &lv_font_montserrat_12, 0);
        purr_theme_apply(d, PURR_TH_DIM);
        lv_obj_align(d, LV_ALIGN_LEFT_MID, tx, 9);
    }
}

/* ---------------------------------------------------------------- System */

static void brightness_cb(lv_event_t *e)
{
    w10m_set_brightness(lv_slider_get_value(lv_event_get_target(e)));
}

static void brightness_released_cb(lv_event_t *e)
{
    (void)e;
    w10m_save_display();                /* written when the finger lifts, not on every step */
}

static void rotation_cb(lv_event_t *e)
{
    w10m_apply_orientation((int)lv_dropdown_get_selected(lv_event_get_target(e)) * 90);
}

/* The Interface dropdown: the built-in shell, every installed shell (/ui/shells/<id>/), Navigation and Card stack. s_shell_ids[i] is what entry i chooses. */
#define SHELL_MAX 6
static purr_shell_info_t s_shells[SHELL_MAX];
static int s_shell_n;

static void shell_cb(lv_event_t *e)
{
    int sel = (int)lv_dropdown_get_selected(lv_event_get_target(e));
    const purr_setting_t *s = purr_settings_find("ui.shell");
    const char *mode = NULL;
    if (sel == 0) {
        mode = "builtin";
    } else if (sel >= 1 && sel <= s_shell_n) {
        mode = s_shells[sel - 1].id;
    } else if (sel == s_shell_n + 1) {
        mode = "nav";
    } else if (sel == s_shell_n + 2) {
        mode = "card_shell";
    }
    char err[64] = "";
    if (s != NULL && mode != NULL && s->set(mode, err, sizeof(err)) == 0) {
        say("Saved. It takes effect after a restart.", 1);
    } else {
        say(err[0] ? err : "Could not save.", 0);
    }
}

static void restart_cb(lv_event_t *e)
{
    if (confirm_tap(lv_event_get_target(e), "Tap again to restart")) {
        say("Restarting...", 1);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
}

/* Seconds for each choice of "Screen off after"; 0 = never. */
static const int k_timeouts[] = {15, 30, 60, 120, 300, 0};

static void timeout_cb(lv_event_t *e)
{
    int sel = (int)lv_dropdown_get_selected(lv_event_get_target(e));
    const purr_setting_t *s = purr_settings_find("power.screen_timeout");
    char v[12], err[64] = "";
    if (sel < 0 || sel >= (int)(sizeof(k_timeouts) / sizeof(k_timeouts[0]))) {
        return;
    }
    snprintf(v, sizeof(v), k_timeouts[sel] ? "%d" : "never", k_timeouts[sel]);
    if (s != NULL && s->set(v, err, sizeof(err)) == 0) {
        say("Saved.", 1);
    } else {
        say(err[0] ? err : "Could not save.", 0);
    }
}

static void page_system(void)
{
    cm_title(s_body, "Display");
    cm_text(s_body, "Brightness");
    lv_obj_t *sl = cm_slider(s_body, w10m_get_brightness(), 1, 255, LV_EVENT_VALUE_CHANGED, brightness_cb);
    lv_obj_add_event_cb(sl, brightness_released_cb, LV_EVENT_RELEASED, NULL);
    cm_space(s_body, 4);
    cm_text(s_body, "Screen off after");
    {
        char cur[12] = "60";
        const purr_setting_t *ts = purr_settings_find("power.screen_timeout");
        if (ts != NULL) {
            ts->get(cur, sizeof(cur));
        }
        int want = strcmp(cur, "never") == 0 ? 0 : atoi(cur), sel = 1;
        for (int i = 0; i < (int)(sizeof(k_timeouts) / sizeof(k_timeouts[0])); i++) {
            if (k_timeouts[i] == want) {
                sel = i;
            }
        }
        cm_dropdown(s_body, "15 seconds\n30 seconds\n1 minute\n2 minutes\n5 minutes\nNever", sel, 150, timeout_cb);
    }
    cm_space(s_body, 4);
    cm_text(s_body, "Rotation");
    cm_dropdown(s_body, "0\xC2\xB0\n90\xC2\xB0\n180\xC2\xB0\n270\xC2\xB0", purr_lvgl_get_rotation() / 90, 120, rotation_cb);

    cm_space(s_body, 6);
    cm_title(s_body, "Interface");
    s_shell_n = purr_shell_list(s_shells, SHELL_MAX);
    char names[SHELL_MAX * 64 + 80] = "Android 2.2 (built in)";
    for (int i = 0; i < s_shell_n; i++) {
        size_t n = strlen(names);
        snprintf(names + n, sizeof(names) - n, "\n%s%s", s_shells[i].name,
                 s_shells[i].state == PURR_SHELL_STATE_NEEDS_UPDATE ? " (needs an update)" : s_shells[i].state == PURR_SHELL_STATE_BAD ? " (damaged)" : "");
    }
    size_t nn = strlen(names);
    snprintf(names + nn, sizeof(names) - nn, "\nNavigation\nCard stack");
    char cur[24] = "builtin";
    purr_shell_mode_get(cur, sizeof(cur));
    int sel = strcmp(cur, "nav") == 0 ? s_shell_n + 1 : strcmp(cur, "card_shell") == 0 ? s_shell_n + 2 : 0;
    for (int i = 0; i < s_shell_n; i++) {
        if (strcmp(s_shells[i].id, cur) == 0) {
            sel = i + 1;
        }
    }
    char chosen[24];
    purr_shell_chosen(chosen, sizeof(chosen));
    for (int i = 0; i < s_shell_n; i++) {                                          /* what is chosen shows, even before the restart that makes it the running one */
        if (strcmp(cur, "nav") != 0 && strcmp(cur, "card_shell") != 0 && strcmp(s_shells[i].id, chosen) == 0) {
            sel = i + 1;
        }
    }
    lv_obj_t *dd = cm_dropdown(s_body, names, sel, 170, shell_cb);
    if (!acting_admin()) {
        lv_obj_add_state(dd, LV_STATE_DISABLED);
        cm_dim(s_body, "Only an administrator can change this.");
    }

    cm_space(s_body, 6);
    cm_title(s_body, "Power");
    lv_obj_t *b = cm_button(s_body, "Restart", restart_cb, NULL);
    if (!acting_admin()) {
        lv_obj_add_state(b, LV_STATE_DISABLED);
    }
    make_status();
}

/* ---------------------------------------------------------------- Devices */

static lv_obj_t *s_kbl_btn;
static lv_obj_t *s_start_msg;                   /* the result line under the Startup controls (this page has several sections with their own results) */

static void start_say(const char *t)
{
    if (s_start_msg != NULL) {
        lv_label_set_text(s_start_msg, t);
    }
}

static void bootmenu_cb(lv_event_t *e)
{
    if (!confirm_tap(lv_event_get_target(e), "Tap again to restart")) {
        return;
    }
    if (purr_boot_request_menu() != 0) {
        start_say("Could not ask for the menu.");
        return;
    }
    start_say("Restarting to the boot menu...");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static void kbl_text(void)
{
    char t[48];
    snprintf(t, sizeof(t), "Keyboard backlight: %s", purr_kbdlight_name(purr_kbdlight_get()));
    cm_button_text(s_kbl_btn, t);
}

static void kbl_cb(lv_event_t *e)
{
    (void)e;
    purr_kbdlight_set((purr_kbdlight_get() + 1) % 4);
    kbl_text();
}

static void page_devices(void)
{
    char buf[96];
    cm_title(s_body, "Display");
    snprintf(buf, sizeof(buf), "%d x %d, rotated %d degrees", w10m_disp_w(), w10m_disp_h(), purr_lvgl_get_rotation());
    cm_text(s_body, buf);

    cm_space(s_body, 4);
    cm_title(s_body, "Input");
    cm_text(s_body, purr_lvgl_keypad() != NULL ? "Keyboard: ready" : "Keyboard: not found");
    cm_text(s_body, "Touch screen: ready");

    if (purr_kbdlight_present()) {
        s_kbl_btn = cm_button(s_body, "", kbl_cb, NULL);
        lv_obj_set_width(s_kbl_btn, LV_PCT(100));
        kbl_text();
    }

    cm_space(s_body, 4);
    cm_title(s_body, "Startup");
    cm_dim(s_body, "Fast startup is turned off for now. The logo and the boot menu show on every start.");
    lv_obj_t *bm = cm_button(s_body, "Restart to the boot menu", bootmenu_cb, NULL);
    lv_obj_set_width(bm, LV_PCT(100));
    s_start_msg = cm_dim(s_body, "");

    cm_space(s_body, 4);
    cm_title(s_body, "Battery");
    int bp, bmv;
    if (purr_battery_get(&bp, &bmv)) {
        snprintf(buf, sizeof(buf), "%d%%  (%d.%02d V)", bp, bmv / 1000, (bmv % 1000) / 10);
    } else {
        snprintf(buf, sizeof(buf), "No battery reading");
    }
    cm_text(s_body, buf);

    cm_space(s_body, 4);
    cm_title(s_body, "Network adapters");
    cm_text(s_body, purr_net_wired() ? "Wired Ethernet: present" : "Wired Ethernet: none");
    cm_text(s_body, "Wi-Fi: built in");

    cm_space(s_body, 4);
    cm_title(s_body, "Memory");
    snprintf(buf, sizeof(buf), "RAM free: %u KB of %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024));
    cm_text(s_body, buf);
    snprintf(buf, sizeof(buf), "PSRAM free: %u KB of %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024));
    cm_text(s_body, buf);
    uint32_t flash = 0;
    if (esp_flash_get_size(NULL, &flash) == ESP_OK) {
        snprintf(buf, sizeof(buf), "Flash: %u MB", (unsigned)(flash / (1024 * 1024)));
        cm_text(s_body, buf);
    }
    cm_space(s_body, 4);
    cm_title(s_body, "Add hardware and updates");
    cm_dim(s_body, "Finding and adding devices, drivers and the developer server are in the Device Manager app. System updates and reinstalling are under About.");
    cm_space(s_body, 8);
}

/* ---------------------------------------------------------------- Network */

#define MAX_APS 16
static purr_net_ap_t s_aps[MAX_APS];
static int s_nap;
static lv_obj_t *s_net_state, *s_net_list, *s_net_form, *s_net_ssid, *s_net_pass;
static char s_net_pick[PURR_NET_SSID_LEN];

static void net_refresh_state(void)
{
    if (s_net_state == NULL) {
        return;
    }
    purr_net_status_t st;
    purr_net_status(&st);
    char buf[120];
    if (st.connected) {
        snprintf(buf, sizeof(buf), "%s: %s  (%s, %d dBm)", purr_net_wired() ? "Wired" : "Connected",
                 st.ssid[0] ? st.ssid : "network", st.ip, st.rssi);
    } else {
        snprintf(buf, sizeof(buf), "Not connected");
    }
    lv_label_set_text(s_net_state, buf);
}

static void net_connect_cb(lv_event_t *e);

static int net_connect_run(job_t *j)
{
    esp_err_t err = purr_net_connect(j->a, j->b, 20000);
    if (err == ESP_OK) {
        snprintf(j->msg, sizeof(j->msg), "Connected to %s.", j->a);
        return 0;
    }
    snprintf(j->msg, sizeof(j->msg), "Could not connect to %s (%s).", j->a, esp_err_to_name(err));
    return -1;
}

static void net_connect_done(job_t *j)
{
    say(j->msg, j->rc == 0);
    net_refresh_state();
    if (j->rc == 0 && s_net_form != NULL) {
        lv_obj_add_flag(s_net_form, LV_OBJ_FLAG_HIDDEN);
        lv_textarea_set_text(s_net_pass, "");
    }
}

static void net_start_connect(const char *ssid, const char *pass)
{
    job_t *j = job_new(net_connect_run, net_connect_done);
    if (j == NULL) {
        return;
    }
    snprintf(j->a, sizeof(j->a), "%s", ssid);
    snprintf(j->b, sizeof(j->b), "%s", pass);
    say("Connecting...", 1);
    job_start(j);
}

static void net_fill_list(void)
{
    if (s_net_list == NULL) {
        return;
    }
    lv_obj_clean(s_net_list);
    if (s_nap == 0) {
        cm_dim(s_net_list, "No networks found.");
        return;
    }
    for (int i = 0; i < s_nap; i++) {
        char sub[48];
        snprintf(sub, sizeof(sub), "%d dBm  %s", s_aps[i].rssi, s_aps[i].open ? "open" : "password");
        cm_row(s_net_list, s_aps[i].ssid, sub, 1, net_connect_cb, (void *)(intptr_t)i);
    }
}

static int net_scan_run(job_t *j)
{
    int n = 0;
    esp_err_t err = purr_net_scan(s_aps, MAX_APS, &n);
    s_nap = err == ESP_OK ? n : 0;
    if (err == ESP_OK) {
        snprintf(j->msg, sizeof(j->msg), "Found %d network(s).", n);
    } else {
        snprintf(j->msg, sizeof(j->msg), "Scan failed (%s).", esp_err_to_name(err));
    }
    return err == ESP_OK ? 0 : -1;
}

static void net_scan_done(job_t *j)
{
    say(j->msg, j->rc == 0);
    net_fill_list();
}

static void net_scan_cb(lv_event_t *e)
{
    (void)e;
    job_t *j = job_new(net_scan_run, net_scan_done);
    say("Scanning...", 1);
    job_start(j);
}

/* Tap a network: an open one connects at once, a protected one asks for its password first. */
static void net_connect_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_nap) {
        return;
    }
    if (s_aps[i].open) {
        net_start_connect(s_aps[i].ssid, "");
        return;
    }
    snprintf(s_net_pick, sizeof(s_net_pick), "%s", s_aps[i].ssid);
    lv_label_set_text(s_net_ssid, s_net_pick);
    lv_obj_remove_flag(s_net_form, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_text(s_net_pass, "");
    lv_group_focus_obj(s_net_pass);
    lv_obj_scroll_to_view(s_net_form, LV_ANIM_ON);
}

static void net_go_cb(lv_event_t *e)
{
    (void)e;
    net_start_connect(s_net_pick, lv_textarea_get_text(s_net_pass));
}

static void net_forget_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const char *ssid = lv_event_get_user_data(e);
    if (confirm_tap(btn, "Sure?")) {
        purr_net_forget(ssid);
        show_page(PAGE_NETWORK);
    }
}

static void page_network(void)
{
    cm_title(s_body, "Status");
    s_net_state = cm_text(s_body, "");
    lv_label_set_long_mode(s_net_state, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_net_state, LV_PCT(100));
    net_refresh_state();
    if (purr_net_wired()) {
        cm_dim(s_body, "A wired connection is in use.");
    }

    cm_space(s_body, 4);
    cm_title(s_body, "Wi-Fi networks");
    cm_button(s_body, "Scan", net_scan_cb, NULL);
    s_net_list = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_net_list);
    lv_obj_set_width(s_net_list, LV_PCT(100));
    lv_obj_set_height(s_net_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_net_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_net_list, 2, 0);
    net_fill_list();

    s_net_form = lv_obj_create(s_body);                  /* the password form, shown on demand */
    lv_obj_remove_style_all(s_net_form);
    lv_obj_set_width(s_net_form, LV_PCT(100));
    lv_obj_set_height(s_net_form, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_net_form, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_net_form, 4, 0);
    lv_obj_add_flag(s_net_form, LV_OBJ_FLAG_HIDDEN);
    s_net_ssid = cm_text(s_net_form, "");
    s_net_pass = cm_input(s_net_form, "Password", 1);
    cm_button(s_net_form, "Connect", net_go_cb, NULL);

    cm_space(s_body, 4);
    cm_title(s_body, "Saved networks");
    static purr_net_ap_t saved[8];
    int n = purr_net_saved(saved, 8);
    if (n <= 0) {
        cm_dim(s_body, "None yet.");
    }
    for (int i = 0; i < n; i++) {
        lv_obj_t *row = cm_row(s_body, saved[i].ssid, NULL, 0, NULL, NULL);
        lv_obj_set_style_opa(row, LV_OPA_COVER, 0);
        lv_obj_t *b = cm_button(row, "Forget", net_forget_cb, saved[i].ssid);
        lv_obj_set_height(b, 26);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    make_status();
}


/* ---------------------------------------------------------------- Personalization */

#define N_SWATCH_MAX 24
static lv_obj_t *s_tick[N_SWATCH_MAX];

static void swatch_ticks(void)
{
    for (int i = 0; i < purr_theme_accent_count() && i < N_SWATCH_MAX; i++) {
        if (s_tick[i] == NULL) {
            continue;
        }
        if (i == purr_theme_accent()) {
            lv_obj_remove_flag(s_tick[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_tick[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void swatch_clicked(lv_event_t *e)
{
    purr_theme_set_accent((int)(intptr_t)lv_event_get_user_data(e));
    swatch_ticks();
}

static void base_cb(lv_event_t *e)
{
    purr_theme_set_dark(lv_dropdown_get_selected(lv_event_get_target(e)) == 0);
}

static void transparency_cb(lv_event_t *e)
{
    purr_theme_set_transparency(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void tile_opa_cb(lv_event_t *e)
{
    purr_theme_set_tile_opacity(lv_slider_get_value(lv_event_get_target(e)));
}

static void wallpaper_cb(lv_event_t *e)
{
    purr_wallpaper_set((int)lv_dropdown_get_selected(lv_event_get_target(e)));      /* saved, and the shell redraws */
}

/* Themes (plan7.md): chosen here, applied at the next start. The ids live in static storage because the rows keep pointers to them. */
static char s_theme_ids[PURR_UI_THEME_SLOTS + 8][40];

static void theme_pick_cb(lv_event_t *e)
{
    if (!acting_admin()) {
        say("Only an administrator can change the theme.", 0);
        return;
    }
    const char *id = lv_event_get_user_data(e);
    int r = purr_ui_theme_select(id);
    if (r == 0) {
        say(id[0] ? "Theme chosen. It takes effect when the device restarts." : "No theme. The shell's own pictures are used after the next restart.", 1);
    } else if (r == -2) {
        say("Three themes are installed. Remove one first, then choose this one from the SD card.", 0);
    } else {
        say("Could not choose that theme.", 0);
    }
}

static void theme_remove_cb(lv_event_t *e)
{
    if (!acting_admin()) {
        say("Only an administrator can remove a theme.", 0);
        return;
    }
    char chosen[40];
    if (!purr_ui_theme_chosen(chosen, sizeof(chosen))) {
        say("No theme is chosen.", 0);
        return;
    }
    if (!confirm_tap(lv_event_get_target(e), "Tap again to remove it")) {
        return;
    }
    say(purr_ui_theme_remove(chosen) == 0 ? "Removed. Open this page again to refresh it." : "Could not remove it.", 1);
}

static void theme_section(void)
{
    static purr_theme_info_t th[PURR_UI_THEME_SLOTS + 6];
    int n = purr_ui_theme_list(th, PURR_UI_THEME_SLOTS + 6);
    char chosen[40] = "", line[112], sub[96];
    purr_ui_theme_chosen(chosen, sizeof(chosen));
    cm_space(s_body, 4);
    cm_title(s_body, "Theme");
    const char *in_use = purr_ui_assets_theme();
    snprintf(line, sizeof(line), "In use: %s", in_use[0] ? in_use : "none (the shell's own pictures)");
    cm_dim(s_body, line);
    if (purr_ui_assets_theme_failed()[0]) {
        snprintf(line, sizeof(line), "The theme %s could not be loaded, so the shell's own pictures are used.", purr_ui_assets_theme_failed());
        lv_obj_set_style_text_color(cm_dim(s_body, line), lv_color_hex(0xFFB900), 0);
    }
    s_theme_ids[0][0] = '\0';
    cm_row(s_body, chosen[0] ? "None" : "None (chosen)", "The shell's own pictures", 1, theme_pick_cb, (void *)s_theme_ids[0]);
    for (int i = 0; i < n && i < PURR_UI_THEME_SLOTS + 6; i++) {
        snprintf(s_theme_ids[i + 1], sizeof(s_theme_ids[i + 1]), "%s", th[i].id);
        char title[64];
        snprintf(title, sizeof(title), "%s%s", th[i].name, strcmp(chosen, th[i].id) == 0 ? " (chosen)" : "");
        snprintf(sub, sizeof(sub), "%s%s%s%s%s", th[i].author, th[i].author[0] && th[i].version[0] ? ", " : "", th[i].version,
                 strcmp(th[i].shell, "any") != 0 && strcmp(th[i].shell, purr_ui_assets_shell()) != 0 ? ", for another shell" : "",
                 th[i].on_sd ? ", on the SD card" : "");
        cm_row(s_body, title, sub[0] ? sub : NULL, 1, theme_pick_cb, (void *)s_theme_ids[i + 1]);
    }
    if (n == 0) {
        cm_dim(s_body, "No themes are installed.");
    }
    cm_dim(s_body, "Up to three themes are kept on the device; more can wait on the SD card and are copied in when chosen. A change takes effect when the device restarts.");
    if (chosen[0]) {
        lv_obj_t *rb = cm_button(s_body, "Remove the chosen theme", theme_remove_cb, NULL);
        lv_obj_set_width(rb, LV_PCT(100));
    }
}

static void page_personal(void)
{
    cm_title(s_body, "Colours");
    cm_text(s_body, "Accent colour");
    lv_obj_t *grid = lv_obj_create(s_body);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, 4, 0);
    lv_obj_set_style_pad_column(grid, 4, 0);
    memset(s_tick, 0, sizeof(s_tick));
    for (int i = 0; i < purr_theme_accent_count() && i < N_SWATCH_MAX; i++) {
        const purr_accent_t *a = purr_theme_accent_at(i);
        lv_obj_t *sw = lv_obj_create(grid);
        lv_obj_remove_style_all(sw);
        lv_obj_set_size(sw, 32, 32);
        lv_obj_set_style_bg_color(sw, lv_color_hex(a->color), 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_outline_color(sw, lv_color_white(), LV_STATE_PRESSED);
        lv_obj_set_style_outline_width(sw, 3, LV_STATE_PRESSED);
        lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(sw, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(sw, swatch_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        uint32_t c = a->color;
        uint32_t lum = (299 * ((c >> 16) & 0xFF) + 587 * ((c >> 8) & 0xFF) + 114 * (c & 0xFF)) / 1000;
        lv_obj_t *tick = lv_label_create(sw);
        lv_label_set_text(tick, LV_SYMBOL_OK);
        lv_obj_set_style_text_color(tick, lum > 150 ? lv_color_black() : lv_color_white(), 0);
        lv_obj_center(tick);
        s_tick[i] = tick;
    }
    swatch_ticks();

    cm_space(s_body, 4);
    cm_text(s_body, "Background");
    cm_dropdown(s_body, "Dark\nLight", purr_theme_dark() ? 0 : 1, 120, base_cb);

    cm_space(s_body, 4);
    cm_title(s_body, "Wallpaper");
    {
        char names[256] = "";
        for (int i = 0; i < purr_wallpaper_count(); i++) {
            size_t n = strlen(names);
            snprintf(names + n, sizeof(names) - n, "%s%s", i ? "\n" : "", purr_wallpaper_name(i));
        }
        cm_dropdown(s_body, names, purr_wallpaper_get() < purr_wallpaper_count() ? purr_wallpaper_get() : 0, 150, wallpaper_cb);
    }
    cm_dim(s_body, "On the start, lock and sign-in screens. It stays when you switch the shell.");
    theme_section();

    cm_space(s_body, 4);
    cm_title(s_body, "Effects");
    cm_switch(s_body, "Transparency effects", purr_theme_transparency(), transparency_cb);
    cm_dim(s_body, "Lets the wallpaper show through the status bar and sheets. Off is faster.");
    if (purr_theme_skin() != PURR_SKIN_ANDROID) {                   /* the Android shell has no tiles */
        cm_space(s_body, 4);
        cm_text(s_body, "Tile opacity");
        cm_slider(s_body, purr_theme_tile_opacity(), 40, 255, LV_EVENT_RELEASED, tile_opa_cb);
    }
    cm_space(s_body, 8);
}

/* ---------------------------------------------------------------- Apps (the app manager) */

static purr_app_registry_t s_reg;
static char s_runq[1600];               /* the runtime's list of running apps */
static lv_obj_t *s_app_list, *s_app_src;

static int is_running(const char *name)
{
    size_t n = strlen(name);
    for (const char *p = s_runq; (p = strstr(p, "app ")) != NULL; p += 4) {
        const char *colon = strchr(p, ':');
        if (colon == NULL) {
            break;
        }
        colon++;
        while (*colon == ' ') colon++;
        if (strncmp(colon, name, n) == 0 && (colon[n] == ' ' || colon[n] == '\n' || colon[n] == '\0' || colon[n] == ',')) {
            return 1;
        }
    }
    return 0;
}

static void apps_load(void);

static int app_load_run(job_t *j)
{
    (void)j;
    memset(&s_reg, 0, sizeof(s_reg));
    purr_apps_scan_current_user(&s_reg);
    s_runq[0] = '\0';
    purr_catrt_query("apps", s_runq, sizeof(s_runq));
    return 0;
}

static void app_act_cb(lv_event_t *e);

static void app_load_done(job_t *j)
{
    (void)j;
    if (s_app_list == NULL) {
        return;
    }
    lv_obj_clean(s_app_list);
    int unsigned_ok = purr_security_unsigned_apps_allowed();
    if (s_reg.count == 0) {
        cm_dim(s_app_list, "No apps installed for this account.");
    }
    for (int i = 0; i < s_reg.count; i++) {
        const purr_app_entry_t *a = &s_reg.apps[i];
        char sub[80];
        snprintf(sub, sizeof(sub), "v%s  %u KB  %s%s", a->version, (unsigned)((a->size + 1023) / 1024),
                 a->verified ? "signed" : unsigned_ok ? "UNSIGNED (developer mode)" : "NOT VERIFIED - will not run",
                 is_running(a->name) ? "  running" : "");
        lv_obj_t *row = cm_row(s_app_list, a->name, sub, 0, NULL, NULL);
        lv_obj_set_height(row, 44);
        lv_obj_set_style_opa(row, LV_OPA_COVER, 0);
        lv_obj_t *rm = cm_button(row, "Remove", app_act_cb, (void *)(intptr_t)(i * 4 + 2));
        lv_obj_set_height(rm, 26);
        lv_obj_align(rm, LV_ALIGN_RIGHT_MID, 0, 0);
        if ((a->verified || unsigned_ok) && a->chip_ok) {
            lv_obj_t *go = cm_button(row, is_running(a->name) ? "Stop" : "Open", app_act_cb,
                                     (void *)(intptr_t)(i * 4 + (is_running(a->name) ? 1 : 0)));
            lv_obj_set_height(go, 26);
            lv_obj_align_to(go, rm, LV_ALIGN_OUT_LEFT_MID, -4, 0);
        }
    }
}

static void apps_load(void)
{
    if (s_app_list != NULL) {
        lv_obj_clean(s_app_list);
        cm_dim(s_app_list, "Checking signatures...");
    }
    job_start(job_new(app_load_run, app_load_done));
}

static int app_do_run(job_t *j)
{
    char out[96] = "";
    if (j->flag == 2) {                                   /* remove: stop it first if it is running */
        char line[64];
        snprintf(line, sizeof(line), "close %s", j->a);
        purr_catrt_dispatch(line, out, sizeof(out));
        if (purr_apps_remove(j->a) == 0) {
            snprintf(j->msg, sizeof(j->msg), "%s removed.", j->a);
            return 0;
        }
        snprintf(j->msg, sizeof(j->msg), "Could not remove %s.", j->a);
        return -1;
    }
    char line[64];
    snprintf(line, sizeof(line), "%s %s", j->flag == 1 ? "close" : "open", j->a);
    int rc = purr_catrt_dispatch(line, out, sizeof(out));
    char *nl = strchr(out, '\n');
    if (nl) *nl = '\0';
    snprintf(j->msg, sizeof(j->msg), "%s", out[0] ? out : (rc == 0 ? "done" : "failed"));
    return rc;
}

static void app_do_done(job_t *j)
{
    say(j->msg, j->rc == 0);
    if (j->flag == 2 && j->rc == 0) {
        w10m_apps_changed();                              /* the tiles and the app list follow */
    }
    apps_load();
}

static void app_act_cb(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    int idx = code / 4, what = code % 4;
    if (idx < 0 || idx >= s_reg.count) {
        return;
    }
    if (what == 2 && !confirm_tap(lv_event_get_target(e), "Sure?")) {
        return;
    }
    job_t *j = job_new(app_do_run, app_do_done);
    if (j == NULL) {
        return;
    }
    snprintf(j->a, sizeof(j->a), "%s", s_reg.apps[idx].name);
    j->flag = what;
    say(what == 2 ? "Removing..." : what == 1 ? "Stopping..." : "Opening...", 1);
    job_start(j);
}

static int app_install_run(job_t *j)
{
    int rc = purr_apps_install(j->b, j->msg, sizeof(j->msg));
    if (rc != 0) {
        char t[112];
        snprintf(t, sizeof(t), "%s", j->msg);
        snprintf(j->msg, sizeof(j->msg), "Could not install: %.90s", t);
    }
    return rc;
}

static void app_install_done(job_t *j)
{
    say(j->msg, j->rc == 0);
    if (j->rc == 0) {
        lv_textarea_set_text(s_app_src, "");
        w10m_apps_changed();
    }
    apps_load();
}

static void app_install_cb(lv_event_t *e)
{
    (void)e;
    const char *src = lv_textarea_get_text(s_app_src);
    if (src[0] == '\0') {
        say("Type a file path or an http address first.", 0);
        return;
    }
    job_t *j = job_new(app_install_run, app_install_done);
    if (j == NULL) {
        return;
    }
    snprintf(j->b, sizeof(j->b), "%s", src);
    say("Installing... an address is fetched first, which can take a while.", 1);
    job_start(j);
}

/* ---- what apps are allowed to do (plan10.md): the decisions on file, with a switch each. An app is asked the
 * first time it needs a permission; turning a switch off here makes it ask again next time. */
static char s_perm_apps[PURR_PERM_MAX_APPS][32];

static void perm_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    int code = (int)(intptr_t)lv_obj_get_user_data(sw);
    int app = code / PURR_PERM_COUNT, perm = code % PURR_PERM_COUNT;
    int on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (app < 0 || app >= PURR_PERM_MAX_APPS || s_perm_apps[app][0] == '\0') {
        return;
    }
    if (purr_catrt_perm_put(s_perm_apps[app], perm, on ? PURR_PERM_ALWAYS : PURR_PERM_ASK) == 0) {
        say(on ? "Allowed." : "It will ask again next time.", 1);
    } else {
        say("Could not save.", 0);
    }
}

static void perm_section(void)
{
    int n = purr_catrt_perm_apps(s_perm_apps, PURR_PERM_MAX_APPS);
    cm_title(s_body, "Permissions");
    if (n == 0) {
        cm_dim(s_body, "No app has asked for one yet.");
        return;
    }
    for (int i = 0; i < n; i++) {
        cm_text(s_body, s_perm_apps[i]);
        for (int p = 0; p < PURR_PERM_COUNT; p++) {
            static const char *const label[PURR_PERM_COUNT] = {"Keep the device awake", "Wake on a schedule", "See and change your files", "Read system files",
                                                               "Manage devices and updates", "Install and update apps",
                                                               "Read and send messages", "Read and change contacts", "Use the LoRa radio"};
            lv_obj_t *sw = cm_switch(s_body, label[p], purr_catrt_perm_get(s_perm_apps[i], p) == PURR_PERM_ALWAYS,
                                     perm_cb);
            lv_obj_set_user_data(sw, (void *)(intptr_t)(i * PURR_PERM_COUNT + p));
        }
    }
}

static void page_apps(void)
{
    cm_title(s_body, "Installed");
    s_app_list = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_app_list);
    lv_obj_set_width(s_app_list, LV_PCT(100));
    lv_obj_set_height(s_app_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_app_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_app_list, 2, 0);

    cm_space(s_body, 4);
    cm_title(s_body, "Install an app");
    s_app_src = cm_input(s_body, "File path or http address of a .cat", 0);
    cm_button(s_body, "Install", app_install_cb, NULL);
    cm_dim(s_body, "Only apps signed by a trusted key are accepted.");
    cm_space(s_body, 4);
    perm_section();
    make_status();
    apps_load();
}

/* ---------------------------------------------------------------- Accounts */

static char s_sel_user[PURR_USER_NAME_LEN];
static lv_obj_t *s_acct_cur, *s_acct_new, *s_acct_new2, *s_acct_name;
static lv_obj_t *s_acct_admin_sw;
static lv_obj_t *s_acct_auto_sw, *s_acct_auto_pw;

static int acct_run(job_t *j)
{
    purr_acct_result_t r;
    switch (j->flag) {
    case 0:  r = purr_login_account_add(j->a, j->b, j->c[0] == '1'); break;
    case 1:  r = purr_login_account_set_password(j->a, j->b, j->c[0] ? j->c : NULL); break;
    case 2:  r = purr_login_account_remove(j->a); break;
    case 4:  r = purr_login_set_autologin(j->c[0] == '1' ? j->a : NULL, j->b); break;
    default: r = purr_login_account_set_admin(j->a, j->c[0] == '1'); break;
    }
    snprintf(j->msg, sizeof(j->msg), "%s", purr_login_account_error(r));
    return r == PURR_ACCT_OK ? 0 : -1;
}

static void acct_done(job_t *j)
{
    if (j->rc != 0) {
        say(j->msg, 0);
        if (j->flag == 4 && s_acct_auto_sw != NULL) {            /* refused: put the switch back */
            if (j->c[0] == '1') lv_obj_remove_state(s_acct_auto_sw, LV_STATE_CHECKED);
            else lv_obj_add_state(s_acct_auto_sw, LV_STATE_CHECKED);
        }
        return;
    }
    switch (j->flag) {
    case 0: show_page(PAGE_ACCOUNTS); say("Account created.", 1); break;
    case 1:
        say("Password changed.", 1);
        if (s_acct_cur) lv_textarea_set_text(s_acct_cur, "");
        lv_textarea_set_text(s_acct_new, "");
        lv_textarea_set_text(s_acct_new2, "");
        break;
    case 2: show_page(PAGE_ACCOUNTS); say("Account removed. Its files were kept.", 1); break;
    case 4:
        say(j->c[0] == '1' ? "Automatic sign-in is on for this account." : "Automatic sign-in is off.", 1);
        if (s_acct_auto_pw) lv_textarea_set_text(s_acct_auto_pw, "");
        break;
    default: say("Role changed.", 1); break;
    }
}

static void acct_start(int what, const char *name, const char *pass, const char *extra, const char *wait)
{
    job_t *j = job_new(acct_run, acct_done);
    if (j == NULL) {
        return;
    }
    j->flag = what;
    snprintf(j->a, sizeof(j->a), "%s", name);
    snprintf(j->b, sizeof(j->b), "%s", pass ? pass : "");
    snprintf(j->c, sizeof(j->c), "%s", extra ? extra : "");
    say(wait, 1);
    job_start(j);
}

static void acct_pick_cb(lv_event_t *e)
{
    snprintf(s_sel_user, sizeof(s_sel_user), "%s", (const char *)lv_event_get_user_data(e));
    show_page(PAGE_ACCT_USER);
}

static void acct_add_open_cb(lv_event_t *e)
{
    (void)e;
    show_page(PAGE_ACCT_ADD);
}

static void page_accounts(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "Signed in as %s (%s)", purr_login_current()->name,
             acting_admin() ? "administrator" : "standard");
    cm_text(s_body, buf);
    cm_space(s_body, 2);
    cm_title(s_body, "Accounts");
    purr_user_list_t *l = purr_login_users();
    int admin = acting_admin();
    for (int i = 0; i < l->count; i++) {
        const purr_user_t *u = &l->users[i];
        int mine = strcmp(u->name, purr_login_current()->name) == 0;
        char sub[40];
        snprintf(sub, sizeof(sub), "%s%s", u->role == PURR_ROLE_USER_ADMIN ? "administrator" : "standard",
                 mine ? "  (you)" : "");
        cm_row(s_body, u->name, sub, admin || mine, acct_pick_cb, (void *)u->name);
    }
    cm_space(s_body, 4);
    lv_obj_t *add = cm_button(s_body, "Add an account", acct_add_open_cb, NULL);
    if (!admin) {
        lv_obj_add_state(add, LV_STATE_DISABLED);
        cm_dim(s_body, "Only an administrator can add accounts. You can change your own password.");
    }
    make_status();
}

static void acct_pw_cb(lv_event_t *e)
{
    (void)e;
    int mine = strcmp(s_sel_user, purr_login_current()->name) == 0;
    const char *n1 = lv_textarea_get_text(s_acct_new), *n2 = lv_textarea_get_text(s_acct_new2);
    if (n1[0] == '\0' || strcmp(n1, n2) != 0) {
        say("The new passwords are empty or do not match.", 0);
        return;
    }
    acct_start(1, s_sel_user, n1, mine ? lv_textarea_get_text(s_acct_cur) : "",
               "Changing the password... this takes a few seconds.");
}

static void acct_admin_cb(lv_event_t *e)
{
    int on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    acct_start(3, s_sel_user, "", on ? "1" : "0", "Changing the role...");
}

static void acct_auto_cb(lv_event_t *e)
{
    int on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    const char *pw = s_acct_auto_pw ? lv_textarea_get_text(s_acct_auto_pw) : "";
    if (on && pw[0] == '\0') {
        lv_obj_remove_state(lv_event_get_target(e), LV_STATE_CHECKED);
        say("Type your own password in the box above the switch first.", 0);
        return;
    }
    acct_start(4, s_sel_user, on ? pw : "", on ? "1" : "0", "Checking...");
}

static void acct_remove_cb(lv_event_t *e)
{
    if (confirm_tap(lv_event_get_target(e), "Tap again to remove it")) {
        acct_start(2, s_sel_user, "", "", "Removing...");
    }
}

static void page_acct_user(void)
{
    int mine = strcmp(s_sel_user, purr_login_current()->name) == 0;
    purr_user_t *u = purr_user_list_find(purr_login_users(), s_sel_user);
    cm_title(s_body, s_sel_user);
    s_acct_cur = NULL;
    if (mine) {
        s_acct_cur = cm_input(s_body, "Current password", 1);
    }
    s_acct_new = cm_input(s_body, "New password", 1);
    s_acct_new2 = cm_input(s_body, "New password again", 1);
    cm_button(s_body, mine ? "Change my password" : "Set password", acct_pw_cb, NULL);

    if (acting_admin() && u != NULL) {
        cm_space(s_body, 4);
        s_acct_admin_sw = cm_switch(s_body, "Administrator", u->role == PURR_ROLE_USER_ADMIN, acct_admin_cb);

        /* Automatic sign-in: no password asked at boot, only while developer mode is active. */
        char who[PURR_USER_NAME_LEN];
        purr_login_autologin_user(who, sizeof(who));
        cm_space(s_body, 4);
        s_acct_auto_pw = cm_input(s_body, "Your password (to turn this on)", 1);
        s_acct_auto_sw = cm_switch(s_body, "Sign in automatically", strcmp(who, s_sel_user) == 0, acct_auto_cb);
        if (!purr_security_dev_active()) {
            lv_obj_add_state(s_acct_auto_sw, LV_STATE_DISABLED);
            cm_dim(s_body, "Automatic sign-in only works while developer mode is active (Privacy & security).");
        } else {
            cm_dim(s_body, "This account signs in at boot with no password. For testing; developer mode only.");
        }
        if (!mine) {
            cm_space(s_body, 4);
            cm_button(s_body, "Remove this account", acct_remove_cb, NULL);
            cm_dim(s_body, "The account's files stay on the device.");
        }
    }
    make_status();
}

static void acct_create_cb(lv_event_t *e)
{
    (void)e;
    const char *name = lv_textarea_get_text(s_acct_name);
    const char *p1 = lv_textarea_get_text(s_acct_new), *p2 = lv_textarea_get_text(s_acct_new2);
    if (!purr_user_name_valid(name)) {
        say("Names use letters, digits, - and _.", 0);
        return;
    }
    if (p1[0] == '\0' || strcmp(p1, p2) != 0) {
        say("The passwords are empty or do not match.", 0);
        return;
    }
    acct_start(0, name, p1, lv_obj_has_state(s_acct_admin_sw, LV_STATE_CHECKED) ? "1" : "0",
               "Creating the account... this takes a few seconds.");
}

static void page_acct_add(void)
{
    cm_title(s_body, "New account");
    s_acct_name = cm_input(s_body, "Name", 0);
    s_acct_new = cm_input(s_body, "Password", 1);
    s_acct_new2 = cm_input(s_body, "Password again", 1);
    s_acct_admin_sw = cm_switch(s_body, "Administrator", 0, acct_admin_cb);
    lv_obj_remove_event_cb(s_acct_admin_sw, acct_admin_cb);     /* here it only picks the new account's role */
    cm_button(s_body, "Create", acct_create_cb, NULL);
    make_status();
}

/* ---------------------------------------------------------------- Time & Language */

static lv_obj_t *s_time_now;

static void time_refresh(void)
{
    if (s_time_now == NULL) {
        return;
    }
    time_t now = time(NULL);
    if (now > 1000000000) {
        struct tm tm;
        localtime_r(&now, &tm);
        char buf[48];
        strftime(buf, sizeof(buf), "%A %d %B %Y  %H:%M", &tm);
        lv_label_set_text(s_time_now, buf);
    } else {
        lv_label_set_text(s_time_now, "The clock has not been set.");
    }
}

static void zone_cb(lv_event_t *e)
{
    int sel = (int)lv_dropdown_get_selected(lv_event_get_target(e));
    const purr_setting_t *s = purr_settings_find("time.zone");
    char err[80] = "";
    if (s != NULL && purr_settings_zone_name(sel) != NULL && s->set(purr_settings_zone_name(sel), err, sizeof(err)) == 0) {
        time_refresh();
        say("Time zone saved.", 1);
    } else {
        say(err[0] ? err : "Could not save.", 0);
    }
}

static void clock24_cb(lv_event_t *e)
{
    int on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    purr_settings_kv_set("time.clock24", on ? "1" : "0");
    say("Saved. The clock updates within a second.", 1);
}

static lv_obj_t *s_ntp_line;

static void ntp_text(void)
{
    char t[80];
    purr_clock_status(t, sizeof(t));
    if (s_ntp_line != NULL) {
        lv_label_set_text(s_ntp_line, t);
    }
}

static void ntp_cb(lv_event_t *e)
{
    purr_clock_set_ntp_enabled(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
    ntp_text();
}

static lv_timer_t *s_ntp_timer;

static void ntp_poll_cb(lv_timer_t *t)
{
    if (!purr_clock_busy()) {
        w10m_busy_end();
        lv_timer_delete(t);
        s_ntp_timer = NULL;
        ntp_text();
    }
}

static void ntp_sync_cb(lv_event_t *e)
{
    (void)e;
    if (purr_clock_sync_now()) {
        say("Asking for the time...", 1);
        w10m_busy_begin("Asking for the time...");
        if (s_ntp_timer == NULL) {
            s_ntp_timer = lv_timer_create(ntp_poll_cb, 200, NULL);
        }
    } else {
        say(purr_clock_ntp_enabled() ? "Not online yet." : "Automatic time is off.", 0);
    }
    ntp_text();
}

static void page_time(void)
{
    cm_title(s_body, "Date and time");
    s_time_now = cm_text(s_body, "");
    time_refresh();
    cm_switch(s_body, "Set time automatically", purr_clock_ntp_enabled(), ntp_cb);
    s_ntp_line = cm_dim(s_body, "");
    ntp_text();
    lv_obj_t *sb = cm_button(s_body, "Sync now", ntp_sync_cb, NULL);
    lv_obj_set_width(sb, LV_PCT(100));
    cm_space(s_body, 4);
    cm_text(s_body, "Time zone");
    char names[400] = "";
    for (int i = 0; i < purr_settings_zone_count(); i++) {
        size_t used = strlen(names);
        snprintf(names + used, sizeof(names) - used, "%s%s", i ? "\n" : "", purr_settings_zone_name(i));
    }
    char cur[48] = "UTC";
    purr_settings_kv_get("time.zone", cur, sizeof(cur));
    int sel = 0;
    for (int i = 0; i < purr_settings_zone_count(); i++) {
        if (strcasecmp(cur, purr_settings_zone_name(i)) == 0) {
            sel = i;
        }
    }
    lv_obj_t *dd = cm_dropdown(s_body, names, sel, 170, zone_cb);
    if (!acting_admin()) {
        lv_obj_add_state(dd, LV_STATE_DISABLED);
        cm_dim(s_body, "Only an administrator can change the time zone.");
    }
    cm_space(s_body, 4);
    cm_switch(s_body, "24-hour clock", purr_settings_kv_int("time.clock24", 1), clock24_cb);
    cm_space(s_body, 4);
    cm_title(s_body, "Language");
    cm_text(s_body, "English");
    cm_dim(s_body, "Other languages are not available yet.");
    make_status();
}

/* ---------------------------------------------------------------- Privacy & security, Developer options */

static purr_secure_status_t s_secst;
static lv_obj_t *s_sec_state, *s_sec_boot, *s_sec_trust, *s_sec_slot, *s_sec_active;
static lv_obj_t *s_sec_enforce_sw, *s_sec_unsigned_sw, *s_sec_dev_btn, *s_sec_warn;
static lv_obj_t *s_sec_target;                          /* the switch a change was asked on, to put back if refused */

static const char *mode_word(int m)
{
    return m == PURR_SECURE_ENFORCE ? "enforce" : m == PURR_SECURE_WARN ? "warn" : "off";
}

static int sec_load_run(job_t *j)
{
    (void)j;
    purr_secure_status(&s_secst);           /* reads the trust block; may verify its signature */
    return 0;
}

static void sec_apply_status(void)
{
    char buf[110];
    if (s_sec_state) {
        snprintf(buf, sizeof(buf), "Device state: %s", purr_secure_level_name(s_secst.level));
        lv_label_set_text(s_sec_state, buf);
    }
    if (s_sec_boot) {
        snprintf(buf, sizeof(buf), "This boot: %s%s.  Next boot: %s%s.",
                 s_secst.have_handoff ? mode_word(s_secst.mode_at_boot) : "unknown",
                 s_secst.dev_at_boot ? " + developer" : "", mode_word(s_secst.mode_now),
                 s_secst.dev_now ? " + developer" : "");
        lv_label_set_text(s_sec_boot, buf);
    }
    if (s_sec_trust) {
        lv_label_set_text(s_sec_trust, s_secst.signed_now ? "Trust block: signed (production policy)"
                                                          : "Trust block: not signed");
    }
    if (s_sec_slot) {
        if (s_secst.slot_verified) {
            snprintf(buf, sizeof(buf), "System image: verified, key %u", (unsigned)s_secst.key_id);
        } else {
            snprintf(buf, sizeof(buf), "System image: NOT verified (booted with a warning)");
        }
        lv_label_set_text(s_sec_slot, buf);
    }
    if (s_sec_enforce_sw) {
        if (s_secst.mode_now == PURR_SECURE_ENFORCE) {
            lv_obj_add_state(s_sec_enforce_sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(s_sec_enforce_sw, LV_STATE_CHECKED);
        }
    }
    if (s_sec_dev_btn) {
        cm_button_text(s_sec_dev_btn, s_secst.dev_now ? "Turn developer mode off" : "Turn developer mode on");
    }
    if (s_sec_active) {
        lv_label_set_text(s_sec_active, purr_security_dev_active()
                              ? "Developer mode is ACTIVE this boot."
                              : "Developer mode is not active this boot.");
    }
    if (s_sec_unsigned_sw) {
        int active = purr_security_dev_active();
        if (active && acting_admin()) {
            lv_obj_remove_state(s_sec_unsigned_sw, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_sec_unsigned_sw, LV_STATE_DISABLED);
        }
        if (purr_security_unsigned_apps_allowed()) {
            lv_obj_add_state(s_sec_unsigned_sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(s_sec_unsigned_sw, LV_STATE_CHECKED);
        }
    }
    if (s_sec_warn) {
        if (purr_security_unsigned_apps_allowed()) {
            lv_obj_remove_flag(s_sec_warn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_sec_warn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    w10m_dev_badge_update();
}

static void sec_load_done(job_t *j)
{
    (void)j;
    sec_apply_status();
}

static void sec_reload(void)
{
    job_start(job_new(sec_load_run, sec_load_done));
}

static int sec_change_run(job_t *j)
{
    int on = j->c[0] == '1';
    switch (j->flag) {
    case 0:  return purr_security_set_enforce(on, j->msg, sizeof(j->msg));
    case 1:  return purr_security_set_dev(on, j->msg, sizeof(j->msg));
    default: return purr_security_set_unsigned_apps(on, j->msg, sizeof(j->msg));
    }
}

static void sec_change_done(job_t *j)
{
    say(j->msg, j->rc == 0);
    if (j->rc != 0 && s_sec_target != NULL) {            /* refused: put the switch back */
        if (j->c[0] == '1') lv_obj_remove_state(s_sec_target, LV_STATE_CHECKED);
        else lv_obj_add_state(s_sec_target, LV_STATE_CHECKED);
    }
    if (j->flag == 2 && j->rc == 0) {
        w10m_apps_changed();                             /* unsigned apps appear in, or leave, the tiles */
    }
    sec_reload();
}

static void sec_change(int what, int on, lv_obj_t *target, const char *wait)
{
    job_t *j = job_new(sec_change_run, sec_change_done);
    if (j == NULL) {
        return;
    }
    j->flag = what;
    j->c[0] = on ? '1' : '0';
    s_sec_target = target;
    say(wait, 1);
    job_start(j);
}

static void sec_enforce_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    int on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    sec_change(0, on, sw, on ? "Checking the boot slots first..." : "Saving...");
}

static void sec_dev_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    int turning_on = !s_secst.dev_now;
    if (!confirm_tap(btn, turning_on ? "Tap again: unsigned images will boot with a warning"
                                      : "Tap again to turn developer mode off")) {
        return;
    }
    sec_change(1, turning_on, NULL, "Saving...");
}

static void sec_unsigned_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    int on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    sec_change(2, on, sw, "Saving...");
}

static void sec_restart_cb(lv_event_t *e)
{
    if (confirm_tap(lv_event_get_target(e), "Tap again to restart")) {
        say("Restarting...", 1);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
}

static void sec_open_dev_cb(lv_event_t *e)
{
    (void)e;
    show_page(PAGE_DEVELOPER);
}

static void page_security(void)
{
    cm_title(s_body, "Security");
    s_sec_state = cm_text(s_body, "Checking...");
    lv_label_set_long_mode(s_sec_state, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_sec_state, LV_PCT(100));
    s_sec_boot = cm_dim(s_body, "");
    s_sec_trust = cm_dim(s_body, "");
    s_sec_slot = cm_dim(s_body, "");
    {
        char who[PURR_USER_NAME_LEN], buf[96];
        purr_login_autologin_user(who, sizeof(who));
        snprintf(buf, sizeof(buf), "Automatic sign-in: %s%s", who[0] ? who : "off",
                 who[0] && !purr_security_dev_active() ? " (inactive: developer mode is off)" : "");
        cm_dim(s_body, buf);
    }

    cm_space(s_body, 4);
    cm_title(s_body, "Boot policy");
    s_sec_enforce_sw = cm_switch(s_body, "Enforce signatures", 0, sec_enforce_cb);
    cm_dim(s_body, "From the next boot, an unsigned, altered or older system image is refused instead of "
                   "booting with a warning. Turning it on first checks that every boot slot verifies.");
    if (!acting_admin()) {
        lv_obj_add_state(s_sec_enforce_sw, LV_STATE_DISABLED);
        cm_dim(s_body, "Only an administrator can change this.");
    }

    cm_space(s_body, 4);
    cm_title(s_body, "Privacy");
    cm_dim(s_body, "App permissions (camera, location, files) are not built yet. For now an app can do what "
                   "the system lets any signed app do.");

    if (purr_theme_skin() != PURR_SKIN_ANDROID) {                   /* in the Android layout it is under Applications > Development */
        cm_space(s_body, 4);
        lv_obj_t *row = cm_row(s_body, "Developer options", "Developer mode, unsigned apps", acting_admin(),
                               sec_open_dev_cb, NULL);
        (void)row;
    }
    make_status();
    sec_reload();
}

static void portal_cb(lv_event_t *e)
{
    (void)e;
    w10m_devportal_open();
}

static void page_developer(void)
{
    cm_title(s_body, "Developer mode");
    cm_dim(s_body, "For building and testing apps. It relaxes the boot policy from enforce to warn: unsigned "
                   "or failed system images boot with a warning. A device whose signed trust block locks "
                   "developer mode out refuses it. Takes effect after a restart.");
    s_sec_dev_btn = cm_button(s_body, "Developer mode...", sec_dev_cb, NULL);
    if (!acting_admin()) {
        lv_obj_add_state(s_sec_dev_btn, LV_STATE_DISABLED);
    }
    s_sec_active = cm_text(s_body, "");
    lv_label_set_long_mode(s_sec_active, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_sec_active, LV_PCT(100));
    lv_obj_t *rb = cm_button(s_body, "Restart", sec_restart_cb, NULL);
    if (!acting_admin()) {
        lv_obj_add_state(rb, LV_STATE_DISABLED);
    }

    cm_space(s_body, 6);
    cm_title(s_body, "Unsigned apps and drivers");
    s_sec_unsigned_sw = cm_switch(s_body, "Allow unsigned apps and drivers", 0, sec_unsigned_cb);
    lv_obj_add_state(s_sec_unsigned_sw, LV_STATE_DISABLED);          /* until the status says developer mode is active */
    cm_dim(s_body, "Apps in your own folder (/home/<you>/apps) and driver files in /boot/drivers can load unsigned, "
                   "and only while developer mode is active. The system's apps and modules always need a signature. "
                   "Turning developer mode off turns this off too.");
    s_sec_warn = cm_dim(s_body, "WARNING: an unsigned app runs with the same access as the system, and an unsigned driver is "
                                "code inside the kernel. Only install ones you built or trust.");
    lv_obj_set_style_text_color(s_sec_warn, lv_color_hex(0xFFB900), 0);
    lv_obj_add_flag(s_sec_warn, LV_OBJ_FLAG_HIDDEN);
    if (!acting_admin()) {
        cm_dim(s_body, "Only an administrator can change developer options.");
    }
    if (purr_theme_skin() == PURR_SKIN_ANDROID) {                   /* the Developer Portal is reached from here too */
        cm_space(s_body, 6);
        cm_title(s_body, "Developer Portal");
        cm_row(s_body, "Developer Portal", "Build, install and test apps from a developer server", purr_security_dev_active(), portal_cb, NULL);
    }
    make_status();
    sec_reload();
}

/* ---------------------------------------------------------------- About */

/* System update and reinstall: in Settings for every shell (Settings is shared). Device Manager keeps hardware, drivers and the developer server. */

static int s_upd_dev;                              /* 1 = the developer server is the source, 0 = the official one */
static lv_obj_t *s_upd_install_btn, *s_upd_src_btn;

static void upd_src_text(void)
{
    purr_repo_server_t sv;
    char buf[100];
    if (s_upd_dev && purr_devsrv_get(&sv) == 0) {
        snprintf(buf, sizeof(buf), "Source: %s", sv.name);
    } else {
        s_upd_dev = 0;
        snprintf(buf, sizeof(buf), "Source: the official server");
    }
    if (s_upd_src_btn != NULL) {
        cm_button_text(s_upd_src_btn, buf);
    }
}

static void upd_src_cb(lv_event_t *e)
{
    (void)e;
    s_upd_dev = !s_upd_dev;
    upd_src_text();
    if (s_upd_install_btn != NULL) {
        lv_obj_add_flag(s_upd_install_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static int upd_check_run(job_t *j)
{
    j->flag = purr_sysupdate_check(j->a[0] == 'd', j->msg, sizeof(j->msg));
    return j->flag >= 0 ? 0 : -1;
}

static void upd_check_done(job_t *j)
{
    say(j->msg, j->rc == 0);
    if (s_upd_install_btn != NULL) {
        if (j->flag == 1) {
            lv_obj_remove_flag(s_upd_install_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_upd_install_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void upd_check_cb(lv_event_t *e)
{
    (void)e;
    if (!acting_admin()) {
        say("Only an administrator can check for updates.", 0);
        return;
    }
    job_t *j = job_new(upd_check_run, upd_check_done);
    if (j == NULL) {
        say("Out of memory.", 0);
        return;
    }
    j->a[0] = s_upd_dev ? 'd' : 'o';
    j->stack = 10240;
    say("Checking...", 1);
    job_start(j);
}

static void upd_go(lv_event_t *e, int force, const char *again, const char *what)
{
    if (!acting_admin()) {
        say("Only an administrator can do that.", 0);
        return;
    }
    if (!confirm_tap(lv_event_get_target(e), again)) {
        return;
    }
    if (purr_sysupdate_request(s_upd_dev, force) != 0) {
        say("Could not ask for that.", 0);
        return;
    }
    say(what, 1);
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

static void upd_install_cb(lv_event_t *e)
{
    if (purr_sysupdate_includes_bootloader()) {
        upd_go(e, 0, "Tap again: includes the bootloader", "Restarting into the updater...");
    } else {
        upd_go(e, 0, "Tap again to restart and update", "Restarting into the updater...");
    }
}

static void upd_reinstall_cb(lv_event_t *e)
{
    upd_go(e, 1, "Tap again: reinstall (files are kept)", "Restarting to reinstall...");
}

/* Back to the compiled default address, after a signed move (plan8.md). */
static void upd_default_cb(lv_event_t *e)
{
    (void)e;
    if (!acting_admin()) {
        say("Only an administrator can do that.", 0);
        return;
    }
    purr_recovery_move_clear();
    say("Updates come from the default address again. Restart this page to see it.", 1);
}

static void about_system_section(void)
{
    char kv[16], ov[16], buf[96];
    purr_sysupdate_versions(kv, ov);
    cm_space(s_body, 4);
    cm_title(s_body, "System update");
    snprintf(buf, sizeof(buf), PURR_BRAND_PRODUCT " %s, " PURR_BRAND_RECOVERY " %s", kv, ov);
    cm_text(s_body, buf);
    {
        char scheme[8], host[96], path[96];
        uint16_t port;
        if (purr_repo_url_split(purr_recovery_official_manifest_url(), scheme, sizeof(scheme), host, sizeof(host), &port, path, sizeof(path)) == 0) {
            snprintf(buf, sizeof(buf), "Updates come from %.60s", host);
            cm_dim(s_body, buf);
        }
        if (purr_recovery_official_moved()) {
            snprintf(buf, sizeof(buf), "Moved by a signed message from the project (number %u).", (unsigned)purr_recovery_move_seq());
            cm_dim(s_body, buf);
            lv_obj_t *db = cm_button(s_body, "Use the default address", upd_default_cb, NULL);
            lv_obj_set_width(db, LV_PCT(100));
        }
    }
    s_upd_src_btn = NULL;
    purr_repo_server_t sv;
    if (purr_security_dev_active() && purr_devsrv_get(&sv) == 0) {
        s_upd_src_btn = cm_button(s_body, "", upd_src_cb, NULL);
        lv_obj_set_width(s_upd_src_btn, LV_PCT(100));
    } else {
        s_upd_dev = 0;
    }
    upd_src_text();
    lv_obj_t *cb = cm_button(s_body, "Check for update", upd_check_cb, NULL);
    lv_obj_set_width(cb, LV_PCT(100));
    s_upd_install_btn = cm_button(s_body, "Install update", upd_install_cb, NULL);
    lv_obj_set_width(s_upd_install_btn, LV_PCT(100));
    lv_obj_add_flag(s_upd_install_btn, LV_OBJ_FLAG_HIDDEN);
    make_status();

    cm_space(s_body, 4);
    cm_title(s_body, "Reinstall");
    cm_dim(s_body, "Puts " PURR_BRAND_PRODUCT " back from the source above, kernel and " PURR_BRAND_RECOVERY " both, even if they look fine. Your files and accounts are kept.");
    lv_obj_t *rb = cm_button(s_body, "Reinstall " PURR_BRAND_PRODUCT, upd_reinstall_cb, NULL);
    lv_obj_set_width(rb, LV_PCT(100));
}

static void about_reset_dismiss_cb(lv_event_t *e)
{
    (void)e;
    purr_resetnote_dismiss();
    say("Dismissed. Open this page again to refresh it.", 1);
}

static void page_about(void)
{
    char buf[96];
    cm_title(s_body, PURR_BRAND_PRODUCT);
    cm_text(s_body, purr_system_name());
    {
        char kv[16], ov[16];
        purr_sysupdate_versions(kv, ov);
        snprintf(buf, sizeof(buf), "Version %s, recovery %s", kv, ov);        /* which build this is: it was not shown anywhere before */
        cm_text(s_body, buf);
        char why[400];
        if (purr_resetnote_pending(NULL, 0, why, sizeof(why))) {
            cm_space(s_body, 4);
            cm_title(s_body, "Your data was reset");
            lv_obj_t *l = cm_text(s_body, why);
            lv_obj_set_style_text_color(l, lv_color_hex(0xFFB900), 0);
            lv_obj_t *gb = cm_button(s_body, "Got it", about_reset_dismiss_cb, NULL);
            lv_obj_set_width(gb, LV_PCT(100));
        }
    }

    purr_secure_status_t st;
    purr_secure_status(&st);
    cm_space(s_body, 4);
    cm_title(s_body, "Security");
    snprintf(buf, sizeof(buf), "State: %s", purr_secure_level_name(st.level));
    cm_text(s_body, buf);
    if (st.slot_verified) {
        purr_signer_t who;
        purr_secure_signer(st.key_id, &who);
        cm_text(s_body, "System: verified");
        if (who.known) {
            snprintf(buf, sizeof(buf), "Signed by: %s (key %u)%s", purr_role_label(who.role), (unsigned)who.key_id,
                     who.revoked ? ", REVOKED" : "");
        } else {
            snprintf(buf, sizeof(buf), "Signed by: key %u (not in this device's key list)", (unsigned)st.key_id);
        }
        lv_obj_t *l = cm_text(s_body, buf);
        if (who.revoked || !who.known) {
            lv_obj_set_style_text_color(l, lv_color_hex(0xFFB900), 0);
        }
    } else {
        lv_obj_t *l = cm_text(s_body, st.have_handoff ? "System: NOT verified (the signature did not check out)"
                                                      : "System: NOT verified (no boot record: not started by the PURR bootloader)");
        lv_obj_set_style_text_color(l, lv_color_hex(0xFFB900), 0);
    }
    snprintf(buf, sizeof(buf), "Boot policy: %s", !st.have_handoff ? "unknown"
             : st.mode_at_boot == PURR_SECURE_ENFORCE ? "enforce (unsigned images are refused)" : "warn (unsigned images boot with a warning)");
    cm_dim(s_body, buf);
    if (st.dev_at_boot || st.dev_now) {
        cm_dim(s_body, "Developer mode is on.");
    }
    switch (esp_reset_reason()) {
    case ESP_RST_PANIC:     snprintf(buf, sizeof(buf), "Last start: after a crash"); break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       snprintf(buf, sizeof(buf), "Last start: after a freeze (watchdog)"); break;
    case ESP_RST_BROWNOUT:  snprintf(buf, sizeof(buf), "Last start: power dipped (brownout)"); break;
    case ESP_RST_SW:        snprintf(buf, sizeof(buf), "Last start: restarted by the system"); break;
    case ESP_RST_POWERON:   snprintf(buf, sizeof(buf), "Last start: power on"); break;
    default:                snprintf(buf, sizeof(buf), "Last start: other (%d)", (int)esp_reset_reason()); break;
    }
    cm_dim(s_body, buf);

    cm_space(s_body, 4);
    cm_title(s_body, "Device");
    snprintf(buf, sizeof(buf), "Signed in as %s", purr_login_current()->name);
    cm_text(s_body, buf);
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    snprintf(buf, sizeof(buf), "Up for %u h %02u min", (unsigned)(up / 3600), (unsigned)((up / 60) % 60));
    cm_text(s_body, buf);
    snprintf(buf, sizeof(buf), "Memory free: %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    cm_text(s_body, buf);
    snprintf(buf, sizeof(buf), "Screen: %dx%d", w10m_disp_w(), w10m_disp_h());
    cm_text(s_body, buf);
    about_system_section();
    cm_space(s_body, 8);
}

/* ---------------------------------------------------------------- pages */

static void group_row_cb(lv_event_t *e)
{
    show_page((page_t)(intptr_t)lv_event_get_user_data(e));
}

/* The two list pages of the Android layout: rows that open the pages the Windows layout reaches from the main list. */
static void page_group(page_t group)
{
    if (group == PAGE_GROUP_DISPLAY) {
        cm_row(s_body, "Display", "Brightness, screen timeout, rotation, interface", 1, group_row_cb, (void *)(intptr_t)PAGE_SYSTEM);
        cm_row(s_body, "Appearance", "Colours, background and wallpaper", 1, group_row_cb, (void *)(intptr_t)PAGE_PERSONAL);
    } else {
        cm_row(s_body, "Manage applications", "Installed apps, install", 1, group_row_cb, (void *)(intptr_t)PAGE_APPS);
        cm_row(s_body, "Development", "Developer mode, unsigned apps, the Developer Portal", 1, group_row_cb, (void *)(intptr_t)PAGE_DEVELOPER);
    }
}

static const char *page_title(page_t p)
{
    if (purr_theme_skin() == PURR_SKIN_ANDROID) {
        switch (p) {
        case PAGE_SYSTEM:        return "Display";
        case PAGE_DEVICES:       return "Hardware";
        case PAGE_NETWORK:       return "Wireless & networks";
        case PAGE_PERSONAL:      return "Appearance";
        case PAGE_APPS:          return "Manage applications";
        case PAGE_ACCOUNTS:      return "Accounts & sync";
        case PAGE_TIME:          return "Date & time";
        case PAGE_SECURITY:      return "Location & security";
        case PAGE_DEVELOPER:     return "Development";
        case PAGE_ABOUT:         return "About phone";
        case PAGE_GROUP_DISPLAY: return "Sound & display";
        case PAGE_GROUP_APPS:    return "Applications";
        default:                 break;
        }
    }
    switch (p) {
    case PAGE_SYSTEM:    return "System";
    case PAGE_DEVICES:   return "Devices";
    case PAGE_NETWORK:   return "Network";
    case PAGE_PERSONAL:  return "Personalization";
    case PAGE_APPS:      return "Apps";
    case PAGE_ACCOUNTS:  return "Accounts";
    case PAGE_ACCT_USER: return "Account";
    case PAGE_ACCT_ADD:  return "Add an account";
    case PAGE_TIME:      return "Time & Language";
    case PAGE_SECURITY:  return "Privacy & security";
    case PAGE_DEVELOPER: return "Developer options";
    case PAGE_ABOUT:     return "About";
    default:             return "Settings";
    }
}

static page_t page_parent(page_t p)
{
    if (purr_theme_skin() == PURR_SKIN_ANDROID) {
        if (p == PAGE_SYSTEM || p == PAGE_PERSONAL) {
            return PAGE_GROUP_DISPLAY;
        }
        if (p == PAGE_APPS || p == PAGE_DEVELOPER) {
            return PAGE_GROUP_APPS;
        }
    }
    if (p == PAGE_DEVELOPER) {
        return PAGE_SECURITY;
    }
    return (p == PAGE_ACCT_USER || p == PAGE_ACCT_ADD) ? PAGE_ACCOUNTS : PAGE_MAIN;
}

static void show_page(page_t page)
{
    s_page = page;
    s_gen++;                            /* results still on their way for the old page are dropped */
    s_armed = NULL;
    s_status = NULL;
    s_net_state = s_net_list = s_net_form = s_net_ssid = s_net_pass = NULL;
    s_app_list = s_app_src = NULL;
    s_kbl_btn = NULL;
    s_ntp_line = NULL;
    s_start_msg = NULL;
    s_time_now = NULL;
    s_acct_auto_sw = s_acct_auto_pw = NULL;
    s_sec_state = s_sec_boot = s_sec_trust = s_sec_slot = s_sec_active = NULL;
    s_sec_enforce_sw = s_sec_unsigned_sw = s_sec_dev_btn = s_sec_warn = NULL;
    lv_group_remove_all_objs(s_group);
    lv_obj_clean(s_body);
    lv_obj_scroll_to(s_body, 0, 0, LV_ANIM_OFF);
    lv_label_set_text(s_head_title, page_title(page));
    switch (page) {
    case PAGE_SYSTEM:    page_system(); break;
    case PAGE_DEVICES:   page_devices(); break;
    case PAGE_NETWORK:   page_network(); break;
    case PAGE_PERSONAL:  page_personal(); break;
    case PAGE_APPS:      page_apps(); break;
    case PAGE_ACCOUNTS:  page_accounts(); break;
    case PAGE_ACCT_USER: page_acct_user(); break;
    case PAGE_ACCT_ADD:  page_acct_add(); break;
    case PAGE_TIME:      page_time(); break;
    case PAGE_SECURITY:  page_security(); break;
    case PAGE_DEVELOPER: page_developer(); break;
    case PAGE_ABOUT:     page_about(); break;
    case PAGE_GROUP_DISPLAY:
    case PAGE_GROUP_APPS: page_group(page); break;
    default:             page_main(); break;
    }
}

/* ---------------------------------------------------------------- the sheet */

static void x_cb(void *obj, int32_t v) { lv_obj_set_x((lv_obj_t *)obj, v); }

static void anim_done(lv_anim_t *a)
{
    (void)a;
    if (!s_open) {
        lv_obj_add_flag(s_set, LV_OBJ_FLAG_HIDDEN);
    }
}

static void move(int open)
{
    s_open = open;
    lv_indev_t *kb = purr_lvgl_keypad();
    if (open) {
        lv_obj_remove_flag(s_set, LV_OBJ_FLAG_HIDDEN);
        show_page(PAGE_MAIN);
        if (kb != NULL) {
            lv_indev_set_group(kb, s_group);            /* text boxes take the hardware keyboard */
        }
    } else {
        s_gen++;
        if (kb != NULL) {
            lv_indev_set_group(kb, NULL);
        }
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_set);
    lv_anim_set_exec_cb(&a, x_cb);
    lv_anim_set_values(&a, lv_obj_get_x(s_set), open ? 0 : w10m_disp_w());
    lv_anim_set_duration(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, anim_done);
    lv_anim_start(&a);
}

static void back_clicked(lv_event_t *e)
{
    (void)e;
    if (s_page == PAGE_MAIN) {
        move(0);
    } else {
        show_page(page_parent(s_page));
    }
}

void w10m_settings_open(void)
{
    if (s_set != NULL && !s_open) {
        move(1);
    }
}

int w10m_settings_is_open(void) { return s_open; }

void w10m_settings_back(void)
{
    if (s_set != NULL && s_open) {
        back_clicked(NULL);
    }
}

void w10m_settings_layout(void)
{
    if (s_set == NULL) {
        return;
    }
    int w = w10m_disp_w(), h = w10m_disp_h();
    lv_obj_set_size(s_set, w, h - W10M_STATUS_H - W10M_NAV_H);
    lv_obj_set_pos(s_set, s_open ? 0 : w, W10M_STATUS_H);
    lv_obj_set_size(s_head, w, HEAD_H);
    lv_obj_set_size(s_body, w, h - W10M_STATUS_H - W10M_NAV_H - HEAD_H);
    lv_obj_align(s_body, LV_ALIGN_TOP_LEFT, 0, HEAD_H);
    if (!s_open) {
        lv_obj_add_flag(s_set, LV_OBJ_FLAG_HIDDEN);
    }
}

void w10m_settings_build(void)
{
    s_group = lv_group_create();
    s_set = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_set);
    purr_theme_apply(s_set, PURR_TH_SHEET);
    lv_obj_clear_flag(s_set, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_set, LV_OBJ_FLAG_CLICKABLE);      /* it swallows touches meant for what is beneath */
    lv_obj_add_flag(s_set, LV_OBJ_FLAG_HIDDEN);

    s_head = lv_obj_create(s_set);
    lv_obj_remove_style_all(s_head);
    lv_obj_add_flag(s_head, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_head, back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *arrow = lv_label_create(s_head);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    purr_theme_apply(arrow, PURR_TH_TEXT);
    lv_obj_align(arrow, LV_ALIGN_LEFT_MID, 8, 0);
    s_head_title = lv_label_create(s_head);
    lv_label_set_text(s_head_title, "Settings");
    lv_obj_set_style_text_font(s_head_title, &lv_font_montserrat_16, 0);
    purr_theme_apply(s_head_title, PURR_TH_TEXT);
    lv_obj_align(s_head_title, LV_ALIGN_LEFT_MID, 34, 0);
    purr_theme_title_bar(s_head, arrow, s_head_title);

    s_body = lv_obj_create(s_set);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_body, 4, 0);
    lv_obj_set_style_pad_hor(s_body, 10, 0);
    lv_obj_set_scrollbar_mode(s_body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_body, LV_DIR_VER);
    w10m_settings_layout();
    (void)TAG;
}
