/*
 * purr_tp_nav.c - the Type shell's d-pad for everything that is not the shell itself (plan7.md, "The Type shell", "The d-pad for every sheet").
 *
 * Settings, the Developer Portal, the task manager and every app window (Store, Notes, Files, ...) are built from the shared widgets, and none of them was
 * written for a d-pad. The shell does not ask them to be: the layout and the input belong to the theme. This file walks the sheet in front, takes every control
 * on it (a switch, a slider, a drop-down, a text box, anything clickable that somebody listens to), and keeps one highlight, drawn as a ring over the control
 * and not as a style on it, so no widget or app needs a focus look of its own.
 *
 *   arrows       the nearest control in that direction (wrapping at the edges); with none that way, the list under it scrolls
 *   Enter (A)    presses it: a button clicks, a switch flips, a drop-down opens and, open, chooses
 *   Esc (B)      closes an open drop-down, or the on-screen keyboard; otherwise it is the shell's Back
 *   text box     Enter opens the on-screen keyboard (the key grid of the sign-in) over the sheet: type, OK
 *
 * UI service task only.
 */
#include "purr_tp_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "purr_catrt_ui.h"
#include "purr_lvgl.h"

static const char *TAG = "dpad";

#define NAV_MAX     96
#define HEAD_H      30                          /* the top of a sheet is its title bar (30 px in the shared sheets): the highlight starts under it */
#define RING_PAD    2

static lv_obj_t *s_c[NAV_MAX];                  /* the controls of the sheet in front, in tree order */
static int s_nc;
static lv_obj_t *s_cur;                         /* the highlighted one */
static lv_obj_t *s_ring;
static lv_timer_t *s_timer;

static lv_obj_t *s_osk;                         /* the on-screen keyboard, while it is up */
static lv_obj_t *s_osk_field;
static lv_obj_t *s_osk_text;
static lv_obj_t *s_osk_target;                  /* the text box it edits */
static tp_grid_t *s_osk_grid;

/* ---------------------------------------------------------------- finding the controls */

static lv_obj_t *(*s_root_fn)(void);            /* a screen that is not a shell sheet (the recovery app, the setup wizard) names its own root */
static int s_root_head;
static int (*s_back_fn)(void);

void tp_nav_set_root(lv_obj_t *(*fn)(void), int head_h)
{
    s_root_fn = fn;
    s_root_head = head_h;
}

void tp_nav_set_back(int (*fn)(void))
{
    s_back_fn = fn;
}

static int head_h(void)
{
    return s_root_fn != NULL ? s_root_head : HEAD_H;
}

static lv_obj_t *front_root(void)
{
    if (s_root_fn != NULL) {
        return s_root_fn();
    }
    lv_obj_t *r = w10m_tasks_root();
    if (r == NULL) {
        r = purr_catrt_ui_front();
    }
    if (r == NULL) {
        r = w10m_devportal_root();
    }
    if (r == NULL) {
        r = w10m_settings_root();
    }
    return r;
}

static int is_input(const lv_obj_t *o)
{
    return lv_obj_has_class(o, &lv_slider_class) || lv_obj_has_class(o, &lv_dropdown_class) || lv_obj_has_class(o, &lv_switch_class) ||
           lv_obj_has_class(o, &lv_textarea_class);
}

static int interactive(lv_obj_t *o)
{
    if (lv_obj_has_state(o, LV_STATE_DISABLED)) {
        return 0;
    }
    if (is_input(o)) {
        return 1;
    }
    return lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) && lv_obj_get_event_count(o) > 0;     /* a plain container is clickable too: only one somebody listens to counts */
}

static void walk(lv_obj_t *o)
{
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n && s_nc < NAV_MAX; i++) {
        lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
        if (c == NULL || lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN) || c == s_ring) {
            continue;
        }
        if (lv_obj_get_width(c) > 0 && lv_obj_get_height(c) > 0 && interactive(c)) {
            s_c[s_nc++] = c;
        }
        walk(c);
    }
}

static void collect(lv_obj_t *root)
{
    s_nc = 0;
    if (root != NULL) {
        walk(root);
    }
}

static int listed(const lv_obj_t *o)
{
    for (int i = 0; o != NULL && i < s_nc; i++) {
        if (s_c[i] == o) {
            return 1;
        }
    }
    return 0;
}

