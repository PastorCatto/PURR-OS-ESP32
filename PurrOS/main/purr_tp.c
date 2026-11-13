/*
 * purr_tp.c - the Type shell's core (plan7.md, "The Type shell"): shell state, the status strip, banners and busy messages, the shared shell contract
 * (purr_w10m_priv.h plus purr_shell_notified), orientation, brightness, the key routing and start-up. See purr_tp_priv.h for the pieces and the idea.
 *
 * It is the built-in shell when the image is built with CONFIG_PURR_SHELL_TYPE. The shell dispatcher (purr_shell.c) calls the built-in shell by fixed names, so
 * this shell's share of the shared interface is named `<name>_droid` the way the Android shell's is (purr_shell_names.h), and purr_tp_slot.c gives the five
 * entry points their names. The Android shell's own files are not part of this image and are not changed.
 */
#define PURR_SHELL_IMPL_ANDROID
#include "purr_shell_names.h"
#include "purr_tp_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"

#include "commands.h"
#include "login.h"
#include "purr_battery.h"
#include "purr_brand.h"
#include "purr_catrt.h"
#include "purr_catrt_game.h"
#include "purr_catrt_ui.h"
#include "purr_console.h"
#include "purr_driver.h"
#include "purr_kernel.h"
#include "purr_lvgl.h"
#include "purr_net.h"
#include "purr_power_plat.h"
#include "purr_settings.h"
#include "purr_shell.h"
#include "purr_ui_assets.h"
#include "purr_ui_config.h"
#include "purr_ui_service.h"
#include "purr_wallpaper.h"

static const char *TAG = "type-shell";

#define BANNER_MS 5000
#define NOTES_MAX 8

/* ---------------------------------------------------------------- the footer: an ink bar with the keys in the accent */

/* One row of hints on an ink bar along the bottom: the key in the accent, what it does in paper, as in the design. A symbol key is set in the symbol font. */
lv_obj_t *tp_footer(lv_obj_t *parent, const tp_hint_t *h, int n)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(bar, TP_FOOT_H);
    lv_obj_set_style_bg_color(bar, tp_ink(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar, 8, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 3, 0);
    lv_color_t acc = tp_accent(), paper = tp_paper();
    for (int i = 0; i < n; i++) {
        int sym = (unsigned char)h[i].key[0] >= 0x80;
        lv_obj_t *k = tp_label(bar, h[i].key, sym ? &lv_font_montserrat_12 : &tp_font_mono_12, &acc);
        lv_obj_t *t = tp_label(bar, h[i].text, &tp_font_mono_12, &paper);
        if (i + 1 < n) {
            lv_obj_set_style_pad_right(t, 8, 0);
        }
        (void)k;
    }
    return bar;
}

/* ---------------------------------------------------------------- pictures (Settings and the windows draw the shared icons through this) */

typedef struct {
    char name[24];
    lv_image_dsc_t dsc;
    int ok;
} img_slot_t;

static EXT_RAM_BSS_ATTR img_slot_t s_imgs[96];
static int s_nimgs;

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

/* ---------------------------------------------------------------- the gamepad, if the board has one */

const purr_gamepad_ops_t *tp_pad(void)
{
    void *h = NULL;
    const purr_gamepad_ops_t *ops = purr_class_ops(PURR_DRV_CLASS_GAMEPAD, &h);
    return (ops != NULL && ops->version >= 1 && ops->buttons != NULL) ? ops : NULL;
}

void *tp_pad_handle(void)
{
    void *h = NULL;
    purr_class_ops(PURR_DRV_CLASS_GAMEPAD, &h);
    return h;
}

void tp_pad_hint(int mask)
{
    static int s_last = -2;
    const purr_gamepad_ops_t *p = tp_pad();
    if (p == NULL || p->set_leds == NULL || mask == s_last) {
        return;
    }
    s_last = mask;
    p->set_leds(tp_pad_handle(), mask);
}

/* ---------------------------------------------------------------- shell state */

static lv_obj_t *s_strip, *s_clock, *s_title, *s_wifi, *s_batt, *s_batt_fill, *s_pct, *s_ticker, *s_ticker_lbl, *s_dev;
static int s_signed_in;
static int s_brightness = 255;
static lv_timer_t *s_banner_timer, *s_done_timer;
static int s_busy;
static char s_busy_msg[40];

