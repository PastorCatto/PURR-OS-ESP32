/*
 * purr_catrt_ui.c - the `ui` catcall (purr_catcalls.h, plan9.md): the window a UI app draws in.
 *
 * Each UI app gets one window, a sheet over the start screen in the W10M theme: a header (back arrow, title,
 * one optional action) over a scrolling column. The app asks for widgets by calling the table below on its
 * own task; every call hops to the UI service task, where the LVGL objects live. The app sees integer
 * handles only, and what it makes is deleted when it stops. Taps and typing return as PURR_APPEV_UI events
 * posted to the app's own queue (never a callback into app code from LVGL).
 *
 * The window is created on the app's first call, shown when the runtime resumes the app, hidden when it
 * pauses, deleted when it is unloaded.
 */
#include "purr_catrt_ui.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"

#include "purr_catcalls.h"
#include "purr_catrt.h"
#include "purr_lvgl.h"
#include "purr_runtime.h"
#include "purr_ui_service.h"
#include "purr_w10m_priv.h"
#include "purr_w10m_theme.h"

#define TAG "ui-cc"
#define HEAD_H 30
#define MAX_LABEL 256

typedef struct {
    int used;
    char name[PURR_RT_NAME_LEN];
    lv_obj_t *sheet, *head, *title, *act, *body;
    lv_group_t *group;
    int back_app;
    int open;
    int count;
    lv_obj_t *w[PURR_CC_UI_MAX_WIDGETS];               /* handle = index + 1 */
} win_t;

static win_t s_win[PURR_RT_MAX_APPS];

typedef enum {
    OP_TITLE, OP_ACTION, OP_BACK, OP_CLEAR, OP_LABEL, OP_BUTTON, OP_ROW, OP_TEXTAREA, OP_SPACER,
    OP_SET_TEXT, OP_GET_TEXT, OP_FOCUS, OP_REMOVE,
} op_t;

typedef struct {
    op_t op;
    int slot;
    char name[PURR_RT_NAME_LEN];                    /* the calling app, for creating its window */
    const char *s1, *s2;
    int i1, i2, i3;
    char *buf;
    size_t cap;
    int rc;
} req_t;

/* ---------------------------------------------------------------- the window */

static void win_layout_one(win_t *w)
{
    int dw = w10m_disp_w(), dh = w10m_disp_h();
    lv_obj_set_size(w->sheet, dw, dh - W10M_STATUS_H);
    lv_obj_set_pos(w->sheet, w->open ? 0 : dw, W10M_STATUS_H);
    lv_obj_set_size(w->head, dw, HEAD_H);
    lv_obj_set_size(w->body, dw, dh - W10M_STATUS_H - HEAD_H);
    lv_obj_align(w->body, LV_ALIGN_TOP_LEFT, 0, HEAD_H);
    for (int i = 0; i < PURR_CC_UI_MAX_WIDGETS; i++) {
        if (w->w[i] != NULL && lv_obj_has_flag(w->w[i], LV_OBJ_FLAG_USER_1)) {      /* a text area that fills */
            lv_obj_set_height(w->w[i], dh - W10M_STATUS_H - HEAD_H - 16);
        }
    }
    if (!w->open) {
        lv_obj_add_flag(w->sheet, LV_OBJ_FLAG_HIDDEN);
    }
}

static void back_clicked(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_win[slot].back_app) {
        purr_catrt_post_ui(slot, 0, "back");
    } else {
        purr_catrt_close_slot(slot);
    }
}

static void action_clicked(lv_event_t *e)
{
    purr_catrt_post_ui((int)(intptr_t)lv_event_get_user_data(e), 0, "action");
}