/* Where to start: the first control under the title bar (so Enter does not close the sheet by accident), else the first. */
static lv_obj_t *first_control(lv_obj_t *root)
{
    lv_area_t r, a;
    lv_obj_get_coords(root, &r);
    lv_obj_t *best = NULL;
    int by = 0, bx = 0;
    for (int i = 0; i < s_nc; i++) {
        lv_obj_get_coords(s_c[i], &a);
        if ((a.y1 + a.y2) / 2 < r.y1 + head_h()) {          /* inside the title bar */
            continue;
        }
        if (best == NULL || a.y1 < by || (a.y1 == by && a.x1 < bx)) {
            best = s_c[i];
            by = a.y1;
            bx = a.x1;
        }
    }
    return best != NULL ? best : (s_nc > 0 ? s_c[0] : NULL);
}

static void set_cur(lv_obj_t *o)
{
    s_cur = o;
    if (o == NULL) {
        return;
    }
    lv_obj_scroll_to_view_recursive(o, LV_ANIM_OFF);
    lv_group_t *g = lv_obj_get_group(o);
    if (g != NULL && lv_obj_has_class(o, &lv_textarea_class)) {
        lv_group_focus_obj(o);                  /* a hardware keyboard types into the box that is highlighted */
    }
}

static void validate(lv_obj_t *root)
{
    if (!listed(s_cur)) {
        set_cur(first_control(root));
    }
}

/* ---------------------------------------------------------------- the highlight */

static void ring_hide(void)
{
    if (s_ring != NULL) {
        lv_obj_add_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ring_make(void)
{
    s_ring = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_ring);
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ring, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(s_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_ring, 0, 0);
    lv_obj_set_style_border_width(s_ring, 2, 0);
    lv_obj_set_style_border_color(s_ring, tp_ink(), 0);
    lv_obj_set_style_outline_width(s_ring, 2, 0);
    lv_obj_set_style_outline_pad(s_ring, 0, 0);
    lv_obj_set_style_outline_opa(s_ring, LV_OPA_COVER, 0);
}

/* Ink inside, the accent around it: reads on paper, on the accent and on ink. */
static void ring_update(lv_obj_t *root)
{
    if (s_ring == NULL) {
        ring_make();
    }
    if (root == NULL || s_cur == NULL || s_osk != NULL || w10m_perm_is_open()) {      /* a permission question is above everything, the ring included */
        ring_hide();
        return;
    }
    lv_area_t a, r;
    lv_obj_get_coords(s_cur, &a);
    lv_obj_get_coords(root, &r);
    if (a.y2 < r.y1 + head_h() / 2 || a.y1 > r.y2 || a.x2 < r.x1 || a.x1 > r.x2) {       /* scrolled out of the sheet: no ring */
        ring_hide();
        return;
    }
    lv_obj_set_style_border_color(s_ring, tp_ink(), 0);
    lv_obj_set_style_outline_color(s_ring, tp_accent(), 0);
    lv_obj_set_pos(s_ring, a.x1 - RING_PAD, a.y1 - RING_PAD);
    lv_obj_set_size(s_ring, a.x2 - a.x1 + 1 + 2 * RING_PAD, a.y2 - a.y1 + 1 + 2 * RING_PAD);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ring);
}

/* ---------------------------------------------------------------- moving */

static void centre(const lv_obj_t *o, int *cx, int *cy)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    *cx = (a.x1 + a.x2) / 2;
    *cy = (a.y1 + a.y2) / 2;
}

/* The nearest control in the direction of `key` (along plus three times across, as the shell's own focus manager does); with none that way, the far side's most
 * aligned one, and *wrapped says so. */
static lv_obj_t *neighbour(const lv_obj_t *from, uint32_t key, int *wrapped)
{
    int fx, fy;
    centre(from, &fx, &fy);
    lv_obj_t *best = NULL, *wrap = NULL;
    long best_s = 0, wrap_s = 0;
    for (int i = 0; i < s_nc; i++) {
        lv_obj_t *o = s_c[i];
        if (o == from) {
            continue;
        }
        int x, y;
        centre(o, &x, &y);
        int dx = x - fx, dy = y - fy, along, across;
        switch (key) {
        case LV_KEY_RIGHT: along = dx; across = dy; break;
        case LV_KEY_LEFT:  along = -dx; across = dy; break;
        case LV_KEY_DOWN:  along = dy; across = dx; break;
        default:           along = -dy; across = dx; break;
        }
        if (across < 0) {
            across = -across;
        }
        if (along > 0) {
            long s = (long)along + 3L * across;
            if (best == NULL || s < best_s) {
                best = o;
                best_s = s;
            }
        } else {
            long s = 3L * across - (long)(-along);
            if (wrap == NULL || s < wrap_s) {
                wrap = o;
                wrap_s = s;
            }
        }
    }
    *wrapped = best == NULL;
    return best != NULL ? best : wrap;
}

