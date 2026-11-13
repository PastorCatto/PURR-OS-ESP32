/*
 * purr_tp_lock.c - the Type shell's lock screen, optional PIN and sign-in (plan7.md, "The Type shell").
 *
 * They are the three screens that take the whole display and every key, so they share one root object on the top layer and one state:
 *
 *   lock screen   ink background, the hours in paper and the minutes in the accent colour, the date in a paper panel at the side, and a bar that fills while
 *                 FUNC is held (or the bar is touched and held, or Enter is pressed on a board with a keyboard): after a second the screen unlocks, through the
 *                 PIN pad if the user has set a PIN. A pocket cannot do that by accident.
 *   PIN pad       four digits on a pad that the d-pad and a finger both use. A pause after wrong tries, and FUNC for the account password instead (purr_pin.h).
 *   sign-in       the accounts as an iPod-style list, then the password typed on the on-screen keyboard (d-pad or touch; a hardware keyboard types straight in).
 *                 The password is checked exactly as the Android shell checks it (purr_login_try on a task of its own: the hash is long), and nothing else is
 *                 reachable until it succeeds.
 *
 * The screens are drawn again each time they show, so a change of accent colour is there the next time.
 */
#define PURR_SHELL_IMPL_ANDROID
#include "purr_shell_names.h"
#include "purr_tp_priv.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "commands.h"
#include "login.h"
#include "purr_battery.h"
#include "purr_brand.h"
#include "purr_fs.h"
#include "purr_lvgl.h"
#include "purr_net.h"
#include "purr_pin.h"
#include "purr_resetnote_os.h"
#include "purr_ui_service.h"

static const char *TAG = "type-lock";

#define HOLD_MS  1000
#define POLL_MS  40

typedef enum {
    M_NONE = 0,
    M_LOCK,
    M_SI_LIST,
    M_SI_PASS,
    M_PIN_UNLOCK,
    M_PIN_NEW,
    M_PIN_REPEAT,
    M_PIN_REMOVE,
} modal_t;

static modal_t s_modal;
static lv_obj_t *s_root;
static lv_timer_t *s_poll;
static int s_ticks;

/* the lock screen */
static lv_obj_t *s_bar, *s_bar_fill;
static int s_hold_ms, s_hold_spent;
static int s_bar_w;

/* the clock labels of the screens that show one (refreshed once a second) */
static lv_obj_t *s_clk_h, *s_clk_m, *s_clk_d;

/* the PIN pad */
static tp_grid_t *s_pad;
static lv_obj_t *s_pin_dot[PURR_PIN_LEN], *s_pin_msg;
static char s_pin[PURR_PIN_LEN + 1], s_pin_new[PURR_PIN_LEN + 1];
static int s_npin;
static purr_pin_guard_t s_guard;

/* the sign-in */
static tp_grid_t *s_kbd;
static lv_obj_t *s_pw_dot[10], *s_pw_msg;
static char s_pw[64];
static int s_npw;
static char s_si_name[PURR_USER_NAME_LEN], s_si_role[24];
static int s_si_fixed;                                  /* 1: the password to unlock the current user (the PIN's way out), not a sign-in */
static int s_si_busy;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---------------------------------------------------------------- the root and small helpers */

static void screen_for_modal(void);