lv_obj_t *tp_strip(void) { return s_strip; }
int tp_signed_in(void) { return s_signed_in; }
void tp_signed_in_set(void) { s_signed_in = 1; }

/* ---------------------------------------------------------------- the clock and the notifications */

static void fmt_hm(const struct tm *tm, char *buf, size_t cap)
{
    if (purr_settings_kv_int("time.clock24", 1)) {
        snprintf(buf, cap, "%02d:%02d", tm->tm_hour, tm->tm_min);
    } else {
        int h = tm->tm_hour % 12;
        snprintf(buf, cap, "%d:%02d %s", h == 0 ? 12 : h, tm->tm_min, tm->tm_hour < 12 ? "AM" : "PM");
    }
}

void tp_clock_strings(char *tb, size_t tcap, char *db, size_t dcap)
{
    time_t now = time(NULL);
    if (now > 1000000000) {
        struct tm tm;
        localtime_r(&now, &tm);
        fmt_hm(&tm, tb, tcap);
        strftime(db, dcap, "%a %d %b", &tm);
        for (char *c = db; *c != '\0'; c++) {
            if (*c >= 'a' && *c <= 'z') {
                *c = (char)(*c - 32);
            }
        }
    } else {
        snprintf(tb, tcap, "--:--");
        snprintf(db, dcap, "CLOCK NOT SET");
    }
}

static struct {
    char title[32], text[96];
} s_notes[NOTES_MAX];
static int s_nnotes;

static void banner_show(const char *msg, int bad);

void tp_note_add(const char *title, const char *text)
{
    memmove(&s_notes[1], &s_notes[0], sizeof(s_notes[0]) * (NOTES_MAX - 1));
    snprintf(s_notes[0].title, sizeof(s_notes[0].title), "%s", title != NULL ? title : "");
    snprintf(s_notes[0].text, sizeof(s_notes[0].text), "%s", text != NULL ? text : "");
    if (s_nnotes < NOTES_MAX) {
        s_nnotes++;
    }
    char line[140];
    snprintf(line, sizeof(line), "%s: %s", s_notes[0].title, s_notes[0].text);
    banner_show(line, 0);
    tp_home_reload();                                   /* the home screen's notice follows the newest */
}

int tp_note_count(void) { return s_nnotes; }

int tp_note_peek(int i, char *title, size_t tcap, char *text, size_t xcap)
{
    if (i < 0 || i >= s_nnotes) {
        return -1;
    }
    snprintf(title, tcap, "%s", s_notes[i].title);
    snprintf(text, xcap, "%s", s_notes[i].text);
    return 0;
}

/* ---------------------------------------------------------------- the status strip: the clock, then wifi and battery. A banner or a busy message takes over the left. */