/* Scrolls the nearest scrollable parent of `from` (or, with none, the first scrollable thing in the sheet) by a third of its height; 1 if it moved. */
static lv_obj_t *scrollable_in(lv_obj_t *o, int dir)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) && (dir > 0 ? lv_obj_get_scroll_bottom(o) > 0 : lv_obj_get_scroll_top(o) > 0)) {
        return o;
    }
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
        if (c != NULL && !lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_t *f = scrollable_in(c, dir);
            if (f != NULL) {
                return f;
            }
        }
    }
    return NULL;
}

static int scroll_some(lv_obj_t *root, const lv_obj_t *from, int dir)
{
    lv_obj_t *target = NULL;
    for (lv_obj_t *p = from != NULL ? lv_obj_get_parent(from) : NULL; p != NULL && target == NULL; p = lv_obj_get_parent(p)) {
        if (lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE) && (dir > 0 ? lv_obj_get_scroll_bottom(p) > 0 : lv_obj_get_scroll_top(p) > 0)) {
            target = p;
        }
        if (p == root) {
            break;
        }
    }
    if (target == NULL) {
        target = scrollable_in(root, dir);
    }
    if (target == NULL) {
        return 0;
    }
    int step = lv_obj_get_height(target) / 3;
    lv_obj_scroll_by(target, 0, dir > 0 ? -step : step, LV_ANIM_OFF);
    return 1;
}

static int is_arrow(uint32_t k)
{
    return k == LV_KEY_UP || k == LV_KEY_DOWN || k == LV_KEY_LEFT || k == LV_KEY_RIGHT;
}

static int move(lv_obj_t *root, uint32_t key)
{
    if (s_cur == NULL) {
        return (key == LV_KEY_UP || key == LV_KEY_DOWN) ? scroll_some(root, NULL, key == LV_KEY_DOWN ? 1 : -1) : 1;
    }
    if (lv_obj_has_class(s_cur, &lv_dropdown_class) && lv_dropdown_is_open(s_cur)) {    /* an open drop-down is its own list: arrows choose */
        lv_obj_send_event(s_cur, LV_EVENT_KEY, &key);
        return 1;
    }
    if (lv_obj_has_class(s_cur, &lv_slider_class) && (key == LV_KEY_LEFT || key == LV_KEY_RIGHT)) {
        int lo = lv_slider_get_min_value(s_cur), hi = lv_slider_get_max_value(s_cur);
        int step = (hi - lo) / 20;
        step = step < 1 ? 1 : step;
        int v = lv_slider_get_value(s_cur) + (key == LV_KEY_RIGHT ? step : -step);
        v = v < lo ? lo : (v > hi ? hi : v);
        lv_slider_set_value(s_cur, v, LV_ANIM_OFF);
        if (lv_obj_send_event(s_cur, LV_EVENT_VALUE_CHANGED, NULL) == LV_RESULT_OK) {
            lv_obj_send_event(s_cur, LV_EVENT_RELEASED, purr_lvgl_keypad());            /* a slider that acts on release acts on each step */
        }
        return 1;
    }
    int wrapped = 0;
    lv_obj_t *n = neighbour(s_cur, key, &wrapped);
    if (wrapped && (key == LV_KEY_UP || key == LV_KEY_DOWN) && scroll_some(root, s_cur, key == LV_KEY_DOWN ? 1 : -1)) {
        return 1;                               /* nothing further that way: what is under the list comes into view first */
    }
    if (n != NULL) {
        set_cur(n);
    }
    return 1;
}

/* ---------------------------------------------------------------- Enter */

static void osk_open(lv_obj_t *ta);

static int activate(void)
{
    lv_obj_t *o = s_cur;
    if (o == NULL) {
        ESP_LOGI(TAG, "A with nothing highlighted");
        return 0;
    }
    ESP_LOGI(TAG, "A on %p", (void *)o);
    if (lv_obj_has_class(o, &lv_textarea_class)) {
        osk_open(o);
        return 1;
    }
    struct _lv_indev_t *kb = purr_lvgl_keypad();
    /* What a finger does, in the order it does it: a switch flips on the release, a button's handler hears the click. A handler may close the page it is on, which
     * deletes the control: lv_obj_send_event says so, and nothing more is sent. */
    if (lv_obj_send_event(o, LV_EVENT_PRESSED, kb) != LV_RESULT_OK) {
        return 1;
    }
    if (lv_obj_send_event(o, LV_EVENT_RELEASED, kb) != LV_RESULT_OK) {
        return 1;
    }
    lv_obj_send_event(o, LV_EVENT_CLICKED, kb);
    return 1;
}

