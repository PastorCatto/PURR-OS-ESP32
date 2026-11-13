/*
 * purr_tp_home.c - the Type shell's home screen, its app list and its menus (plan7.md, "The Type shell").
 *
 * Home: the clock big, the date, the newest notification in an accent bar, and four tiles under them. The first tile is the app list. The other three are
 * shortcuts the user maps to whatever they like: Weather and Friends (planned features, they say so when opened), Music, or any installed app. A tile is
 * chosen with the d-pad or touched; FUNC held plus A, B, X or Y opens tile 1 to 4 straight away. The mapping is the settings home.tile1 to home.tile3 (a name:
 * "weather", "friends", "music", "none" or an app's name) and is edited from the quick menu (X), under Shortcuts.
 *
 * One generic list screen (in the iPod's way: a title in the strip, a row for each choice with a chevron, the chosen row filled, a scroll bar, and the clock in
 * a side panel) serves the app list, the quick menu, the alerts and the shortcut pickers. Everything is a clickable box, so touch and d-pad work alike.
 *
 * The apps are listed exactly as the Android shell lists them (Settings first, then installed apps that verify, a DEV mark on unsigned developer apps, the
 * Developer Portal in developer mode) and started the same way, through the app runtime.
 */
#define PURR_SHELL_IMPL_ANDROID
#include "purr_shell_names.h"
#include "purr_tp_priv.h"

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
#include "purr_appmgr.h"
#include "purr_battery.h"
#include "purr_net.h"
#include "purr_catrt.h"
#include "purr_lvgl.h"
#include "purr_settings.h"
#include "purr_ui_service.h"

static const char *TAG = "type-home";

#define MAX_APPS  40
#define MAX_ROWS  48
#define TILES     4

#define PAD(n) (1 << (n))
#define HINT_HOME  (PAD(PURR_PAD_LEFT) | PAD(PURR_PAD_RIGHT) | PAD(PURR_PAD_A) | PAD(PURR_PAD_X))
#define HINT_LIST  (PAD(PURR_PAD_UP) | PAD(PURR_PAD_DOWN) | PAD(PURR_PAD_A) | PAD(PURR_PAD_B))

/* ---------------------------------------------------------------- the app list */

typedef struct {
    char name[PURR_APP_NAME_LEN];
    uint8_t system;                     /* 1 = Settings, 2 = the Developer Portal */
    uint8_t dev;                        /* a developer app: shown with a DEV mark */
} app_t;

static app_t s_apps[MAX_APPS];
static int s_napps;

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
        if (!a->chip_ok || !(a->verified || purr_security_unsigned_apps_allowed()) || strcmp(a->name, "Settings") == 0 || strcmp(a->name, "storesvc") == 0) {
            continue;                                   /* storesvc is the Store's background companion, not something to open */
        }
        d = &s_apps[s_napps++];
        memset(d, 0, sizeof(*d));
        snprintf(d->name, sizeof(d->name), "%s", a->name);
        d->dev = !a->verified;
    }
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

/* The picture for an app: fr_<name in lower case letters and digits>, with a few apps sharing another's, or the generic app icon (the same rule the Android
 * shell uses, so the one icon pack serves both). */