static void win_create(int slot, const char *name)
{
    win_t *w = &s_win[slot];
    memset(w, 0, sizeof(*w));
    w->used = 1;
    snprintf(w->name, sizeof(w->name), "%s", name);
    w->group = lv_group_create();

    w->sheet = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(w->sheet);
    purr_theme_apply(w->sheet, PURR_TH_SHEET);
    lv_obj_clear_flag(w->sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(w->sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(w->sheet, LV_OBJ_FLAG_HIDDEN);
    w10m_place_sheet(w->sheet);

    w->head = lv_obj_create(w->sheet);
    lv_obj_remove_style_all(w->head);
    lv_obj_clear_flag(w->head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(w->head, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(w->head, back_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)slot);
    lv_obj_t *arrow = lv_label_create(w->head);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    purr_theme_apply(arrow, PURR_TH_TEXT);
    lv_obj_align(arrow, LV_ALIGN_LEFT_MID, 8, 0);
    w->title = lv_label_create(w->head);
    lv_label_set_text_fmt(w->title, "%.30s", name);
    lv_obj_set_style_text_font(w->title, &lv_font_montserrat_16, 0);
    purr_theme_apply(w->title, PURR_TH_TEXT);
    lv_obj_align(w->title, LV_ALIGN_LEFT_MID, 34, 0);

    w->act = lv_label_create(w->sheet);                 /* above the header, so it has its own tap area */
    lv_obj_set_style_text_font(w->act, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_all(w->act, 6, 0);
    lv_obj_add_flag(w->act, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(w->act, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(w->act, action_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)slot);
    lv_obj_align(w->act, LV_ALIGN_TOP_RIGHT, -6, 0);

    w->body = lv_obj_create(w->sheet);
    lv_obj_remove_style_all(w->body);
    lv_obj_set_flex_flow(w->body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(w->body, 4, 0);
    lv_obj_set_style_pad_hor(w->body, 10, 0);
    lv_obj_set_scrollbar_mode(w->body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(w->body, LV_DIR_VER);
    win_layout_one(w);
}

static void x_cb(void *obj, int32_t v) { lv_obj_set_x((lv_obj_t *)obj, v); }

static void anim_done(lv_anim_t *a)
{
    for (int i = 0; i < PURR_RT_MAX_APPS; i++) {
        if (s_win[i].used && s_win[i].sheet == a->var && !s_win[i].open) {
            lv_obj_add_flag(s_win[i].sheet, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void win_show(win_t *w, int open)
{
    w->open = open;
    lv_indev_t *kb = purr_lvgl_keypad();
    if (open) {
        lv_obj_remove_flag(w->sheet, LV_OBJ_FLAG_HIDDEN);
        if (kb != NULL) {
            lv_indev_set_group(kb, w->group);
        }
    } else if (kb != NULL && lv_indev_get_group(kb) == w->group) {
        lv_indev_set_group(kb, NULL);
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, w->sheet);
    lv_anim_set_exec_cb(&a, x_cb);
    lv_anim_set_values(&a, lv_obj_get_x(w->sheet), open ? 0 : w10m_disp_w());
    lv_anim_set_duration(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, anim_done);
    lv_anim_start(&a);
}

static void win_destroy(win_t *w)
{
    lv_indev_t *kb = purr_lvgl_keypad();
    if (kb != NULL && lv_indev_get_group(kb) == w->group) {
        lv_indev_set_group(kb, NULL);
    }
    lv_anim_delete(w->sheet, x_cb);
    lv_obj_delete(w->sheet);
    lv_group_delete(w->group);
    memset(w, 0, sizeof(*w));
}

/* ---------------------------------------------------------------- widgets */

static int slot_of(const lv_obj_t *obj, int *handle)
{
    for (int s = 0; s < PURR_RT_MAX_APPS; s++) {
        if (!s_win[s].used) {
            continue;
        }
        for (int i = 0; i < PURR_CC_UI_MAX_WIDGETS; i++) {
            if (s_win[s].w[i] == obj) {
                *handle = i + 1;
                return s;
            }
        }
    }
    return -1;
}

static void widget_event(lv_event_t *e)
{
    int h;
    int s = slot_of(lv_event_get_target(e), &h);
    if (s < 0) {
        return;
    }
    purr_catrt_post_ui(s, h, lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED ? "changed" : "click");
}

static int add_widget(win_t *w, lv_obj_t *o)
{
    for (int i = 0; i < PURR_CC_UI_MAX_WIDGETS; i++) {
        if (w->w[i] == NULL) {
            w->w[i] = o;
            w->count++;
            return i + 1;
        }
    }
    lv_obj_delete(o);
    return -1;
}

static lv_obj_t *get_widget(win_t *w, int h)
{
    return h >= 1 && h <= PURR_CC_UI_MAX_WIDGETS ? w->w[h - 1] : NULL;
}

/* A string the app passed, bounded; NULL means "empty". */
static const char *safe(const char *s, size_t max)
{
    return s != NULL && strnlen(s, max + 1) <= max ? s : "";
}

static lv_obj_t *make_label(win_t *w, const char *text, int role)
{
    static const struct {
        const lv_font_t *font;
        purr_th_style_t style;
    } look[] = {
        {&lv_font_montserrat_14, PURR_TH_TEXT},
        {&lv_font_montserrat_12, PURR_TH_DIM},
        {&lv_font_montserrat_16, PURR_TH_ACCENT_TEXT},
        {&lv_font_montserrat_14, PURR_TH_ACCENT_TEXT},
    };
    if (role < 0 || role > 3) {
        role = 0;
    }
    lv_obj_t *l = lv_label_create(w->body);
    lv_label_set_text(l, safe(text, MAX_LABEL));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_font(l, look[role].font, 0);
    purr_theme_apply(l, look[role].style);
    return l;
}

static lv_obj_t *make_button(win_t *w, const char *text)
{
    lv_obj_t *b = lv_button_create(w->body);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, safe(text, 64));
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, widget_event, LV_EVENT_CLICKED, NULL);
    return b;
}

static lv_obj_t *make_row(win_t *w, const char *title, const char *sub)
{
    lv_obj_t *r = lv_obj_create(w->body);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), 48);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    purr_theme_apply(r, PURR_TH_SURFACE);
    purr_theme_apply(r, PURR_TH_ACCENT_BORDER);
    purr_theme_apply(r, PURR_TH_ROW_PRESSED);
    lv_obj_add_event_cb(r, widget_event, LV_EVENT_CLICKED, NULL);
    int has_sub = sub != NULL && sub[0] != '\0';
    lv_obj_t *a = lv_label_create(r);
    lv_label_set_text(a, safe(title, 96));
    lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
    lv_obj_set_width(a, LV_PCT(92));
    lv_obj_set_style_text_font(a, &lv_font_montserrat_14, 0);
    purr_theme_apply(a, PURR_TH_TEXT);
    lv_obj_align(a, LV_ALIGN_LEFT_MID, 10, has_sub ? -9 : 0);
    if (has_sub) {
        lv_obj_t *d = lv_label_create(r);
        lv_label_set_text(d, safe(sub, 96));
        lv_label_set_long_mode(d, LV_LABEL_LONG_DOT);
        lv_obj_set_width(d, LV_PCT(92));
        lv_obj_set_style_text_font(d, &lv_font_montserrat_12, 0);
        purr_theme_apply(d, PURR_TH_DIM);
        lv_obj_align(d, LV_ALIGN_LEFT_MID, 10, 10);
    }
    return r;
}

static lv_obj_t *make_textarea(win_t *w, int max_len, const char *placeholder, int fill)
{
    lv_obj_t *ta = lv_textarea_create(w->body);
    lv_textarea_set_max_length(ta, max_len < 1 || max_len > PURR_CC_UI_MAX_TEXT ? PURR_CC_UI_MAX_TEXT : (uint32_t)max_len);
    lv_textarea_set_placeholder_text(ta, safe(placeholder, 64));
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, 0);
    if (fill) {
        lv_obj_add_flag(ta, LV_OBJ_FLAG_USER_1);
        lv_obj_set_height(ta, w10m_disp_h() - W10M_STATUS_H - HEAD_H - 16);
    } else {
        lv_obj_set_height(ta, 90);
    }
    lv_obj_add_event_cb(ta, widget_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_group_add_obj(w->group, ta);
    return ta;
}

static void set_action(win_t *w, int action)
{
    static const char *sym[] = {"", LV_SYMBOL_PLUS, LV_SYMBOL_TRASH, LV_SYMBOL_OK};
    if (action <= 0 || action > 3) {
        lv_obj_add_flag(w->act, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(w->act, sym[action]);
    lv_obj_set_style_text_color(w->act, action == 1 || action == 3 ? purr_theme_accent_color() : purr_theme_text_color(), 0);
    lv_obj_remove_flag(w->act, LV_OBJ_FLAG_HIDDEN);
}

static void *exec(void *arg)
{
    req_t *r = arg;
    r->rc = -1;
    if (r->slot < 0 || r->slot >= PURR_RT_MAX_APPS) {
        return NULL;
    }
    win_t *w = &s_win[r->slot];
    if (!w->used) {
        win_create(r->slot, r->name);
    }
    lv_obj_t *o = NULL;
    switch (r->op) {
    case OP_TITLE:
        lv_label_set_text_fmt(w->title, "%.30s", safe(r->s1, 30));
        r->rc = 0;
        break;
    case OP_ACTION:
        set_action(w, r->i1);
        r->rc = 0;
        break;
    case OP_BACK:
        w->back_app = r->i1 != 0;
        r->rc = 0;
        break;
    case OP_CLEAR:
        lv_group_remove_all_objs(w->group);
        lv_obj_clean(w->body);
        memset(w->w, 0, sizeof(w->w));
        w->count = 0;
        lv_obj_scroll_to(w->body, 0, 0, LV_ANIM_OFF);
        r->rc = 0;
        break;
    case OP_LABEL:
        o = make_label(w, r->s1, r->i1);
        break;
    case OP_BUTTON:
        o = make_button(w, r->s1);
        break;
    case OP_ROW:
        o = make_row(w, r->s1, r->s2);
        break;
    case OP_TEXTAREA:
        o = make_textarea(w, r->i1, r->s1, r->i2);
        break;
    case OP_SPACER: {
        o = lv_obj_create(w->body);
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, 1, r->i1 < 0 ? 0 : r->i1 > 200 ? 200 : r->i1);
        break;
    }
    case OP_SET_TEXT:
        if ((o = get_widget(w, r->i1)) != NULL) {
            const char *t = safe(r->s1, PURR_CC_UI_MAX_TEXT);
            if (lv_obj_check_type(o, &lv_textarea_class)) {
                lv_textarea_set_text(o, t);
            } else if (lv_obj_check_type(o, &lv_label_class)) {
                lv_label_set_text(o, t);
            }
            r->rc = 0;
        }
        o = NULL;
        break;
    case OP_GET_TEXT:
        r->rc = 0;
        if (r->cap > 0) {
            r->buf[0] = '\0';
        }
        if ((o = get_widget(w, r->i1)) != NULL && r->cap > 0) {
            const char *t = lv_obj_check_type(o, &lv_textarea_class) ? lv_textarea_get_text(o)
                          : lv_obj_check_type(o, &lv_label_class) ? lv_label_get_text(o) : "";
            snprintf(r->buf, r->cap, "%s", t);
            r->rc = (int)strlen(r->buf);
        }
        o = NULL;
        break;
    case OP_FOCUS:
        if ((o = get_widget(w, r->i1)) != NULL) {
            lv_group_focus_obj(o);
            r->rc = 0;
        }
        o = NULL;
        break;
    case OP_REMOVE:
        if ((o = get_widget(w, r->i1)) != NULL) {
            w->w[r->i1 - 1] = NULL;
            w->count--;
            lv_obj_delete(o);
            r->rc = 0;
        }
        o = NULL;
        break;
    }
    if (o != NULL) {                                    /* a widget was just made */
        r->rc = add_widget(w, o);
    }
    return NULL;
}

static int call(req_t *r)
{
    r->slot = purr_catrt_caller(r->name, sizeof(r->name));
    if (r->slot < 0) {
        return -1;
    }
    if (purr_ui_service_run(exec, r, NULL) != ESP_OK) {
        return -1;
    }
    return r->rc;
}

/* ---------------------------------------------------------------- the catcall table */

static void cc_win_title(const char *t) { req_t r = {.op = OP_TITLE, .s1 = t}; call(&r); }
static void cc_win_action(int a) { req_t r = {.op = OP_ACTION, .i1 = a}; call(&r); }
static void cc_win_back(int app) { req_t r = {.op = OP_BACK, .i1 = app}; call(&r); }
static void cc_clear(void) { req_t r = {.op = OP_CLEAR}; call(&r); }
static int cc_label(const char *t, int role) { req_t r = {.op = OP_LABEL, .s1 = t, .i1 = role}; return call(&r); }
static int cc_button(const char *t) { req_t r = {.op = OP_BUTTON, .s1 = t}; return call(&r); }
static int cc_row(const char *t, const char *s) { req_t r = {.op = OP_ROW, .s1 = t, .s2 = s}; return call(&r); }
static int cc_textarea(int max, const char *ph, int fill)
{
    req_t r = {.op = OP_TEXTAREA, .i1 = max, .s1 = ph, .i2 = fill};
    return call(&r);
}
static int cc_spacer(int h) { req_t r = {.op = OP_SPACER, .i1 = h}; return call(&r); }
static void cc_set_text(int h, const char *t) { req_t r = {.op = OP_SET_TEXT, .i1 = h, .s1 = t}; call(&r); }
static int cc_get_text(int h, char *buf, size_t cap)
{
    req_t r = {.op = OP_GET_TEXT, .i1 = h, .buf = buf, .cap = cap};
    return call(&r);
}
static void cc_focus(int h) { req_t r = {.op = OP_FOCUS, .i1 = h}; call(&r); }
static void cc_remove(int h) { req_t r = {.op = OP_REMOVE, .i1 = h}; call(&r); }
static void cc_status(const char *msg, int bad)
{
    if (purr_catrt_caller(NULL, 0) >= 0) {              /* a closing app says nothing */
        w10m_banner_post(msg, bad);
    }
}

static const purr_cc_ui_t s_cc_ui = {
    .version = 2, .win_title = cc_win_title, .win_action = cc_win_action, .win_back = cc_win_back,
    .clear = cc_clear, .label = cc_label, .button = cc_button, .row = cc_row, .textarea = cc_textarea,
    .spacer = cc_spacer, .set_text = cc_set_text, .get_text = cc_get_text, .focus = cc_focus,
    .remove = cc_remove, .status = cc_status,
};

/* ---------------------------------------------------------------- lifecycle and layout */

typedef struct {
    int slot;
    const char *name;
    int ev;
} life_t;

static void *life_cb(void *arg)
{
    life_t *l = arg;
    win_t *w = &s_win[l->slot];
    if (l->ev == PURR_RT_EV_DESTROY) {
        if (w->used) {
            win_destroy(w);
        }
    } else if (l->ev == PURR_RT_EV_RESUME) {
        if (!w->used) {
            win_create(l->slot, l->name);               /* an app with nothing drawn yet still gets its window */
        }
        win_show(w, 1);
    } else if (l->ev == PURR_RT_EV_PAUSE && w->used) {
        win_show(w, 0);
    }
    return NULL;
}

static void lifecycle(int slot, const char *app, int ev)
{
    if (slot < 0 || slot >= PURR_RT_MAX_APPS) {
        return;
    }
    life_t l = {slot, app, ev};
    purr_ui_service_run(life_cb, &l, NULL);
}

static void *layout_cb(void *arg)
{
    (void)arg;
    for (int i = 0; i < PURR_RT_MAX_APPS; i++) {
        if (s_win[i].used) {
            win_layout_one(&s_win[i]);
        }
    }
    return NULL;
}

void purr_catrt_ui_layout(void)
{
    layout_cb(NULL);                                    /* called on the UI task by the shell */
}

void purr_catrt_ui_init(void)
{
    purr_catrt_set_ui(&s_cc_ui, lifecycle);
}