static void root_prepare(int ink)
{
    tp_focus_clear();
    s_bar = s_bar_fill = NULL;
    s_clk_h = s_clk_m = s_clk_d = NULL;
    s_pin_msg = s_pw_msg = NULL;
    for (int i = 0; i < PURR_PIN_LEN; i++) {
        s_pin_dot[i] = NULL;
    }
    for (int i = 0; i < 10; i++) {
        s_pw_dot[i] = NULL;
    }
    tp_grid_free(s_pad);
    tp_grid_free(s_kbd);
    s_pad = NULL;
    s_kbd = NULL;
    lv_obj_clean(s_root);                               /* deletes the keys and labels; the grids' own records were freed above */
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_size(s_root, w10m_disp_w(), w10m_disp_h());
    lv_obj_set_style_bg_color(s_root, ink ? tp_ink() : tp_paper(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_root, ink ? tp_paper() : tp_ink(), 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);
}

static void root_hide(void)
{
    tp_focus_clear();
    if (s_root != NULL) {
        lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    }
    s_modal = M_NONE;
    tp_pad_hint(-1);
}

static lv_obj_t *big(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color, int x, int y)
{
    lv_obj_t *l = tp_label(parent, text, font, &color);
    lv_obj_set_style_text_letter_space(l, -3, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

/* The padlock of the design: a filled body under an open shackle, in the accent. */
static void padlock(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *sh = lv_obj_create(parent);
    lv_obj_remove_style_all(sh);
    lv_obj_clear_flag(sh, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(sh, 9, 9);
    lv_obj_set_pos(sh, x + 2, y);
    lv_obj_set_style_border_width(sh, 2, 0);
    lv_obj_set_style_border_color(sh, tp_accent(), 0);
    lv_obj_set_style_border_side(sh, (lv_border_side_t)(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT), 0);
    lv_obj_set_style_radius(sh, 4, 0);
    lv_obj_t *bd = lv_obj_create(parent);
    lv_obj_remove_style_all(bd);
    lv_obj_clear_flag(bd, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bd, 13, 9);
    lv_obj_set_pos(bd, x, y + 7);
    lv_obj_set_style_bg_color(bd, tp_accent(), 0);
    lv_obj_set_style_bg_opa(bd, LV_OPA_COVER, 0);
}

static void mono(lv_obj_t *l)
{
    lv_obj_set_style_text_letter_space(l, 1, 0);
}

static lv_obj_t *title_bar(const char *title)
{
    lv_obj_t *b = lv_obj_create(s_root);
    lv_obj_remove_style_all(b);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(b, 0, 0);
    lv_obj_set_size(b, w10m_disp_w(), TP_STRIP_H);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(b, tp_ink(), 0);
    lv_obj_t *l = tp_label(b, title, &tp_font_mono_12, NULL);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_center(l);
    return b;
}

static void date_parts(char *dow, size_t dc, char *day, size_t yc, char *mon, size_t mc)
{
    time_t now = time(NULL);
    if (now > 1000000000) {
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(dow, dc, "%a", &tm);
        strftime(day, yc, "%d", &tm);
        strftime(mon, mc, "%b", &tm);
        for (char *p = dow; *p; p++) *p = (char)toupper((unsigned char)*p);
        for (char *p = mon; *p; p++) *p = (char)toupper((unsigned char)*p);
    } else {
        snprintf(dow, dc, "---");
        snprintf(day, yc, "--");
        snprintf(mon, mc, "---");
    }
}

/* The stacked clock in a side panel (the sign-in list's, as the app list has it); refreshed by the poll timer. */
static void panel_clock(lv_obj_t *panel)
{
    s_clk_d = tp_label(panel, "", &tp_font_mono_12, NULL);
    mono(s_clk_d);
    lv_obj_align(s_clk_d, LV_ALIGN_TOP_LEFT, 8, 8);
    s_clk_h = tp_label(panel, "", &tp_font_clock_panel, NULL);
    lv_obj_align(s_clk_h, LV_ALIGN_TOP_LEFT, 6, 28);
    s_clk_m = tp_label(panel, "", &tp_font_clock_panel, NULL);
    lv_obj_align(s_clk_m, LV_ALIGN_TOP_LEFT, 6, 28 + 47);
}

static void clock_refresh(void)
{
    if (s_clk_h == NULL) {
        return;
    }
    char tb[16], db[24];
    tp_clock_strings(tb, sizeof(tb), db, sizeof(db));
    char hh[4] = {'-', '-', 0, 0}, mm[4] = {'-', '-', 0, 0};
    if (tb[2] == ':') {
        hh[0] = tb[0];
        hh[1] = tb[1];
        mm[0] = tb[3];
        mm[1] = tb[4];
    }
    lv_label_set_text(s_clk_h, hh);
    lv_label_set_text(s_clk_m, mm);
    if (s_clk_d != NULL) {
        lv_label_set_text(s_clk_d, db);
    }
}

/* ---------------------------------------------------------------- the PIN record (/home/<user>/.config/lock.pin) */

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

static int pin_path(char *out, size_t cap)
{
    const char *user = purr_login_current()->name;
    if (user[0] == '\0') {
        return -1;
    }
    snprintf(out, cap, "/home/%s/.config/lock.pin", user);
    return 0;
}

static int pin_read(char *buf, size_t cap)
{
    char path[96];
    purr_fs_t *fs = purr_login_fs();
    if (pin_path(path, sizeof(path)) != 0 || fs == NULL || !purr_fs_mounted(fs)) {
        return 0;
    }
    rd_t r = {buf, (uint32_t)cap - 1, 0};
    if (purr_fs_read(fs, path, rd_cb, &r) < 0) {
        return 0;
    }
    buf[r.len] = '\0';
    return r.len > 0;
}

int tp_pin_is_set(void)
{
    char rec[PURR_PIN_RECORD_MAX + 4];
    return pin_read(rec, sizeof(rec));
}

static int pin_write(const char *rec)
{
    char path[96];
    purr_fs_t *fs = purr_login_fs();
    if (pin_path(path, sizeof(path)) != 0 || fs == NULL || !purr_fs_mounted(fs)) {
        return -1;
    }
    for (size_t i = 1; path[i] != '\0'; i++) {          /* make /home/<you>/.config; an existing directory is just an ignored error */
        if (path[i] == '/') {
            char dir[96];
            memcpy(dir, path, i);
            dir[i] = '\0';
            purr_fs_mkdir(fs, dir);
        }
    }
    return purr_fs_write(fs, path, rec, (uint32_t)strlen(rec)) < 0 ? -1 : 0;
}

static void pin_clear(void)
{
    char path[96];
    purr_fs_t *fs = purr_login_fs();
    if (pin_path(path, sizeof(path)) == 0 && fs != NULL && purr_fs_mounted(fs)) {
        purr_fs_remove(fs, path);
    }
}

/* ---------------------------------------------------------------- the lock screen */

static void bar_layout_fill(void)
{
    if (s_bar_fill == NULL) {
        return;
    }
    int w = s_hold_ms * (s_bar_w - 2) / HOLD_MS;
    lv_obj_set_width(s_bar_fill, w < 0 ? 0 : (w > s_bar_w - 2 ? s_bar_w - 2 : w));
}

static void pin_screen(modal_t mode);
static void signin_pass_for(const char *name, const char *role, int fixed);

static void unlock_attempt(void)
{
    if (tp_pin_is_set()) {
        pin_screen(M_PIN_UNLOCK);
    } else {
        tp_lock_unlock();
    }
}

static void lock_screen(void)
{
    s_modal = M_LOCK;
    root_prepare(1);
    int w = w10m_disp_w(), h = w10m_disp_h();
    int side = w > 260 ? 80 : 0;                        /* the design: a 240 px dark side and an 80 px paper panel */
    int lw = w - side;

    padlock(s_root, 16, 10);
    lv_obj_t *lock = tp_label(s_root, "LOCKED", &tp_font_mono_12, NULL);
    mono(lock);
    lv_obj_set_pos(lock, 36, 9);
    int n = tp_note_count();
    char tag[24];
    snprintf(tag, sizeof(tag), n > 0 ? "%d NEW" : "NOTHING NEW", n);
    lv_obj_t *tg = tp_label(s_root, tag, &tp_font_mono_12, NULL);
    mono(tg);
    lv_obj_set_style_border_width(tg, 1, 0);
    lv_obj_set_style_border_color(tg, tp_paper(), 0);
    lv_obj_set_style_pad_hor(tg, 5, 0);
    lv_obj_align(tg, LV_ALIGN_TOP_RIGHT, -(side + 14), 7);          /* from the right edge of the dark side: its width is not known yet */

    char tb[16], db[24];
    tp_clock_strings(tb, sizeof(tb), db, sizeof(db));
    char hh[4] = {'-', '-', 0, 0}, mm[4] = {'-', '-', 0, 0};
    if (tb[2] == ':') {
        hh[0] = tb[0];
        hh[1] = tb[1];
        mm[0] = tb[3];
        mm[1] = tb[4];
    }
    int line = tp_font_clock_lock.line_height;          /* the hours in paper over the minutes in the accent, stacked tight */
    s_clk_h = big(s_root, hh, &tp_font_clock_lock, tp_paper(), 14, 28);
    s_clk_m = big(s_root, mm, &tp_font_clock_lock, tp_accent(), 14, 28 + line - 2);
    s_clk_d = NULL;

    lv_obj_t *cap = tp_label(s_root, tp_pad() != NULL ? "HOLD FUNC TO UNLOCK" : "HOLD TO UNLOCK", &tp_font_mono_12, NULL);
    mono(cap);
    lv_obj_set_pos(cap, 16, h - 40);
    s_bar_w = lw - 32;
    s_bar = lv_obj_create(s_root);                      /* a finger can hold it too */
    lv_obj_remove_style_all(s_bar);
    lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_bar, 16, h - 24);
    lv_obj_set_size(s_bar, s_bar_w, 14);
    lv_obj_set_style_border_width(s_bar, 2, 0);
    lv_obj_set_style_border_color(s_bar, tp_paper(), 0);
    s_bar_fill = lv_obj_create(s_bar);
    lv_obj_remove_style_all(s_bar_fill);
    lv_obj_clear_flag(s_bar_fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_bar_fill, tp_accent(), 0);
    lv_obj_set_style_bg_opa(s_bar_fill, LV_OPA_COVER, 0);
    lv_obj_set_pos(s_bar_fill, 0, 0);
    lv_obj_set_height(s_bar_fill, 12);
    s_hold_ms = 0;
    s_hold_spent = 0;
    bar_layout_fill();

    if (side > 0) {
        lv_obj_t *p = lv_obj_create(s_root);
        lv_obj_remove_style_all(p);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(p, lw, 0);
        lv_obj_set_size(p, side, h);
        lv_obj_set_style_bg_color(p, tp_paper(), 0);
        lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(p, tp_ink(), 0);
        char dow[8], day[4], mon[8];
        date_parts(dow, sizeof(dow), day, sizeof(day), mon, sizeof(mon));
        lv_obj_t *l1 = tp_label(p, dow, &tp_font_bold_26, NULL);
        lv_obj_set_pos(l1, 10, 14);
        lv_obj_t *l2 = tp_label(p, day, &tp_font_date_big, NULL);
        lv_obj_set_style_text_letter_space(l2, -2, 0);
        lv_obj_set_pos(l2, 8, 44);
        lv_obj_t *l3 = tp_label(p, mon, &tp_font_bold_26, NULL);
        lv_obj_set_pos(l3, 10, 90);
        int pct = 0;
        char bt[16] = "";
        int have_batt = purr_battery_get(&pct, NULL);       /* a board with no gauge (Playtime today) shows no battery at all, not an empty one */
        if (have_batt) {
            snprintf(bt, sizeof(bt), "BATTERY %d%%", pct);
        }
        lv_obj_t *bl = tp_label(p, bt, &tp_font_mono_12, NULL);
        mono(bl);
        lv_obj_set_pos(bl, 10, h - 56);
        lv_obj_t *bb = lv_obj_create(p);                /* the battery bar of the design, and under it whether the wire or the radio is up */
        if (!have_batt) {
            lv_obj_add_flag(bb, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_remove_style_all(bb);
        lv_obj_clear_flag(bb, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(bb, 10, h - 40);
        lv_obj_set_size(bb, side - 20, 10);
        lv_obj_set_style_border_width(bb, 2, 0);
        lv_obj_set_style_border_color(bb, tp_ink(), 0);
        lv_obj_t *bf = lv_obj_create(bb);
        lv_obj_remove_style_all(bf);
        lv_obj_clear_flag(bf, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(bf, tp_ink(), 0);
        lv_obj_set_style_bg_opa(bf, LV_OPA_COVER, 0);
        lv_obj_set_size(bf, bt[0] ? (side - 24) * pct / 100 + 1 : 1, 6);
        lv_obj_align(bf, LV_ALIGN_LEFT_MID, 0, 0);
        purr_net_status_t ns;
        purr_net_status(&ns);
        lv_obj_t *wl = tp_label(p, ns.connected ? "WI-FI ON" : "WI-FI OFF", &tp_font_mono_12, NULL);
        mono(wl);
        lv_obj_set_pos(wl, 10, h - 24);
    }
    tp_pad_hint(1 << PURR_PAD_FUNC);                    /* only FUNC does anything here */
}

void tp_lock_show(void)
{
    if (s_modal != M_NONE || s_root == NULL) {
        return;
    }
    lock_screen();                                      /* the lists and the home screen keep their focus: it is only paused */
}

void tp_lock_unlock(void)
{
    if (s_modal == M_NONE) {
        return;
    }
    root_hide();
    if (tp_home_showing()) {
        tp_home_focus();
    }
}

int tp_lock_is_on(void)
{
    return s_modal == M_LOCK || s_modal == M_PIN_UNLOCK || (s_modal == M_SI_PASS && s_si_fixed);
}

int tp_modal_up(void)
{
    return s_modal != M_NONE;
}

/* ---------------------------------------------------------------- the PIN pad */

static const char *pin_title(modal_t m)
{
    return m == M_PIN_NEW ? "NEW PIN" : m == M_PIN_REPEAT ? "REPEAT THE PIN" : m == M_PIN_REMOVE ? "REMOVE THE PIN" : "UNLOCK";
}

static void pin_dots(void)
{
    for (int i = 0; i < PURR_PIN_LEN; i++) {
        if (s_pin_dot[i] != NULL) {
            lv_obj_set_style_bg_color(s_pin_dot[i], tp_paper(), 0);
            lv_obj_set_style_bg_opa(s_pin_dot[i], i < s_npin ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
    }
}

static void pin_say(const char *t)
{
    if (s_pin_msg != NULL) {
        lv_label_set_text(s_pin_msg, t);
    }
}

static void pin_reset_entry(void)
{
    s_npin = 0;
    memset(s_pin, 0, sizeof(s_pin));
    pin_dots();
}

static void pin_complete(void);

static void pin_key_cb(void *ud, tp_key_kind_t kind, char ch)
{
    (void)ud;
    if (kind == TP_KEY_CHAR && s_npin < PURR_PIN_LEN) {
        s_pin[s_npin++] = ch;
        s_pin[s_npin] = '\0';
        pin_dots();
        if (s_npin == PURR_PIN_LEN) {
            pin_complete();
        }
    } else if (kind == TP_KEY_DEL && s_npin > 0) {
        s_pin[--s_npin] = '\0';
        pin_dots();
    } else if (kind == TP_KEY_OK) {
        if (s_npin == PURR_PIN_LEN) {
            pin_complete();
        } else {
            pin_say("FOUR DIGITS");
        }
    }
}

static void pin_screen(modal_t mode)
{
    s_modal = mode;
    root_prepare(1);
    int w = w10m_disp_w(), h = w10m_disp_h();
    int pw = w > 260 ? 176 : w - 20;
    padlock(s_root, 14, 10);                            /* the padlock and the name of the job, then the question, as in the design */
    lv_obj_t *lock = tp_label(s_root, pin_title(mode), &tp_font_mono_12, NULL);
    mono(lock);
    lv_obj_set_pos(lock, 34, 9);
    lv_obj_t *q = tp_label(s_root, mode == M_PIN_NEW ? "Choose four\ndigits" : (mode == M_PIN_REPEAT ? "Once more" : "Enter your\nPIN"), &tp_font_bold_26, NULL);
    lv_obj_set_style_text_letter_space(q, -1, 0);
    lv_obj_set_pos(q, 14, 40);
    for (int i = 0; i < PURR_PIN_LEN; i++) {
        lv_obj_t *d = lv_obj_create(s_root);
        lv_obj_remove_style_all(d);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(d, 22, 28);
        lv_obj_set_pos(d, 14 + i * 28, 116);
        lv_obj_set_style_border_width(d, 2, 0);
        lv_obj_set_style_border_color(d, tp_paper(), 0);
        s_pin_dot[i] = d;
    }
    s_pin_msg = tp_label(s_root, "", &tp_font_mono_12, NULL);
    mono(s_pin_msg);
    lv_obj_set_style_text_color(s_pin_msg, lv_color_hex(TP_MUTED), 0);
    lv_obj_set_pos(s_pin_msg, 14, 154);
    lv_obj_t *hint = tp_label(s_root, mode == M_PIN_UNLOCK ? "FUNC  USE PASSWORD\nB  DELETE" : "B  DELETE, OR BACK OUT", &tp_font_mono_12, NULL);
    mono(hint);
    lv_obj_set_pos(hint, 14, h - 36);

    lv_obj_t *panel = lv_obj_create(s_root);
    lv_obj_remove_style_all(panel);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(panel, w - pw - 8, 0);
    lv_obj_set_size(panel, pw + 8, h);
    lv_obj_set_style_bg_color(panel, tp_paper(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(panel, tp_ink(), 0);
    s_pad = tp_grid_pinpad(panel, 12, 14, pw - 16, h - 28, pin_key_cb, NULL);
    tp_grid_add_focus(s_pad);
    tp_focus_set(tp_grid_first(s_pad));
    pin_reset_entry();
    if (mode == M_PIN_UNLOCK) {
        uint32_t wait = purr_pin_guard_wait(&s_guard, now_ms());
        if (wait > 0) {
            char m[32];
            snprintf(m, sizeof(m), "WAIT %u S", (unsigned)wait);
            pin_say(m);
        }
    }
    tp_pad_hint((1 << PURR_PAD_UP) | (1 << PURR_PAD_DOWN) | (1 << PURR_PAD_LEFT) | (1 << PURR_PAD_RIGHT) | (1 << PURR_PAD_A) | (1 << PURR_PAD_B) | (1 << PURR_PAD_FUNC));
}

static void pin_close_setup(const char *banner)
{
    root_hide();
    if (banner != NULL) {
        w10m_banner(banner, 0);
    }
    if (tp_home_showing()) {
        tp_home_focus();
    }
}

static void pin_complete(void)
{
    char rec[PURR_PIN_RECORD_MAX + 4];
    switch (s_modal) {
    case M_PIN_UNLOCK: {
        uint32_t wait = purr_pin_guard_wait(&s_guard, now_ms());
        if (wait > 0) {
            char m[32];
            snprintf(m, sizeof(m), "WAIT %u S", (unsigned)wait);
            pin_reset_entry();
            pin_say(m);
            return;
        }
        if (pin_read(rec, sizeof(rec)) && purr_pin_check(rec, s_pin)) {
            purr_pin_guard_ok(&s_guard);
            tp_lock_unlock();
            return;
        }
        purr_pin_guard_fail(&s_guard, now_ms());
        pin_reset_entry();
        if (purr_pin_guard_locked_out(&s_guard)) {
            signin_pass_for(purr_login_current()->name, "", 1);           /* too many wrong PINs: only the password will do */
            return;
        }
        uint32_t w2 = purr_pin_guard_wait(&s_guard, now_ms());
        char m[40];
        if (w2 > 0) {
            snprintf(m, sizeof(m), "WRONG PIN. WAIT %u S", (unsigned)w2);
        } else {
            snprintf(m, sizeof(m), "WRONG PIN. %d TRIES LEFT", PURR_PIN_MAX_FAILS - s_guard.fails);
        }
        pin_say(m);
        break;
    }
    case M_PIN_NEW:
        memcpy(s_pin_new, s_pin, sizeof(s_pin_new));
        pin_screen(M_PIN_REPEAT);
        break;
    case M_PIN_REPEAT:
        if (strcmp(s_pin, s_pin_new) == 0) {
            uint8_t salt[PURR_PIN_SALT_LEN];
            esp_fill_random(salt, sizeof(salt));
            if (purr_pin_make(s_pin, salt, rec, sizeof(rec)) == 0 && pin_write(rec) == 0) {
                memset(s_pin_new, 0, sizeof(s_pin_new));
                pin_close_setup("PIN set. The lock screen asks for it now.");
            } else {
                pin_close_setup("Could not save the PIN.");
            }
        } else {
            memset(s_pin_new, 0, sizeof(s_pin_new));
            pin_screen(M_PIN_NEW);
            pin_say("THEY DID NOT MATCH. AGAIN");
        }
        break;
    case M_PIN_REMOVE:
        if (pin_read(rec, sizeof(rec)) && purr_pin_check(rec, s_pin)) {
            pin_clear();
            pin_close_setup("PIN removed.");
        } else {
            pin_reset_entry();
            pin_say("WRONG PIN");
        }
        break;
    default:
        break;
    }
}

void tp_pin_setup_open(void)
{
    if (s_modal != M_NONE || s_root == NULL) {
        return;
    }
    pin_screen(tp_pin_is_set() ? M_PIN_REMOVE : M_PIN_NEW);
}

/* ---------------------------------------------------------------- the sign-in: the check runs on a task of its own, as the Android shell's does */

typedef struct {
    char name[PURR_USER_NAME_LEN];
    char pass[64];
    purr_login_result_t result;
    uint32_t wait_s;
    int fixed;
} signin_job_t;

static void signin_list(void);

static void pw_say(const char *t)
{
    if (s_pw_msg != NULL) {
        lv_label_set_text(s_pw_msg, t);
    }
}

static void pw_dots(void)
{
    for (int i = 0; i < 10; i++) {
        if (s_pw_dot[i] != NULL) {
            lv_obj_set_style_bg_color(s_pw_dot[i], tp_ink(), 0);
            lv_obj_set_style_bg_opa(s_pw_dot[i], i < s_npw ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
    }
}

static void *signin_done_cb(void *arg)
{
    signin_job_t *j = arg;
    s_si_busy = 0;
    if (j->result == PURR_LOGIN_OK) {
        memset(s_pw, 0, sizeof(s_pw));
        s_npw = 0;
        if (j->fixed) {                                  /* the password instead of the PIN: it was the lock, not a sign-in */
            purr_pin_guard_ok(&s_guard);
            tp_lock_unlock();
        } else {
            tp_signed_in_set();
            tp_signin_hidden_check();
            ESP_LOGI(TAG, "signed in as %s", j->name);
            purr_theme_load_user(j->name);
            purr_session_started();
            tp_home_reload();
            char dn[PURR_USER_NAME_LEN];
            if (purr_login_default_account(dn, sizeof(dn)) && strcmp(j->name, dn) == 0) {
                tp_note_add("Change the default password", "This account uses the password MEOW. Change it in Settings > Accounts.");
            }
            char msg[64];
            snprintf(msg, sizeof(msg), "Signed in as %s.", j->name);
            tp_note_add(PURR_BRAND_PRODUCT, msg);
            char why[400];
            if (purr_resetnote_pending(NULL, 0, why, sizeof(why))) {
                tp_note_add("Your data was reset", why);
            }
        }
    } else if (j->result == PURR_LOGIN_WAIT) {
        char msg[40];
        snprintf(msg, sizeof(msg), "WAIT %u S", (unsigned)j->wait_s);
        s_npw = 0;
        memset(s_pw, 0, sizeof(s_pw));
        pw_dots();
        pw_say(msg);
    } else {
        s_npw = 0;
        memset(s_pw, 0, sizeof(s_pw));
        pw_dots();
        pw_say("WRONG PASSWORD");
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
    purr_ui_service_run(signin_done_cb, j, NULL);        /* frees j on the UI task */
    vTaskDelete(NULL);
}

static void pw_submit(void)
{
    if (s_si_busy || s_modal != M_SI_PASS) {
        return;
    }
    if (s_npw == 0) {
        pw_say("TYPE PASSWORD");
        return;
    }
    signin_job_t *j = calloc(1, sizeof(*j));
    if (j == NULL) {
        pw_say("OUT OF MEMORY");
        return;
    }
    snprintf(j->name, sizeof(j->name), "%s", s_si_name);
    memcpy(j->pass, s_pw, sizeof(j->pass));
    j->fixed = s_si_fixed;
    s_si_busy = 1;
    pw_say("SIGNING IN...");
    if (xTaskCreate(signin_task, "tp-signin", 6144, j, 4, NULL) != pdPASS) {
        s_si_busy = 0;
        pw_say("TRY AGAIN");
        free(j);
    }
}

static void pw_back(void)
{
    if (s_si_busy) {
        return;
    }
    if (s_si_fixed) {
        pin_screen(M_PIN_UNLOCK);                        /* back to the PIN pad */
    } else {
        signin_list();
    }
}

static void pw_key_cb(void *ud, tp_key_kind_t kind, char ch)
{
    (void)ud;
    if (s_si_busy) {
        return;
    }
    if ((kind == TP_KEY_CHAR || kind == TP_KEY_SPACE) && s_npw < (int)sizeof(s_pw) - 1) {
        s_pw[s_npw++] = ch;
        s_pw[s_npw] = '\0';
    } else if (kind == TP_KEY_DEL && s_npw > 0) {
        s_pw[--s_npw] = '\0';
    }
    pw_dots();
    if (kind == TP_KEY_CHAR || kind == TP_KEY_DEL) {
        pw_say("");
    }
}

static void pw_back_clicked(lv_event_t *e)
{
    (void)e;
    pw_back();
}

static void pw_go_clicked(lv_event_t *e)
{
    (void)e;
    pw_submit();
}

static void signin_pass_for(const char *name, const char *role, int fixed)
{
    s_modal = M_SI_PASS;
    s_si_fixed = fixed;
    snprintf(s_si_name, sizeof(s_si_name), "%s", name);
    snprintf(s_si_role, sizeof(s_si_role), "%s", role);
    s_npw = 0;
    memset(s_pw, 0, sizeof(s_pw));
    s_si_busy = 0;
    root_prepare(0);
    int w = w10m_disp_w(), h = w10m_disp_h();
    title_bar(fixed ? "UNLOCK WITH PASSWORD" : "PASSWORD");

    lv_obj_t *av = lv_obj_create(s_root);                /* the account's initial in an ink square */
    lv_obj_remove_style_all(av);
    lv_obj_clear_flag(av, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(av, 8, 24);
    lv_obj_set_size(av, 30, 30);
    lv_obj_set_style_bg_color(av, tp_ink(), 0);
    lv_obj_set_style_bg_opa(av, LV_OPA_COVER, 0);
    char ini[2] = {(char)toupper((unsigned char)(name[0] ? name[0] : '?')), 0};
    lv_color_t acc = tp_accent();
    lv_obj_t *il = tp_label(av, ini, &tp_font_bold_20, &acc);
    lv_obj_center(il);
    lv_obj_t *nm = tp_label(s_root, name, &tp_font_bold_20, NULL);
    lv_obj_set_pos(nm, 46, 22);
    lv_obj_t *rl = tp_label(s_root, role, &tp_font_mono_12, NULL);
    mono(rl);
    lv_obj_set_pos(rl, 47, 44);

    lv_obj_t *back = tp_box(s_root, TP_FILL_SAND);
    lv_obj_set_pos(back, w - 8 - 64 - 4 - 64, 26);
    lv_obj_set_size(back, 64, 26);
    lv_obj_t *bl = tp_label(back, "BACK", &tp_font_mono_12, NULL);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, pw_back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *go = tp_box(s_root, TP_FILL_INK);
    lv_obj_set_pos(go, w - 8 - 64, 26);
    lv_obj_set_size(go, 64, 26);
    lv_obj_t *gl = tp_label(go, "SIGN IN", &tp_font_mono_12, NULL);
    lv_obj_center(gl);
    lv_obj_add_event_cb(go, pw_go_clicked, LV_EVENT_CLICKED, NULL);

    s_pw_msg = tp_label(s_root, "", &tp_font_mono_12, NULL);
    mono(s_pw_msg);
    lv_obj_set_pos(s_pw_msg, 8, 60);
    lv_label_set_long_mode(s_pw_msg, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_pw_msg, w - 8 - 10 * 14 - 16, 16);   /* one line, so it can never run under the keys */
    lv_label_set_text(s_pw_msg, "TYPE PASSWORD");
    for (int i = 0; i < 10; i++) {
        lv_obj_t *d = lv_obj_create(s_root);
        lv_obj_remove_style_all(d);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(d, 11, 15);
        lv_obj_set_pos(d, w - 8 - (10 - i) * 14 + 3, 58);
        lv_obj_set_style_border_width(d, 1, 0);
        lv_obj_set_style_border_color(d, tp_ink(), 0);
        s_pw_dot[i] = d;
    }
    pw_dots();

    int ky = 82;
    s_kbd = tp_grid_keyboard(s_root, 6, ky, w - 12, h - ky - 6, pw_key_cb, NULL);
    tp_grid_add_focus(s_kbd);
    tp_focus_add(back);
    tp_focus_add(go);
    tp_focus_set(tp_grid_first(s_kbd));
    tp_pad_hint((1 << PURR_PAD_UP) | (1 << PURR_PAD_DOWN) | (1 << PURR_PAD_LEFT) | (1 << PURR_PAD_RIGHT) | (1 << PURR_PAD_A) | (1 << PURR_PAD_B) | (1 << PURR_PAD_X) |
                (1 << PURR_PAD_FUNC));
}

/* ---------------------------------------------------------------- the account list */

static const char *role_text(purr_user_role_t r)
{
    return r == PURR_ROLE_USER_ADMIN ? "ADMINISTRATOR" : "STANDARD USER";
}

static void row_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    const purr_user_list_t *ul = purr_login_users();
    if (i >= 0 && i < ul->count) {
        signin_pass_for(ul->users[i].name, role_text(ul->users[i].role), 0);
    }
}

static void signin_list(void)
{
    s_modal = M_SI_LIST;
    s_si_fixed = 0;
    root_prepare(0);
    int w = w10m_disp_w(), h = w10m_disp_h();
    int pw = w > 260 ? 110 : 0;
    title_bar("SIGN IN");
    lv_obj_t *list = lv_obj_create(s_root);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, TP_STRIP_H);
    lv_obj_set_size(list, w - pw, h - TP_STRIP_H - TP_FOOT_H - 30);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(list, tp_ink(), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_top(list, 3, 0);

    const purr_user_list_t *ul = purr_login_users();
    lv_obj_t *first = NULL;
    lv_color_t acc = tp_accent();
    for (int i = 0; i < ul->count && i < PURR_USER_MAX; i++) {
        lv_obj_t *b = tp_box(list, TP_FILL_NONE);
        lv_obj_set_size(b, LV_PCT(100), 42);
        lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_pad_hor(b, 8, 0);
        static const lv_state_t chosen[2] = {LV_STATE_FOCUSED, LV_STATE_PRESSED};     /* the chosen row is an ink bar with paper text, as in the app list */
        for (int k = 0; k < 2; k++) {
            lv_obj_set_style_bg_color(b, tp_ink(), chosen[k]);
            lv_obj_set_style_text_color(b, tp_paper(), chosen[k]);
        }
        lv_obj_t *av = lv_obj_create(b);
        lv_obj_remove_style_all(av);
        lv_obj_clear_flag(av, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(av, 28, 28);
        lv_obj_set_style_bg_color(av, tp_ink(), 0);
        lv_obj_set_style_bg_opa(av, LV_OPA_COVER, 0);
        lv_obj_align(av, LV_ALIGN_LEFT_MID, 0, 0);
        char ini[2] = {(char)toupper((unsigned char)(ul->users[i].name[0] ? ul->users[i].name[0] : '?')), 0};
        lv_obj_t *il = tp_label(av, ini, &tp_font_bold_20, &acc);
        lv_obj_center(il);
        lv_obj_t *nm = tp_label(b, ul->users[i].name, &tp_font_bold_20, NULL);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 38, -7);
        lv_obj_t *rl = tp_label(b, role_text(ul->users[i].role), &tp_font_mono_12, NULL);
        mono(rl);
        lv_obj_align(rl, LV_ALIGN_LEFT_MID, 38, 10);
        lv_obj_t *c = tp_label(b, LV_SYMBOL_RIGHT, &lv_font_montserrat_12, NULL);
        lv_obj_align(c, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_event_cb(b, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        tp_focus_add(b);
        if (first == NULL) {
            first = b;
        }
    }
    char note[120] = "", reset[96], dn[PURR_USER_NAME_LEN];
    if (purr_login_store_unreadable()) {
        snprintf(note, sizeof(note), "CAN'T READ THE ACCOUNT LIST. YOUR FILES ARE SAFE. RESTART TO TRY AGAIN.");
    } else if (purr_resetnote_pending(reset, sizeof(reset), NULL, 0)) {
        snprintf(note, sizeof(note), "%s", reset);
    } else if (purr_login_default_account(dn, sizeof(dn))) {
        snprintf(note, sizeof(note), "DEFAULT ACCOUNT: MEOW, PASSWORD MEOW. CHANGE IT IN SETTINGS.");
    }
    lv_obj_t *nl = tp_label(s_root, note, &tp_font_mono_12, NULL);
    lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);        /* dots need a fixed height: two lines of 12 pixels, then "..." */
    lv_obj_set_size(nl, w - pw - 16, 30);
    lv_obj_set_pos(nl, 8, h - TP_FOOT_H - 30);

    if (pw > 0) {
        lv_obj_t *p = lv_obj_create(s_root);
        lv_obj_remove_style_all(p);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(p, w - pw, TP_STRIP_H);
        lv_obj_set_size(p, pw, h - TP_STRIP_H - TP_FOOT_H);
        lv_obj_set_style_bg_color(p, tp_accent(), 0);
        lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(p, 1, 0);
        lv_obj_set_style_border_side(p, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(p, tp_ink(), 0);
        lv_obj_set_style_text_color(p, tp_on_accent(), 0);
        panel_clock(p);
        lv_obj_t *who = tp_label(p, "WHO IS\nSIGNING IN?", &tp_font_mono_12, NULL);
        mono(who);
        lv_obj_align(who, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    }
    {
        static const tp_hint_t keys[] = {{LV_SYMBOL_UP LV_SYMBOL_DOWN, "CHOOSE"}, {"A", "OPEN"}};
        lv_obj_t *fb = tp_footer(s_root, keys, 2);
        lv_obj_set_pos(fb, 0, h - TP_FOOT_H);
        lv_obj_set_width(fb, w);
    }
    clock_refresh();
    tp_focus_set(first);
    tp_pad_hint((1 << PURR_PAD_UP) | (1 << PURR_PAD_DOWN) | (1 << PURR_PAD_A));
}

void tp_signin_hidden_check(void)
{
    if (s_modal == M_SI_LIST || s_modal == M_SI_PASS) {
        root_hide();
    }
    if (tp_home_showing()) {
        tp_home_focus();
    }
}

/* ---------------------------------------------------------------- keys */

/* Whether Enter is the gamepad's A (it clicks the highlighted key) or a keyboard's Enter (it submits). */
static int enter_is_a(void)
{
    return tp_pad() != NULL;
}

int tp_modal_key(uint32_t key)
{
    switch (s_modal) {
    case M_NONE:
        return 0;
    case M_LOCK:
        if (key == LV_KEY_ENTER && !enter_is_a()) {      /* a keyboard board: Enter unlocks; the gamepad needs FUNC held */
            unlock_attempt();
        }
        return 1;
    case M_SI_LIST:
        if (key == LV_KEY_ENTER && !enter_is_a() && tp_focus_get() != NULL) {
            lv_obj_send_event(tp_focus_get(), LV_EVENT_CLICKED, NULL);
            return 1;
        }
        tp_focus_key(key);
        return 1;
    case M_SI_PASS:
        if (s_si_busy) {
            return 1;
        }
        if (key == LV_KEY_ENTER && !enter_is_a()) {
            pw_submit();
        } else if (key == LV_KEY_ESC) {
            if (s_npw > 0 && enter_is_a()) {              /* the gamepad's B deletes, and backs out when there is nothing to delete */
                pw_key_cb(NULL, TP_KEY_DEL, 0);
            } else {
                pw_back();
            }
        } else if (key == PURR_KEY_MENU) {
            tp_grid_cycle(s_kbd);                         /* X: abc, ABC, symbols */
        } else if (key == LV_KEY_NEXT) {
            pw_submit();                                  /* FUNC signs in */
        } else if (key == LV_KEY_BACKSPACE || key == 127) {
            pw_key_cb(NULL, TP_KEY_DEL, 0);
        } else if (key >= 0x20 && key < 0x7F) {
            pw_key_cb(NULL, key == ' ' ? TP_KEY_SPACE : TP_KEY_CHAR, (char)key);   /* a hardware keyboard types straight in */
        } else {
            tp_focus_key(key);
        }
        return 1;
    case M_PIN_UNLOCK:
    case M_PIN_NEW:
    case M_PIN_REPEAT:
    case M_PIN_REMOVE:
        if (key == LV_KEY_ESC) {
            if (s_npin > 0) {
                pin_key_cb(NULL, TP_KEY_DEL, 0);
            } else if (s_modal == M_PIN_UNLOCK) {
                lock_screen();                            /* back to the lock screen */
            } else {
                pin_close_setup(NULL);                    /* a setup is cancelled */
            }
        } else if (key == LV_KEY_NEXT && s_modal == M_PIN_UNLOCK) {
            signin_pass_for(purr_login_current()->name, "", 1);
        } else if (key >= '0' && key <= '9') {
            pin_key_cb(NULL, TP_KEY_CHAR, (char)key);
        } else if (key == LV_KEY_BACKSPACE || key == 127) {
            pin_key_cb(NULL, TP_KEY_DEL, 0);
        } else if (key == LV_KEY_ENTER && !enter_is_a()) {
            pin_key_cb(NULL, TP_KEY_OK, 0);
        } else {
            tp_focus_key(key);
        }
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- the poll: the hold on the lock screen, the clock, the pause on the PIN pad */

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    s_ticks++;
    if (s_modal == M_LOCK && s_bar != NULL) {
        const purr_gamepad_ops_t *p = tp_pad();
        int holding = (p != NULL && (p->buttons(tp_pad_handle()) & (1u << PURR_PAD_FUNC))) || lv_obj_has_state(s_bar, LV_STATE_PRESSED);
        if (holding) {
            if (!s_hold_spent) {
                s_hold_ms += POLL_MS;
                if (s_hold_ms >= HOLD_MS) {
                    s_hold_ms = HOLD_MS;
                    s_hold_spent = 1;
                    bar_layout_fill();
                    unlock_attempt();
                    return;
                }
            }
        } else {
            s_hold_spent = 0;
            s_hold_ms = s_hold_ms > 3 * POLL_MS ? s_hold_ms - 3 * POLL_MS : 0;      /* it drains faster than it fills */
        }
        bar_layout_fill();
    }
    if (s_ticks % (1000 / POLL_MS) == 0) {
        clock_refresh();
        if (s_modal == M_PIN_UNLOCK && s_pin_msg != NULL) {
            uint32_t wait = purr_pin_guard_wait(&s_guard, now_ms());
            if (wait > 0) {
                char m[32];
                snprintf(m, sizeof(m), "WAIT %u S", (unsigned)wait);
                pin_say(m);
            }
        }
    }
}

/* ---------------------------------------------------------------- building, layout */

static void screen_for_modal(void)
{
    switch (s_modal) {
    case M_LOCK: lock_screen(); break;
    case M_SI_LIST: signin_list(); break;
    case M_SI_PASS: {
        char name[PURR_USER_NAME_LEN], role[24];
        snprintf(name, sizeof(name), "%s", s_si_name);
        snprintf(role, sizeof(role), "%s", s_si_role);
        signin_pass_for(name, role, s_si_fixed);
        break;
    }
    case M_PIN_UNLOCK:
    case M_PIN_NEW:
    case M_PIN_REPEAT:
    case M_PIN_REMOVE: pin_screen(s_modal); break;
    default: break;
    }
}

void tp_lock_build(void)
{
    s_root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_root);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);       /* nothing beneath it can be touched */
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    s_poll = lv_timer_create(poll_cb, POLL_MS, NULL);
    s_modal = M_NONE;
}

void tp_lock_layout(void)
{
    if (s_root != NULL && s_modal != M_NONE) {
        screen_for_modal();                               /* the screen turned: draw the one that is up again at the new size */
    }
}

void tp_signin_build(void)
{
    if (!tp_signed_in()) {
        signin_list();                                    /* covers everything until someone signs in */
    }
}

void tp_signin_layout(void)
{
    /* the lock layout call redraws whichever of the three is up */
}