static const char *icon_for(const char *name, int system, char *buf, size_t cap)
{
    if (system == 1) {
        return "fr_settings";
    }
    if (system == 2) {
        return "fr_app";
    }
    static const struct { const char *app, *icon; } alias[] = {
        {"files", "fr_folder"}, {"devices", "fr_settings"}, {"notes", "fr_app"}, {"store", "fr_app"},
        {"photos", "fr_gallery"}, {"gallery", "fr_gallery"}, {"messages", "fr_messaging"}, {"sms", "fr_messaging"},
    };
    size_t n = (size_t)snprintf(buf, cap, "fr_");
    for (const char *s = name; *s != '\0' && n + 1 < cap; s++) {
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

const lv_image_dsc_t *and_app_icon(const char *app)
{
    char buf[40];
    const lv_image_dsc_t *d = w10m_img(icon_for(app, 0, buf, sizeof(buf)));
    return d != NULL ? d : w10m_img("fr_app");
}

static int find_app(const char *name)
{
    for (int i = 0; i < s_napps; i++) {
        if (strcasecmp(s_apps[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------- starting an app */

typedef struct {
    char name[PURR_APP_NAME_LEN];
} open_job_t;

static void open_task(void *arg)
{
    open_job_t *j = arg;
    char line[PURR_APP_NAME_LEN + 8], out[160];
    snprintf(line, sizeof(line), "%s %s", purr_catrt_slot_of(j->name) >= 0 ? "focus" : "open", j->name);     /* a running app comes to the front */
    out[0] = '\0';
    int rc = purr_catrt_dispatch(line, out, sizeof(out));
    if (rc != 0) {
        char msg[200];
        snprintf(msg, sizeof(msg), "%s", out[0] ? out : "could not start");
        purr_tp_notify(j->name, msg);
    }
    free(j);
    vTaskDelete(NULL);
}

static void list_close(void);

void tp_open_app(const char *name, int system)
{
    list_close();
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
    if (xTaskCreate(open_task, "tp-open", 4096, j, 4, NULL) != pdPASS) {
        free(j);
        w10m_banner("Not enough memory to open the app", 0);
    }
}

/* ---------------------------------------------------------------- the generic list screen */

typedef struct {
    char label[PURR_APP_NAME_LEN];
    char sub[20];
    char icon[PURR_APP_NAME_LEN];       /* the name of an app whose icon the row shows; empty for none */
    int system;
    int tag;
} lrow_t;

static struct {
    lv_obj_t *root, *list, *panel, *clk_h, *clk_m, *clk_d, *foot, *bat_lbl, *bat_bar, *bat_fill, *wifi_lbl;
    lrow_t rows[MAX_ROWS];
    int n;
    int clock;                          /* 1: the clock in a side panel */
    void (*pick)(int tag);
    char title[16];
    int open;
} s_ls;

static void clock_refresh(void);

static void list_close(void)
{
    if (!s_ls.open) {
        return;
    }
    tp_focus_clear();
    lv_obj_delete(s_ls.root);
    memset(&s_ls, 0, sizeof(s_ls));
    tp_strip_title(NULL);
    tp_home_focus();
}

static void row_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_ls.n) {
        return;
    }
    void (*pick)(int) = s_ls.pick;
    int tag = s_ls.rows[i].tag;
    if (pick != NULL) {
        pick(tag);                      /* it may close this list, or open another in its place */
    }
}

static void list_layout(void)
{
    if (!s_ls.open) {
        return;
    }
    int w = w10m_disp_w(), h = w10m_disp_h() - TP_STRIP_H;
    int pw = s_ls.clock ? (w > 260 ? 110 : 0) : 0;
    int bh = h - TP_FOOT_H;                                                    /* the body: the footer takes the bottom */
    lv_obj_set_pos(s_ls.root, 0, TP_STRIP_H);
    lv_obj_set_size(s_ls.root, w, h);
    lv_obj_set_pos(s_ls.list, 0, 0);
    lv_obj_set_size(s_ls.list, w - pw, bh);
    if (s_ls.panel != NULL) {
        lv_obj_set_pos(s_ls.panel, w - pw, 0);
        lv_obj_set_size(s_ls.panel, pw, bh);
        if (s_ls.bat_lbl != NULL) {
            lv_obj_align(s_ls.bat_lbl, LV_ALIGN_BOTTOM_LEFT, 8, -34);
            lv_obj_align(s_ls.bat_bar, LV_ALIGN_BOTTOM_LEFT, 8, -22);
            lv_obj_align(s_ls.wifi_lbl, LV_ALIGN_BOTTOM_LEFT, 8, -6);
        }
    }
    if (s_ls.foot != NULL) {
        lv_obj_set_pos(s_ls.foot, 0, bh);
        lv_obj_set_width(s_ls.foot, w);
    }
}

/* Opens a list screen with `n` rows; picking one calls pick(tag). The focus goes to the first row (or `focus_tag`). */
static void list_open(const char *title, const lrow_t *rows, int n, void (*pick)(int tag), int with_clock, int focus_tag)
{
    list_close();
    memset(&s_ls, 0, sizeof(s_ls));
    s_ls.n = n > MAX_ROWS ? MAX_ROWS : n;
    memcpy(s_ls.rows, rows, sizeof(lrow_t) * (size_t)s_ls.n);
    s_ls.pick = pick;
    s_ls.clock = with_clock;
    snprintf(s_ls.title, sizeof(s_ls.title), "%s", title);
    s_ls.open = 1;

    lv_obj_t *scr = lv_screen_active();
    s_ls.root = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ls.root);
    lv_obj_clear_flag(s_ls.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_ls.root, tp_paper(), 0);
    lv_obj_set_style_bg_opa(s_ls.root, LV_OPA_COVER, 0);

    s_ls.list = lv_obj_create(s_ls.root);
    lv_obj_remove_style_all(s_ls.list);
    lv_obj_set_flex_flow(s_ls.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_ls.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_ls.list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(s_ls.list, tp_ink(), LV_PART_SCROLLBAR);          /* the iPod's scroll bar: a thin ink bar at the edge */
    lv_obj_set_style_bg_opa(s_ls.list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_ls.list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_top(s_ls.list, 3, 0);

    lv_obj_t *first = NULL, *want = NULL;
    for (int i = 0; i < s_ls.n; i++) {
        lv_obj_t *b = tp_box(s_ls.list, TP_FILL_NONE);
        lv_obj_set_size(b, LV_PCT(100), 32);
        lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_pad_hor(b, 10, 0);
        static const lv_state_t chosen[2] = {LV_STATE_FOCUSED, LV_STATE_PRESSED};     /* the iPod's way: the chosen row is an ink bar with paper text, not the accent */
        for (int k = 0; k < 2; k++) {
            lv_obj_set_style_bg_color(b, tp_ink(), chosen[k]);
            lv_obj_set_style_text_color(b, tp_paper(), chosen[k]);
        }
        int ix = 0;
        if (s_ls.rows[i].icon[0] != '\0') {                /* the app's icon from the pack, at 20 pixels */
            char nb[40];
            const lv_image_dsc_t *ic = w10m_img(icon_for(s_ls.rows[i].icon, s_ls.rows[i].system, nb, sizeof(nb)));
            if (ic != NULL) {
                lv_obj_t *im = lv_image_create(b);
                lv_image_set_src(im, ic);
                lv_image_set_scale(im, 107);                /* 48 pixels at 107/256 */
                lv_obj_set_size(im, 20, 20);
                lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_align(im, LV_ALIGN_LEFT_MID, 0, 0);
                ix = 26;
            }
        }
        char shown[PURR_APP_NAME_LEN];                      /* "store" is shown as "Store" */
        snprintf(shown, sizeof(shown), "%s", s_ls.rows[i].label);
        if (shown[0] >= 'a' && shown[0] <= 'z') {
            shown[0] = (char)(shown[0] - 32);
        }
        lv_obj_t *l = tp_label(b, shown, &tp_font_bold_20, NULL);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        int row_w = w10m_disp_w() - ((with_clock && w10m_disp_w() > 260) ? 110 : 0);
        int lab_w = row_w - 20 - ix - (s_ls.rows[i].sub[0] != '\0' ? 76 : 28);        /* the row, less its padding, the icon, and the chevron (and the small text) */
        lv_obj_set_width(l, lab_w < 40 ? 40 : lab_w);                               /* exact pixels: a percentage minus pixels is not a width */
        lv_obj_align(l, LV_ALIGN_LEFT_MID, ix, 0);
        lv_obj_t *c = tp_label(b, LV_SYMBOL_RIGHT, &lv_font_montserrat_12, NULL);
        lv_obj_set_style_text_color(c, tp_accent(), LV_STATE_FOCUSED);        /* the chosen row's chevron is the accent, as in the design */
        lv_obj_set_style_text_color(c, tp_accent(), LV_STATE_PRESSED);
        lv_obj_align(c, LV_ALIGN_RIGHT_MID, 0, 0);
        if (s_ls.rows[i].sub[0] != '\0') {
            lv_obj_t *s = tp_label(b, s_ls.rows[i].sub, &tp_font_mono_12, NULL);
            lv_obj_align(s, LV_ALIGN_RIGHT_MID, -16, 0);
        }
        lv_obj_add_event_cb(b, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        tp_focus_add(b);
        if (first == NULL) {
            first = b;
        }
        if (s_ls.rows[i].tag == focus_tag) {
            want = b;
        }
    }
    if (with_clock && w10m_disp_w() > 260) {
        s_ls.panel = lv_obj_create(s_ls.root);
        lv_obj_remove_style_all(s_ls.panel);
        lv_obj_clear_flag(s_ls.panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(s_ls.panel, tp_accent(), 0);
        lv_obj_set_style_bg_opa(s_ls.panel, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_ls.panel, 1, 0);
        lv_obj_set_style_border_side(s_ls.panel, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(s_ls.panel, tp_ink(), 0);
        lv_obj_set_style_text_color(s_ls.panel, tp_on_accent(), 0);
        s_ls.clk_d = tp_label(s_ls.panel, "", &tp_font_mono_12, NULL);
        lv_obj_align(s_ls.clk_d, LV_ALIGN_TOP_LEFT, 8, 8);
        s_ls.clk_h = tp_label(s_ls.panel, "", &tp_font_clock_panel, NULL);
        lv_obj_align(s_ls.clk_h, LV_ALIGN_TOP_LEFT, 6, 28);
        s_ls.clk_m = tp_label(s_ls.panel, "", &tp_font_clock_panel, NULL);
        lv_obj_align(s_ls.clk_m, LV_ALIGN_TOP_LEFT, 6, 28 + 45 + 2);
        s_ls.bat_lbl = tp_label(s_ls.panel, "", &tp_font_mono_12, NULL);                    /* BATTERY 87%, its bar, WI-FI ON */
        s_ls.bat_bar = lv_obj_create(s_ls.panel);
        lv_obj_remove_style_all(s_ls.bat_bar);
        lv_obj_clear_flag(s_ls.bat_bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(s_ls.bat_bar, 94, 10);
        lv_obj_set_style_border_width(s_ls.bat_bar, 2, 0);
        lv_obj_set_style_border_color(s_ls.bat_bar, tp_ink(), 0);
        s_ls.bat_fill = lv_obj_create(s_ls.bat_bar);
        lv_obj_remove_style_all(s_ls.bat_fill);
        lv_obj_set_style_bg_color(s_ls.bat_fill, tp_ink(), 0);
        lv_obj_set_style_bg_opa(s_ls.bat_fill, LV_OPA_COVER, 0);
        lv_obj_set_size(s_ls.bat_fill, 1, 6);
        lv_obj_align(s_ls.bat_fill, LV_ALIGN_LEFT_MID, 0, 0);
        s_ls.wifi_lbl = tp_label(s_ls.panel, "", &tp_font_mono_12, NULL);
    }
    {
        static const tp_hint_t keys[] = {{LV_SYMBOL_UP LV_SYMBOL_DOWN, "SCROLL"}, {"A", "OPEN"}, {"B", "BACK"}, {"X", "MENU"}};
        s_ls.foot = tp_footer(s_ls.root, keys, 4);
    }
    list_layout();
    clock_refresh();
    lv_obj_move_foreground(s_ls.root);
    tp_focus_set(want != NULL ? want : first);
    tp_strip_title(title);
    tp_pad_hint(HINT_LIST);
}

/* ---------------------------------------------------------------- the home screen */

static lv_obj_t *s_home, *s_clock_big, *s_date, *s_notice, *s_notice_lbl, *s_notice_cnt, *s_hint;
static lv_obj_t *s_tile[TILES];
static lv_timer_t *s_tick;

typedef struct {
    char name[12], big[8], sub[16];
} tile_face_t;

static const char *const k_tile_key[TILES] = {"A", "B", "X", "Y"};
static const char *const k_tile_default[TILES] = {"apps", "weather", "music", "friends"};

static void tile_setting(int idx, char *out, size_t cap)
{
    char key[16];
    snprintf(key, sizeof(key), "home.tile%d", idx);
    if (idx == 0 || purr_settings_kv_get(key, out, cap) != 0 || out[0] == '\0') {
        snprintf(out, cap, "%s", k_tile_default[idx]);
    }
}

static void upper_copy(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < cap; i++) {
        dst[i] = (char)toupper((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

static void tile_face(int idx, tile_face_t *f)
{
    char id[PURR_APP_NAME_LEN];
    memset(f, 0, sizeof(*f));
    tile_setting(idx, id, sizeof(id));
    if (idx == 0 || strcmp(id, "apps") == 0) {
        snprintf(f->name, sizeof(f->name), "APPS");
        snprintf(f->big, sizeof(f->big), LV_SYMBOL_LIST);
        snprintf(f->sub, sizeof(f->sub), "%d APPS", s_napps);
    } else if (strcmp(id, "weather") == 0) {
        snprintf(f->name, sizeof(f->name), "WEATHER");
        snprintf(f->big, sizeof(f->big), "--");
        snprintf(f->sub, sizeof(f->sub), "SOON");
    } else if (strcmp(id, "friends") == 0) {
        snprintf(f->name, sizeof(f->name), "FRIENDS");
        snprintf(f->big, sizeof(f->big), "--");
        snprintf(f->sub, sizeof(f->sub), "SOON");
    } else if (strcmp(id, "music") == 0) {
        snprintf(f->name, sizeof(f->name), "MUSIC");
        snprintf(f->big, sizeof(f->big), LV_SYMBOL_PLAY);
        snprintf(f->sub, sizeof(f->sub), find_app("Music") >= 0 ? "OPEN" : "NONE");
    } else if (strcmp(id, "none") == 0) {
        snprintf(f->name, sizeof(f->name), "EMPTY");
        snprintf(f->big, sizeof(f->big), "+");
        snprintf(f->sub, sizeof(f->sub), "CHOOSE");
    } else {
        upper_copy(f->name, sizeof(f->name), id);
        f->big[0] = (char)toupper((unsigned char)id[0]);
        f->big[1] = '\0';
        snprintf(f->sub, sizeof(f->sub), find_app(id) >= 0 ? "APP" : "MISSING");
    }
}

static void shortcut_picker(int idx);
static void menu_open(void);

static void tile_activate(int idx)
{
    char id[PURR_APP_NAME_LEN];
    tile_setting(idx, id, sizeof(id));
    if (idx == 0 || strcmp(id, "apps") == 0) {
        tp_drawer_open();
    } else if (strcmp(id, "weather") == 0 || strcmp(id, "friends") == 0) {
        w10m_banner("Coming soon. It is not in this version yet.", 0);
    } else if (strcmp(id, "music") == 0) {
        int a = find_app("Music");
        if (a >= 0) {
            tp_open_app(s_apps[a].name, s_apps[a].system);
        } else {
            w10m_banner("No music app is installed. Get one from the Store.", 0);
        }
    } else if (strcmp(id, "none") == 0) {
        shortcut_picker(idx);
    } else {
        int a = find_app(id);
        if (a >= 0) {
            tp_open_app(s_apps[a].name, s_apps[a].system);
        } else {
            w10m_banner("That app is not installed any more. Choose another in the menu.", 1);
        }
    }
}

static void tile_clicked(lv_event_t *e)
{
    tile_activate((int)(intptr_t)lv_event_get_user_data(e));
}

static void notice_clicked(lv_event_t *e);

static void home_fill(void)
{
    if (!s_ls.open) {
        tp_focus_clear();                               /* the focus list holds the tiles about to be deleted */
    }
    for (int i = 0; i < TILES; i++) {
        if (s_tile[i] != NULL) {
            lv_obj_delete(s_tile[i]);
            s_tile[i] = NULL;
        }
    }
    for (int i = 0; i < TILES; i++) {
        tile_face_t f;
        tile_face(i, &f);
        lv_obj_t *t = tp_box(s_home, TP_FILL_NONE);
        s_tile[i] = t;
        lv_obj_set_style_border_width(t, 2, 0);
        lv_obj_set_style_translate_x(t, -2, LV_STATE_FOCUSED);        /* the chosen tile lifts: it moves up and left and throws a hard shadow, as in the design */
        lv_obj_set_style_translate_y(t, -2, LV_STATE_FOCUSED);
        lv_obj_set_style_shadow_width(t, 1, LV_STATE_FOCUSED);
        lv_obj_set_style_shadow_spread(t, 1, LV_STATE_FOCUSED);
        lv_obj_set_style_shadow_offset_x(t, 4, LV_STATE_FOCUSED);
        lv_obj_set_style_shadow_offset_y(t, 4, LV_STATE_FOCUSED);
        lv_obj_set_style_shadow_color(t, tp_ink(), LV_STATE_FOCUSED);
        lv_obj_set_style_shadow_opa(t, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_t *n = tp_label(t, f.name, &tp_font_mono_12, NULL);
        lv_label_set_long_mode(n, LV_LABEL_LONG_CLIP);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, 4, 3);
        lv_obj_t *k = lv_obj_create(t);                 /* the button that opens it with FUNC held */
        lv_obj_remove_style_all(k);
        lv_obj_clear_flag(k, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(k, 14, 14);
        lv_obj_set_style_bg_color(k, tp_ink(), 0);
        lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
        lv_obj_align(k, LV_ALIGN_BOTTOM_RIGHT, -3, -3);            /* bottom right: the name has the top row to itself */
        lv_obj_t *kl = tp_label(k, k_tile_key[i], &tp_font_mono_12, NULL);
        lv_obj_set_style_text_color(kl, tp_paper(), 0);
        lv_obj_center(kl);
        int sym = (unsigned char)f.big[0] >= 0x80;           /* a symbol is drawn from the symbol font */
        lv_obj_t *b = tp_label(t, f.big, sym ? &lv_font_montserrat_20 : &tp_font_bold_20, NULL);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, 4, 19);           /* between the name and the small line, which has the bottom to itself */
        lv_obj_t *s = tp_label(t, f.sub, &tp_font_mono_12, NULL);
        lv_label_set_long_mode(s, LV_LABEL_LONG_CLIP);
        lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, 4, -3);
        lv_obj_add_event_cb(t, tile_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    /* the notice: the newest notification in an accent bar */
    char title[32], text[96], line[140];
    int n = tp_note_count();
    if (n > 0 && tp_note_peek(0, title, sizeof(title), text, sizeof(text)) == 0) {
        snprintf(line, sizeof(line), "%s  %s", title, text);
    } else {
        snprintf(line, sizeof(line), "NOTHING NEW");
    }
    lv_label_set_text(s_notice_lbl, line);
    char cnt[8];
    snprintf(cnt, sizeof(cnt), "%d", n);
    lv_label_set_text(s_notice_cnt, n > 0 ? cnt : "");
    lv_obj_set_style_bg_color(s_notice, n > 0 ? tp_accent() : lv_color_hex(TP_SAND), 0);
    lv_obj_set_style_text_color(s_notice, n > 0 ? tp_on_accent() : tp_ink(), 0);
    lv_obj_set_style_bg_color(s_notice, tp_accent(), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(s_notice, tp_on_accent(), LV_STATE_FOCUSED);
}

void tp_home_focus(void)
{
    if (s_home == NULL || s_ls.open) {
        return;
    }
    tp_focus_clear();
    for (int i = 0; i < TILES; i++) {
        tp_focus_add(s_tile[i]);
    }
    tp_focus_add(s_notice);
    tp_focus_set(s_tile[0]);
    tp_pad_hint(HINT_HOME);
}

void tp_home_layout(void)
{
    if (s_home == NULL) {
        return;
    }
    int w = w10m_disp_w(), h = w10m_disp_h() - TP_STRIP_H;
    lv_obj_set_pos(s_home, 0, TP_STRIP_H);
    lv_obj_set_size(s_home, w, h);
    int m = 12;
    lv_obj_set_pos(s_clock_big, m - 2, 4);               /* the big clock, set in the bold face at its own size */
    int tile_h = 62;
    int ty = h - TP_FOOT_H - tile_h - 9;                    /* a margin under the tiles for the chosen one's shadow */
    int ny = ty - 6 - 20;
    lv_obj_set_pos(s_notice, m, ny);
    lv_obj_set_size(s_notice, w - 2 * m, 20);
    int gap = 6, total = w - 2 * m - 3 * gap;
    int unit = total * 10 / 45;                          /* the first tile is one and a half units */
    int first = total - 3 * unit;
    int x = m;
    for (int i = 0; i < TILES; i++) {
        int tw = i == 0 ? first : unit;
        if (s_tile[i] != NULL) {
            lv_obj_set_pos(s_tile[i], x, ty);
            lv_obj_set_size(s_tile[i], tw, tile_h);
        }
        x += tw + gap;
    }
    lv_obj_set_pos(s_hint, 0, h - TP_FOOT_H);
    lv_obj_set_width(s_hint, w);
    list_layout();
}

static void clock_refresh(void)
{
    char tb[16], db[24];
    tp_clock_strings(tb, sizeof(tb), db, sizeof(db));
    if (s_clock_big != NULL) {
        lv_label_set_text(s_clock_big, tb);
    }
    if (s_ls.open && s_ls.clk_h != NULL) {
        char hh[4] = {tb[0], tb[1], 0, 0}, mm[4] = {0, 0, 0, 0};
        if (tb[2] == ':') {
            mm[0] = tb[3];
            mm[1] = tb[4];
        } else {
            hh[0] = '-';
            hh[1] = '-';
            mm[0] = '-';
            mm[1] = '-';
        }
        lv_label_set_text(s_ls.clk_h, hh);
        lv_label_set_text(s_ls.clk_m, mm);
        lv_label_set_text(s_ls.clk_d, db);
        if (s_ls.bat_lbl != NULL) {
            int pct;
            char bl[24];
            int have = purr_battery_get(&pct, NULL);          /* no gauge (Playtime today): no battery block, only the Wi-Fi line */
            if (have) {
                snprintf(bl, sizeof(bl), "BATTERY %d%%", pct);
                lv_obj_set_width(s_ls.bat_fill, (lv_coord_t)(pct * 90 / 100 < 1 ? 1 : pct * 90 / 100));
                lv_obj_remove_flag(s_ls.bat_bar, LV_OBJ_FLAG_HIDDEN);
            } else {
                bl[0] = '\0';
                lv_obj_add_flag(s_ls.bat_bar, LV_OBJ_FLAG_HIDDEN);
            }
            lv_label_set_text(s_ls.bat_lbl, bl);
            purr_net_status_t ns;
            purr_net_status(&ns);
            lv_label_set_text(s_ls.wifi_lbl, ns.connected ? "WI-FI ON" : "WI-FI OFF");
        }
    }
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    clock_refresh();
}

void tp_home_reload(void)
{
    if (s_home == NULL) {
        return;
    }
    load_apps();
    home_fill();
    tp_home_layout();
    if (!s_ls.open) {
        tp_home_focus();
    }
}

void tp_home_reset(void)
{
    list_close();
    tp_home_focus();
}

int tp_home_back(void)
{
    if (s_ls.open) {
        list_close();
        return 1;
    }
    return 0;
}

int tp_drawer_is_open(void)
{
    return s_ls.open;
}

int tp_home_showing(void)
{
    return s_home != NULL && !s_ls.open && !tp_sheet_open();
}

int tp_now_playing(char *title, size_t tcap, char *artist, size_t acap)
{
    (void)title;
    (void)tcap;
    (void)artist;
    (void)acap;
    return 0;                                           /* no music app reports what it plays yet; when one does, the cover takes the clock's place */
}

int tp_home_key(uint32_t key)
{
    if (s_home == NULL || tp_sheet_open()) {
        return 0;
    }
    return tp_focus_key(key);
}

int tp_home_chord(int button)
{
    int idx = button == PURR_PAD_A ? 0 : button == PURR_PAD_B ? 1 : button == PURR_PAD_X ? 2 : button == PURR_PAD_Y ? 3 : -1;
    if (idx < 0 || s_home == NULL) {
        return 0;
    }
    tile_activate(idx);
    return 1;
}

/* ---------------------------------------------------------------- the apps list */

static void apps_picked(int tag)
{
    if (tag >= 0 && tag < s_napps) {
        tp_open_app(s_apps[tag].name, s_apps[tag].system);
    }
}

void tp_drawer_open(void)
{
    static lrow_t rows[MAX_APPS];                       /* not on the UI task's stack */
    for (int i = 0; i < s_napps; i++) {
        memset(&rows[i], 0, sizeof(rows[i]));
        snprintf(rows[i].label, sizeof(rows[i].label), "%s", s_apps[i].name);
        snprintf(rows[i].icon, sizeof(rows[i].icon), "%s", s_apps[i].name);
        rows[i].system = s_apps[i].system;
        if (s_apps[i].dev) {
            snprintf(rows[i].sub, sizeof(rows[i].sub), "DEV");
        }
        rows[i].tag = i;
    }
    list_open("APPS", rows, s_napps, apps_picked, 1, -1);
}

/* ---------------------------------------------------------------- the quick menu and the shortcut picker */

enum { M_SETTINGS = 1, M_ALERTS, M_SHORTCUTS, M_LOCK, M_PIN, M_ROTATE, M_TASKS };

static void alerts_open(void);

static void menu_picked(int tag)
{
    switch (tag) {
    case M_SETTINGS: tp_open_app("Settings", 1); break;
    case M_ALERTS: alerts_open(); break;
    case M_SHORTCUTS: shortcut_picker(-1); break;
    case M_LOCK: list_close(); tp_lock_show(); break;
    case M_PIN: list_close(); tp_pin_setup_open(); break;
    case M_ROTATE: list_close(); tp_rotate_next(); break;
    case M_TASKS: list_close(); w10m_tasks_open(); break;
    default: break;
    }
}

static void menu_open(void)
{
    lrow_t rows[8];
    int n = 0;
    memset(rows, 0, sizeof(rows));
    snprintf(rows[n].label, sizeof(rows[n].label), "Settings"); rows[n++].tag = M_SETTINGS;
    snprintf(rows[n].label, sizeof(rows[n].label), "Alerts");
    if (tp_note_count() > 0) {
        snprintf(rows[n].sub, sizeof(rows[n].sub), "%d", tp_note_count());
    }
    rows[n++].tag = M_ALERTS;
    snprintf(rows[n].label, sizeof(rows[n].label), "Shortcuts"); rows[n++].tag = M_SHORTCUTS;
    snprintf(rows[n].label, sizeof(rows[n].label), "Lock now"); rows[n++].tag = M_LOCK;
    snprintf(rows[n].label, sizeof(rows[n].label), "PIN");
    snprintf(rows[n].sub, sizeof(rows[n].sub), "%s", tp_pin_is_set() ? "ON" : "OFF");
    rows[n++].tag = M_PIN;
    snprintf(rows[n].label, sizeof(rows[n].label), "Rotate screen"); rows[n++].tag = M_ROTATE;
    snprintf(rows[n].label, sizeof(rows[n].label), "Tasks"); rows[n++].tag = M_TASKS;
    list_open("MENU", rows, n, menu_picked, 0, -1);
}

void tp_home_menu_toggle(void)
{
    if (s_ls.open) {
        list_close();
    } else if (tp_home_showing()) {
        menu_open();
    }
}

static void alerts_picked(int tag)
{
    (void)tag;
    list_close();
}

static void alerts_open(void)
{
    lrow_t rows[8];
    int n = 0;
    memset(rows, 0, sizeof(rows));
    for (int i = 0; i < tp_note_count() && n < 8; i++) {
        char title[32], text[96];
        if (tp_note_peek(i, title, sizeof(title), text, sizeof(text)) == 0) {
            snprintf(rows[n].label, sizeof(rows[n].label), "%s", title);
            snprintf(rows[n].sub, sizeof(rows[n].sub), "%.18s", text);
            rows[n].tag = i;
            n++;
        }
    }
    if (n == 0) {
        snprintf(rows[0].label, sizeof(rows[0].label), "Nothing new");
        n = 1;
    }
    list_open("ALERTS", rows, n, alerts_picked, 0, -1);
}

static void notice_clicked(lv_event_t *e)
{
    (void)e;
    alerts_open();
}

/* The shortcut picker: first which tile (B, X or Y), then what it opens. */
static int s_pick_tile;

enum { C_WEATHER = 1000, C_MUSIC, C_FRIENDS, C_NONE, C_APP = 2000 };

static void choice_picked(int tag)
{
    char key[16], val[PURR_APP_NAME_LEN];
    snprintf(key, sizeof(key), "home.tile%d", s_pick_tile);
    switch (tag) {
    case C_WEATHER: snprintf(val, sizeof(val), "weather"); break;
    case C_MUSIC: snprintf(val, sizeof(val), "music"); break;
    case C_FRIENDS: snprintf(val, sizeof(val), "friends"); break;
    case C_NONE: snprintf(val, sizeof(val), "none"); break;
    default:
        if (tag >= C_APP && tag - C_APP < s_napps) {
            snprintf(val, sizeof(val), "%s", s_apps[tag - C_APP].name);
        } else {
            return;
        }
    }
    purr_settings_kv_set(key, val);
    list_close();
    home_fill();
    tp_home_layout();
    tp_home_focus();
    w10m_banner("Shortcut saved.", 0);
}

static void choices_open(void)
{
    static lrow_t rows[MAX_ROWS];
    int n = 0;
    memset(rows, 0, sizeof(rows));
    snprintf(rows[n].label, sizeof(rows[n].label), "Weather"); snprintf(rows[n].sub, sizeof(rows[n].sub), "SOON"); rows[n++].tag = C_WEATHER;
    snprintf(rows[n].label, sizeof(rows[n].label), "Music"); rows[n++].tag = C_MUSIC;
    snprintf(rows[n].label, sizeof(rows[n].label), "Friends"); snprintf(rows[n].sub, sizeof(rows[n].sub), "SOON"); rows[n++].tag = C_FRIENDS;
    for (int i = 0; i < s_napps && n < MAX_ROWS - 1; i++) {
        snprintf(rows[n].label, sizeof(rows[n].label), "%s", s_apps[i].name);
        snprintf(rows[n].sub, sizeof(rows[n].sub), "APP");
        rows[n++].tag = C_APP + i;
    }
    snprintf(rows[n].label, sizeof(rows[n].label), "Nothing"); rows[n++].tag = C_NONE;
    char title[16];
    snprintf(title, sizeof(title), "TILE %s", k_tile_key[s_pick_tile]);
    list_open(title, rows, n, choice_picked, 0, -1);
}

static void tilepick_picked(int tag)
{
    s_pick_tile = tag;
    choices_open();
}

static void shortcut_picker(int idx)
{
    if (idx >= 1 && idx < TILES) {
        s_pick_tile = idx;
        choices_open();
        return;
    }
    lrow_t rows[3];
    memset(rows, 0, sizeof(rows));
    for (int i = 1; i < TILES; i++) {
        tile_face_t f;
        tile_face(i, &f);
        snprintf(rows[i - 1].label, sizeof(rows[i - 1].label), "%s  %s", k_tile_key[i], f.name);
        rows[i - 1].tag = i;
    }
    list_open("SHORTCUTS", rows, 3, tilepick_picked, 0, -1);
}

/* ---------------------------------------------------------------- settings home.tile1 to home.tile3, and building */

static int tile_get_n(int idx, char *o, size_t c)
{
    char v[PURR_APP_NAME_LEN];
    tile_setting(idx, v, sizeof(v));
    snprintf(o, c, "%s", v);
    return 0;
}

static void *reload_cb(void *arg)
{
    (void)arg;
    home_fill();
    tp_home_layout();
    tp_home_focus();
    return NULL;
}

static int tile_set_n(int idx, const char *v, char *e, size_t c)
{
    if (v[0] == '\0' || strlen(v) >= PURR_APP_NAME_LEN) {
        snprintf(e, c, "weather, music, friends, none, or the name of an app");
        return -1;
    }
    char key[16];
    snprintf(key, sizeof(key), "home.tile%d", idx);
    if (purr_settings_kv_set(key, v) != 0) {
        snprintf(e, c, "could not save");
        return -1;
    }
    purr_ui_service_run(reload_cb, NULL, NULL);
    return 0;
}

static int t1_get(char *o, size_t c) { return tile_get_n(1, o, c); }
static int t2_get(char *o, size_t c) { return tile_get_n(2, o, c); }
static int t3_get(char *o, size_t c) { return tile_get_n(3, o, c); }
static int t1_set(const char *v, char *e, size_t c) { return tile_set_n(1, v, e, c); }
static int t2_set(const char *v, char *e, size_t c) { return tile_set_n(2, v, e, c); }
static int t3_set(const char *v, char *e, size_t c) { return tile_set_n(3, v, e, c); }

static const purr_setting_t s_tile_settings[] = {
    {"home.tile1", "what the B shortcut tile opens", "weather|music|friends|none|<app>", 0, t1_get, t1_set},
    {"home.tile2", "what the X shortcut tile opens", "weather|music|friends|none|<app>", 0, t2_get, t2_set},
    {"home.tile3", "what the Y shortcut tile opens", "weather|music|friends|none|<app>", 0, t3_get, t3_set},
};

void tp_home_build(lv_obj_t *screen)
{
    for (size_t i = 0; i < sizeof(s_tile_settings) / sizeof(s_tile_settings[0]); i++) {
        purr_settings_register(&s_tile_settings[i]);
    }
    s_home = lv_obj_create(screen);
    lv_obj_remove_style_all(s_home);
    lv_obj_clear_flag(s_home, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_home, LV_OPA_TRANSP, 0);

    /* the big clock: Space Grotesk Bold at 92 px (the date is in the strip above, as in the design) */
    s_clock_big = tp_label(s_home, "--:--", &tp_font_clock_xl, NULL);
    lv_obj_set_style_text_color(s_clock_big, tp_ink(), 0);
    lv_obj_set_style_text_letter_space(s_clock_big, -3, 0);
    s_date = NULL;

    s_notice = tp_box(s_home, TP_FILL_SAND);
    lv_obj_set_style_pad_hor(s_notice, 6, 0);
    lv_obj_set_style_border_width(s_notice, 2, 0);
    s_notice_lbl = tp_label(s_notice, "", &tp_font_mono_12, NULL);
    lv_label_set_long_mode(s_notice_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_notice_lbl, LV_PCT(88));
    lv_obj_align(s_notice_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    s_notice_cnt = tp_label(s_notice, "", &tp_font_mono_12, NULL);
    lv_obj_align(s_notice_cnt, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(s_notice, notice_clicked, LV_EVENT_CLICKED, NULL);

    {
        static const tp_hint_t keys[] = {{LV_SYMBOL_LEFT LV_SYMBOL_RIGHT, "CHOOSE"}, {"A", "OPEN"}, {"X", "MENU"}, {"FUNC+ABXY", "SHORTCUT"}};
        s_hint = tp_footer(s_home, keys, 4);
    }

    load_apps();
    home_fill();
    tp_home_layout();
    tp_home_focus();
    s_tick = lv_timer_create(tick_cb, 1000, NULL);
    clock_refresh();
    ESP_LOGI(TAG, "home built, %d apps", s_napps);
}