static void strip_layout(void)
{
    if (s_strip == NULL) {
        return;
    }
    lv_obj_set_size(s_strip, disp_w(), TP_STRIP_H);
    lv_obj_align(s_strip, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_align(s_clock, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(s_batt, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_align_to(s_pct, s_batt, LV_ALIGN_OUT_LEFT_MID, -4, 0);
    lv_obj_align_to(s_wifi, s_pct, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    lv_obj_align_to(s_dev, s_clock, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
    lv_obj_set_size(s_ticker, disp_w() - 90, TP_STRIP_H - 2);
    lv_obj_align(s_ticker, LV_ALIGN_LEFT_MID, 3, 0);
}

static void strip_refresh(lv_timer_t *t)
{
    (void)t;
    char notice[64];
    if (purr_catrt_take_notice(notice, sizeof(notice))) {      /* the runtime closed an app for room, or refused a service */
        w10m_banner(notice, 0);
    }
    char tb[16], db[24];
    tp_clock_strings(tb, sizeof(tb), db, sizeof(db));
    lv_label_set_text(s_clock, tp_home_showing() ? db : (tp_drawer_is_open() ? "" : tb));        /* on the home screen the clock is the big one below: the strip says the date, as in the design */
    int pct;
    if (purr_battery_get(&pct, NULL)) {
        char p[8];
        snprintf(p, sizeof(p), "%d%%", pct);
        lv_label_set_text(s_pct, p);
        lv_obj_set_width(s_batt_fill, (lv_coord_t)(pct * 17 / 100 < 1 ? 1 : pct * 17 / 100));
    } else {
        lv_label_set_text(s_pct, "");
        lv_obj_set_width(s_batt_fill, 1);
    }
    purr_net_status_t ns;
    purr_net_status(&ns);
    if (ns.connected) {
        lv_obj_remove_flag(s_wifi, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_wifi, LV_OBJ_FLAG_HIDDEN);
    }
    strip_layout();
}

void tp_strip_title(const char *t)
{
    if (s_title == NULL) {
        return;
    }
    if (t == NULL || t[0] == '\0') {
        lv_obj_add_flag(s_title, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_title, t);
        lv_obj_remove_flag(s_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_title, LV_ALIGN_CENTER, 0, 0);
    }
    strip_refresh(NULL);                                            /* the left side follows the screen: the date on home, nothing in a list (its panel has the clock), the time elsewhere */
}

void w10m_dev_badge_update(void)
{
    if (s_dev == NULL) {
        return;
    }
    if (purr_security_dev_active()) {
        lv_obj_remove_flag(s_dev, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_dev, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_strip(void)
{
    s_strip = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_strip);
    lv_obj_clear_flag(s_strip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_strip, tp_paper(), 0);
    lv_obj_set_style_bg_opa(s_strip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_strip, 2, 0);
    lv_obj_set_style_border_side(s_strip, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(s_strip, tp_ink(), 0);
    lv_obj_set_style_text_color(s_strip, tp_ink(), 0);
    lv_obj_set_style_text_font(s_strip, &tp_font_mono_12, 0);

    s_clock = tp_label(s_strip, "--:--", &tp_font_mono_12, NULL);
    lv_obj_set_style_text_letter_space(s_clock, 1, 0);
    s_title = tp_label(s_strip, "", &tp_font_mono_12, NULL);
    lv_obj_set_style_text_letter_space(s_title, 2, 0);
    lv_obj_add_flag(s_title, LV_OBJ_FLAG_HIDDEN);
    s_wifi = tp_label(s_strip, LV_SYMBOL_WIFI, &lv_font_montserrat_12, NULL);
    lv_obj_add_flag(s_wifi, LV_OBJ_FLAG_HIDDEN);
    s_pct = tp_label(s_strip, "", &tp_font_mono_12, NULL);

    s_batt = lv_obj_create(s_strip);                    /* the battery: an outline with a fill that is as wide as the charge */
    lv_obj_remove_style_all(s_batt);
    lv_obj_clear_flag(s_batt, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_batt, 22, 9);
    lv_obj_set_style_border_width(s_batt, 1, 0);
    lv_obj_set_style_border_color(s_batt, tp_ink(), 0);
    s_batt_fill = lv_obj_create(s_batt);
    lv_obj_remove_style_all(s_batt_fill);
    lv_obj_set_style_bg_color(s_batt_fill, tp_ink(), 0);
    lv_obj_set_style_bg_opa(s_batt_fill, LV_OPA_COVER, 0);
    lv_obj_set_size(s_batt_fill, 1, 5);
    lv_obj_align(s_batt_fill, LV_ALIGN_LEFT_MID, 1, 0);

    s_dev = tp_label(s_strip, "DEV", &tp_font_mono_12, NULL);
    lv_obj_set_style_bg_color(s_dev, tp_ink(), 0);
    lv_obj_set_style_bg_opa(s_dev, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_dev, tp_paper(), 0);
    lv_obj_set_style_pad_hor(s_dev, 3, 0);
    lv_obj_add_flag(s_dev, LV_OBJ_FLAG_HIDDEN);

    s_ticker = lv_obj_create(s_strip);                  /* a banner or a busy message, over the clock */
    lv_obj_remove_style_all(s_ticker);
    lv_obj_clear_flag(s_ticker, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_ticker, tp_paper(), 0);
    lv_obj_set_style_bg_opa(s_ticker, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_ticker, LV_OBJ_FLAG_HIDDEN);
    s_ticker_lbl = tp_label(s_ticker, "", &tp_font_mono_12, NULL);
    lv_label_set_long_mode(s_ticker_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_ticker_lbl, LV_PCT(100));
    lv_obj_align(s_ticker_lbl, LV_ALIGN_LEFT_MID, 3, 0);

    lv_timer_create(strip_refresh, 1000, NULL);
    strip_refresh(NULL);
    w10m_dev_badge_update();
}

/* ---------------------------------------------------------------- banners and busy messages */

static void ticker_set(const char *text, int bad)
{
    lv_label_set_text(s_ticker_lbl, text);
    lv_obj_set_style_bg_color(s_ticker, bad ? tp_accent() : tp_paper(), 0);
    lv_obj_set_style_text_color(s_ticker, bad ? tp_on_accent() : tp_ink(), 0);
    lv_obj_remove_flag(s_ticker, LV_OBJ_FLAG_HIDDEN);
}

static void ticker_hide(void)
{
    lv_obj_add_flag(s_ticker, LV_OBJ_FLAG_HIDDEN);
}

static void banner_end_cb(lv_timer_t *t)
{
    s_banner_timer = NULL;
    lv_timer_delete(t);
    if (s_busy) {
        ticker_set(s_busy_msg, 0);                      /* work in progress shows again */
    } else {
        ticker_hide();
    }
}

static void banner_show(const char *msg, int bad)
{
    if (s_ticker == NULL) {
        return;
    }
    ticker_set(msg, bad);
    if (s_banner_timer != NULL) {
        lv_timer_delete(s_banner_timer);
    }
    s_banner_timer = lv_timer_create(banner_end_cb, BANNER_MS, NULL);
    lv_timer_set_repeat_count(s_banner_timer, 1);
}

void w10m_banner(const char *msg, int bad)
{
    banner_show(msg != NULL ? msg : "", bad);
}

static void *banner_ui(void *arg)
{
    const char *m = arg;
    banner_show(m, m[0] == '!');
    return NULL;
}

void w10m_banner_post(const char *msg, int bad)
{
    char buf[100];
    snprintf(buf, sizeof(buf), "%s%s", bad ? "!" : "", msg != NULL ? msg : "");        /* a leading ! is the error mark across the task hop */
    purr_ui_service_run(banner_ui, buf, NULL);
}

static void done_cb(lv_timer_t *t)
{
    s_done_timer = NULL;
    lv_timer_delete(t);
    if (!s_busy) {
        ticker_hide();
    }
}

void w10m_busy_begin(const char *msg)
{
    s_busy = 1;
    snprintf(s_busy_msg, sizeof(s_busy_msg), "%s", msg != NULL ? msg : "");
    if (s_banner_timer == NULL) {
        ticker_set(s_busy_msg, 0);
    }
}

void w10m_busy_end(void)
{
    if (!s_busy) {
        return;
    }
    s_busy = 0;
    if (s_banner_timer == NULL) {
        ticker_set("Done", 0);
        if (s_done_timer != NULL) {
            lv_timer_delete(s_done_timer);
        }
        s_done_timer = lv_timer_create(done_cb, 500, NULL);
        lv_timer_set_repeat_count(s_done_timer, 1);
    }
}

/* ---------------------------------------------------------------- what the three keys do */

int tp_sheet_open(void)
{
    return w10m_settings_is_open() || w10m_devportal_is_open() || w10m_tasks_is_open() || w10m_perm_is_open() || purr_catrt_ui_is_open();
}

void tp_go_home(void)
{
    if (tp_modal_up() || !s_signed_in || w10m_perm_is_open()) {
        return;
    }
    tp_nav_reset();
    w10m_tasks_close();
    for (int i = 0; i < 4 && w10m_settings_is_open(); i++) {
        w10m_settings_back();                           /* up a page at a time, the last one closes it */
    }
    for (int i = 0; i < 4 && w10m_devportal_is_open(); i++) {
        w10m_devportal_back();
    }
    purr_catrt_home();                                  /* the app in front steps back; it is not closed */
    tp_home_reset();
}

void tp_back(void)
{
    if (tp_modal_up() || !s_signed_in || w10m_perm_is_open()) {
        return;
    }
    if (tp_home_back()) {
        /* the quick menu or the app list closed */
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

void tp_menu_key(void)
{
    if (tp_modal_up() || !s_signed_in || w10m_perm_is_open()) {
        return;
    }
    tp_home_menu_toggle();
}

static int button_of_key(uint32_t key)
{
    switch (key) {
    case LV_KEY_ENTER: return PURR_PAD_A;
    case LV_KEY_ESC:   return PURR_PAD_B;
    case PURR_KEY_MENU: return PURR_PAD_X;
    case LV_KEY_HOME:  return PURR_PAD_Y;
    default:           return -1;
    }
}

/* Every key passes here before it is delivered. The lock screen, the PIN pad and the sign-in take what they need; then, FUNC held plus a face button is a
 * shortcut; with a sheet or an app window in front the keys are theirs (apart from Back, Home and Menu); otherwise the home screen and its lists use them. */
static int key_hook(uint32_t key)
{
    if (w10m_perm_key(key)) {                           /* a permission question is up: it is answered first, by whatever is on screen (a gamepad has no touch) */
        return 1;
    }
    if (tp_modal_key(key)) {
        return 1;
    }
    if (!s_signed_in) {
        return 0;
    }
    const purr_gamepad_ops_t *p = tp_pad();
    int b = button_of_key(key);
    if (p != NULL && b >= 0 && (p->buttons(tp_pad_handle()) & (1u << PURR_PAD_FUNC)) && tp_home_chord(b)) {
        return 1;
    }
    if (key == LV_KEY_NEXT) {                           /* FUNC tapped: nothing on its own yet (play and pause, once a music app reports) */
        return 1;
    }
    if (tp_sheet_open()) {
        if (tp_nav_key(key)) {                          /* the sheet's controls: arrows, Enter, the keyboard for a text box */
            return 1;
        }
        if (key == LV_KEY_ESC) {
            tp_back();
            return 1;
        }
        if (key == LV_KEY_HOME) {
            tp_go_home();
            return 1;
        }
        return 0;
    }
    if (tp_home_key(key)) {
        return 1;
    }
    switch (key) {
    case LV_KEY_ESC:
        tp_back();
        return 1;
    case LV_KEY_HOME:
        tp_go_home();
        return 1;
    case PURR_KEY_MENU:
        tp_menu_key();
        return 1;
    default:
        return 0;
    }
}

static void home_hold(void)
{
    tp_go_home();
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

static void relayout(void)
{
    strip_layout();
    tp_home_layout();
    w10m_settings_layout();
    w10m_devportal_layout();
    purr_catrt_ui_layout();
    tp_lock_layout();
    tp_signin_layout();
    tp_nav_layout();
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

void tp_rotate_next(void)
{
    w10m_apply_orientation((purr_lvgl_get_rotation() + 90) % 360);
}

static void *orient_cb(void *arg)
{
    int deg = (int)(intptr_t)arg;
    if (purr_lvgl_set_rotation(deg) != 0) {
        return (void *)(intptr_t)-1;
    }
    w10m_apply_orientation(deg);
    return NULL;
}

esp_err_t purr_tp_set_orientation(int degrees)
{
    void *r = NULL;
    esp_err_t e = purr_ui_service_run(orient_cb, (void *)(intptr_t)degrees, &r);
    if (e != ESP_OK) {
        return e;
    }
    return r == NULL ? ESP_OK : ESP_ERR_INVALID_ARG;
}

/* ---------------------------------------------------------------- settings this shell registers (the same ones Froyo registers) */

static int bright_get(char *o, size_t c) { snprintf(o, c, "%d", s_brightness); return 0; }

static int bright_set(const char *v, char *e, size_t c)
{
    int n = atoi(v);
    if (n < 1 || n > 255) {
        snprintf(e, c, "expected 1 to 255");
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
    if (purr_tp_set_orientation(n) != ESP_OK) {
        snprintf(e, c, "the screen is not running");
        return -1;
    }
    return 0;
}

static int shell_get(char *o, size_t c) { return purr_shell_mode_get(o, c); }
static int shell_set(const char *v, char *e, size_t c) { return purr_shell_mode_set(v, e, c); }

static const purr_setting_t s_keys_table[] = {
    {"display.brightness", "backlight", "1-255", 0, bright_get, bright_set},
    {"display.rotation", "screen orientation in degrees", "0|90|180|270", 0, rot_get, rot_set},
    {"ui.shell", "which shell starts (next boot)", "builtin|<id>|nav", 1, shell_get, shell_set},
};

/* ---------------------------------------------------------------- sheets and apps */

void w10m_place_sheet(lv_obj_t *sheet)
{
    /* The strip, the lock screen and the sign-in are on the top layer, always above the screen. The sheets (Settings, the portal, an app window) are
     * children of the screen, so the newest on top is all that is needed. */
    lv_obj_move_foreground(sheet);
}

int w10m_is_locked(void)
{
    return tp_lock_is_on() || !s_signed_in;
}

void w10m_apps_changed(void)
{
    tp_home_reload();
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
    tp_note_add(r->title, r->text);
    return NULL;
}

void w10m_note_post(const char *title, const char *text)
{
    note_req_t r;
    snprintf(r.title, sizeof(r.title), "%s", title != NULL ? title : "");
    snprintf(r.text, sizeof(r.text), "%s", text != NULL ? text : "");
    purr_ui_service_run(note_ui, &r, NULL);
}

/* This shell draws no wallpaper (paper is the picture); the choice in Settings is kept for a shell that does. */
static void wallpaper_changed(void) {}

/* ---------------------------------------------------------------- start */

static void theme_changed(void)
{
    tp_home_reload();                                   /* the accent changed: what shows it is drawn again */
}

static void *build_cb(void *arg)
{
    (void)arg;
    purr_theme_set_skin(PURR_SKIN_W10M);                /* the shared screens (Settings, windows) in the flat, square look that sits next to this one */
    purr_theme_set_type_look(1);                        /* ... in paper, ink and the accent: Settings, the portal, the task manager and every app window take their look from here */
    for (int i = 0; i < purr_theme_accent_count(); i++) {
        if (strcmp(purr_theme_accent_at(i)->name, "Orange") == 0) {
            purr_theme_set_accent(i);                   /* this shell's own default; a saved choice (purr_theme_init) overrides it */
            break;
        }
    }
    purr_theme_init();
    int saved_b = purr_settings_kv_int("display.brightness", 255);
    w10m_set_brightness(saved_b < 1 ? 255 : saved_b);
    int saved_r = purr_settings_kv_int("display.rotation", -1);
    if (saved_r >= 0 && saved_r <= 270 && saved_r % 90 == 0) {
        purr_lvgl_set_rotation(saved_r);                /* before anything is laid out */
    }
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, tp_paper(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_signed_in = purr_login_current()->name[0] != '\0';   /* a session already exists (first-time setup) */

    tp_home_build(scr);
    w10m_settings_build();
    w10m_devportal_build();
    build_strip();
    tp_lock_build();                                    /* above the strip: the lock screen and the sign-in cover the whole screen */
    tp_signin_build();
    tp_nav_init();
    purr_theme_on_change(theme_changed);
    if (purr_login_autologin_used()) {
        char msg[80];
        snprintf(msg, sizeof(msg), "Signed in automatically as %s (developer mode).", purr_login_current()->name);
        tp_note_add(PURR_BRAND_PRODUCT, msg);
    }
    if (s_signed_in) {
        purr_theme_load_user(purr_login_current()->name);
        tp_signin_hidden_check();
    }
    relayout();
    ESP_LOGI(TAG, "shell up: %dx%d, accent %s", disp_w(), disp_h(), purr_theme_accent_at(purr_theme_accent())->name);
    return NULL;
}

static void lock_on_sleep(int on)
{
    if (!on && purr_settings_kv_int("security.lock_on_sleep", 1)) {
        purr_tp_lock();
    }
}

esp_err_t purr_tp_start(void)
{
    purr_lvgl_set_scroll_handler(purr_catrt_ui_scroll);              /* the trackball scrolls the app window on screen */
    purr_lvgl_set_key_hook(key_hook);
    purr_lvgl_set_trackball_hold(home_hold, 600);                    /* holding the trackball's button is Home */
    purr_wallpaper_set_listener(wallpaper_changed);
    purr_wallpaper_register();
    purr_catrt_ui_init();
    purr_catrt_game_init();
    purr_power_sys_set_repaint(purr_ui_service_repaint);              /* the panel sleeps with the screen; wake draws the current picture first */
    purr_power_sys_set_screen_hook(lock_on_sleep);                    /* and it locks when the screen goes off (security.lock_on_sleep, on unless set to 0) */
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
    tp_home_reload();
    return NULL;
}

esp_err_t purr_tp_refresh_apps(void)
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
    tp_note_add(n->title, n->text);
    return NULL;
}

esp_err_t purr_tp_notify(const char *title, const char *text)
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
        tp_lock_show();
    }
    return NULL;
}

esp_err_t purr_tp_lock(void)
{
    return purr_ui_service_run(lock_cb, NULL, NULL);
}

/* The shared notify log tells the shell about each new notification. */
void purr_shell_notified_droid(const char *title, const char *text)
{
    tp_note_add(title, text);
}