/* ---------------------------------------------------------------- the on-screen keyboard */

static void osk_refresh(void)
{
    if (s_osk == NULL || s_osk_target == NULL) {
        return;
    }
    const char *t = lv_textarea_get_text(s_osk_target);
    int pw = lv_textarea_get_password_mode(s_osk_target);
    int n = (int)strlen(t);
    int room = (w10m_disp_w() - 32) / 9;                                  /* characters that fit in the field, about */
    room = room < 8 ? 8 : (room > 60 ? 60 : room);
    char buf[64];
    int from = n > room ? n - room : 0;
    int k = 0;
    for (int i = from; i < n && k < (int)sizeof(buf) - 2; i++) {
        buf[k++] = pw ? '*' : t[i];
    }
    buf[k++] = '_';
    buf[k] = '\0';
    lv_label_set_text(s_osk_text, buf);
}

static void osk_close(void)
{
    if (s_osk == NULL) {
        return;
    }
    tp_grid_free(s_osk_grid);
    s_osk_grid = NULL;
    lv_obj_delete(s_osk);
    s_osk = s_osk_field = s_osk_text = NULL;
    s_osk_target = NULL;
    tp_focus_clear();
}

static void osk_cb(void *ud, tp_key_kind_t kind, char ch)
{
    (void)ud;
    lv_obj_t *ta = s_osk_target;
    if (ta == NULL) {
        return;
    }
    switch (kind) {
    case TP_KEY_CHAR:
        if (ch != '\0') {
            lv_textarea_add_char(ta, (uint32_t)(uint8_t)ch);
        }
        break;
    case TP_KEY_SPACE:
        lv_textarea_add_char(ta, ' ');
        break;
    case TP_KEY_DEL:
        lv_textarea_delete_char(ta);
        break;
    case TP_KEY_OK:
        osk_close();                            /* the box is still there: Enter on a one-line box is "ready", as on a hardware keyboard */
        if (lv_textarea_get_one_line(ta)) {
            lv_obj_send_event(ta, LV_EVENT_READY, NULL);
        }
        return;
    default:
        break;
    }
    osk_refresh();
}

static void osk_open(lv_obj_t *ta)
{
    if (s_osk != NULL) {
        return;
    }
    int w = w10m_disp_w(), h = w10m_disp_h();
    s_osk_target = ta;
    s_osk = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_osk);
    lv_obj_clear_flag(s_osk, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_osk, LV_OBJ_FLAG_CLICKABLE);                      /* it swallows touches: the sheet under it is not reachable */
    lv_obj_set_size(s_osk, w, h);
    lv_obj_set_pos(s_osk, 0, 0);
    lv_obj_set_style_bg_color(s_osk, tp_paper(), 0);
    lv_obj_set_style_bg_opa(s_osk, LV_OPA_COVER, 0);

    const char *ph = lv_textarea_get_placeholder_text(ta);
    lv_color_t ink = tp_ink();
    lv_obj_t *cap = tp_label(s_osk, (ph != NULL && ph[0] != '\0') ? ph : "TYPE", &lv_font_montserrat_12, &ink);
    lv_obj_set_pos(cap, 8, 6);

    s_osk_field = lv_obj_create(s_osk);
    lv_obj_remove_style_all(s_osk_field);
    lv_obj_clear_flag(s_osk_field, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_osk_field, 6, 24);
    lv_obj_set_size(s_osk_field, w - 12, 28);
    lv_obj_set_style_border_width(s_osk_field, 2, 0);
    lv_obj_set_style_border_color(s_osk_field, ink, 0);
    lv_obj_set_style_bg_color(s_osk_field, lv_color_hex(TP_SAND), 0);
    lv_obj_set_style_bg_opa(s_osk_field, LV_OPA_COVER, 0);
    s_osk_text = tp_label(s_osk_field, "", &lv_font_montserrat_16, &ink);
    lv_obj_set_style_text_font(s_osk_text, &lv_font_montserrat_16, 0);
    lv_obj_align(s_osk_text, LV_ALIGN_LEFT_MID, 4, 0);

    s_osk_grid = tp_grid_keyboard(s_osk, 6, 58, w - 12, h - 58 - 6, osk_cb, NULL);
    tp_focus_clear();
    tp_grid_add_focus(s_osk_grid);
    tp_focus_set(tp_grid_first(s_osk_grid));
    osk_refresh();
    tp_pad_hint((1 << PURR_PAD_A) | (1 << PURR_PAD_B) | (1 << PURR_PAD_X));
    ring_hide();
}

/* A key while the keyboard is up. A = press, B = close, X = capitals and symbols; a hardware keyboard types straight in. Home passes on (and closes it). */
static int osk_key(uint32_t key)
{
    if (key == LV_KEY_HOME) {
        return 0;
    }
    if (key == LV_KEY_ESC) {
        osk_close();
        return 1;
    }
    if (key == PURR_KEY_MENU) {
        tp_grid_cycle(s_osk_grid);
        return 1;
    }
    if (tp_focus_key(key)) {
        return 1;
    }
    if (s_osk_target != NULL) {
        if (key == LV_KEY_BACKSPACE) {
            lv_textarea_delete_char(s_osk_target);
            osk_refresh();
        } else if (key >= 32 && key < 127) {
            lv_textarea_add_char(s_osk_target, key);
            osk_refresh();
        }
    }
    return 1;
}

/* ---------------------------------------------------------------- the entry points */

int tp_nav_key(uint32_t key)
{
    if (s_osk != NULL) {
        return osk_key(key);
    }
    lv_obj_t *root = front_root();
    if (root == NULL) {
        return 0;
    }
    collect(root);
    validate(root);
    if (key == LV_KEY_ESC && s_cur != NULL && lv_obj_has_class(s_cur, &lv_dropdown_class) && lv_dropdown_is_open(s_cur)) {
        lv_obj_send_event(s_cur, LV_EVENT_KEY, &key);                   /* an open drop-down closes first, back to what it was */
        return 1;
    }
    int used = 0;
    if (is_arrow(key)) {
        used = move(root, key);
    } else if (key == LV_KEY_ENTER) {
        used = activate();
    }
    root = front_root();                        /* the key may have closed the sheet, or changed the page */
    if (root == NULL) {
        s_cur = NULL;
        ring_hide();
        return used;
    }
    if (s_osk == NULL) {
        collect(root);
        validate(root);
        ring_update(root);
    }
    return used;
}

/* The highlight follows the sheet: it appears when one opens, starts on the first control of a new page, and goes when the sheet does. */
static void tick(lv_timer_t *t)
{
    (void)t;
    lv_obj_t *root = front_root();
    if (w10m_perm_is_open()) {                  /* the question is answered first; the highlight stays where it was */
        ring_hide();
        return;
    }
    if (root == NULL || !tp_signed_in() || tp_modal_up()) {
        if (s_osk != NULL) {
            osk_close();
        }
        s_cur = NULL;
        ring_hide();
        return;
    }
    collect(root);
    if (s_osk != NULL) {
        if (!listed(s_osk_target)) {            /* the page under it changed: the box it was editing is gone */
            osk_close();
        } else {
            return;
        }
    }
    validate(root);
    ring_update(root);
    static lv_obj_t *last_root, *last_cur;
    static int last_nc = -1;
    if (root != last_root || s_cur != last_cur || s_nc != last_nc) {           /* only a change is logged */
        lv_area_t a = {0};
        if (s_cur != NULL) {
            lv_obj_get_coords(s_cur, &a);
        }
        ESP_LOGI(TAG, "sheet %p: %d control(s), on %p (%d,%d %dx%d)", (void *)root, s_nc, (void *)s_cur, (int)a.x1, (int)a.y1, (int)(a.x2 - a.x1 + 1), (int)(a.y2 - a.y1 + 1));
        last_root = root;
        last_cur = s_cur;
        last_nc = s_nc;
    }
}

void tp_nav_reset(void)
{
    osk_close();
    s_cur = NULL;
    ring_hide();
}

void tp_nav_layout(void)
{
    tp_nav_reset();
}

int tp_nav_osk_up(void)
{
    return s_osk != NULL;
}

static lv_obj_t *screen_root(void)
{
    return lv_screen_active();
}

/* The hook for a board with a gamepad and no shell above it (the recovery app, the setup wizard, the internet loader): every control on the screen, arrows and A as
 * in the sheets, B for the screen's own Back if it has one. A shell that starts later puts its own hook in its place. UI task. */
int purr_dpad_key(uint32_t key)
{
    tp_nav_init();
    if (s_root_fn == NULL) {
        s_root_fn = screen_root;
        s_root_head = 0;
    }
    if (key == LV_KEY_ESC && s_osk == NULL) {
        return s_back_fn != NULL ? s_back_fn() : 0;
    }
    return tp_nav_key(key);
}

void tp_nav_init(void)
{
    if (s_timer == NULL) {
        s_timer = lv_timer_create(tick, 80, NULL);
    }
}
