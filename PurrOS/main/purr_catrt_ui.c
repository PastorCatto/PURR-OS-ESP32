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
#include "purr_catrt_files.h"
#include "purr_lvgl.h"
#include "purr_runtime.h"
#include "purr_ui_service.h"
#include "purr_w10m_priv.h"
#include "purr_w10m_theme.h"
#if CONFIG_PURR_SHELL_ANDROID
#include "purr_android_priv.h"
#endif

#define TAG "ui-cc"
#define HEAD_H 30
#define MAX_LABEL 256

typedef struct {
    int used;
    char name[PURR_RT_NAME_LEN];
    lv_obj_t *sheet, *head, *title, *act, *body;
    lv_group_t *group;
    int back_app;
    int light;                                          /* win_style(PURR_UI_STYLE_LIGHT): a white list with dark text */
    int open;
    int count;
    lv_obj_t *w[PURR_CC_UI_MAX_WIDGETS];               /* handle = index + 1 */
} win_t;

static win_t s_win[PURR_RT_MAX_APPS];

typedef enum {
    OP_TITLE, OP_ACTION, OP_BACK, OP_CLEAR, OP_LABEL, OP_BUTTON, OP_ROW, OP_TEXTAREA, OP_SPACER,
    OP_SET_TEXT, OP_GET_TEXT, OP_FOCUS, OP_REMOVE,
    OP_GRID, OP_GRID_BUTTON, OP_INPUT_LINE, OP_PROGRESS, OP_SLIDER, OP_SET_VALUE, OP_GET_VALUE, OP_IMAGE, OP_SET_IMAGE, OP_BUBBLE, OP_WIN_STYLE, OP_CONV_ROW,
} op_t;

typedef struct {
    op_t op;
    int slot;
    char name[PURR_RT_NAME_LEN];                    /* the calling app, for creating its window */
    const char *s1, *s2, *s3, *s4;
    int i1, i2, i3;
    char *buf;
    size_t cap;
    int rc;
} req_t;

/* ---------------------------------------------------------------- the window */

static void win_layout_one(win_t *w)
{
    int dw = w10m_disp_w(), dh = w10m_disp_h();
    lv_obj_set_size(w->sheet, dw, dh - W10M_STATUS_H - W10M_NAV_H);
    lv_obj_set_pos(w->sheet, w->open ? 0 : dw, W10M_STATUS_H);
    lv_obj_set_size(w->head, dw, HEAD_H);
    lv_obj_set_size(w->body, dw, dh - W10M_STATUS_H - W10M_NAV_H - HEAD_H);
    lv_obj_align(w->body, LV_ALIGN_TOP_LEFT, 0, HEAD_H);
    for (int i = 0; i < PURR_CC_UI_MAX_WIDGETS; i++) {
        if (w->w[i] != NULL && lv_obj_has_flag(w->w[i], LV_OBJ_FLAG_USER_1)) {      /* a text area that fills */
            lv_obj_set_height(w->w[i], dh - W10M_STATUS_H - W10M_NAV_H - HEAD_H - 16);
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

int purr_catrt_ui_back(void)
{
    for (int i = 0; i < PURR_RT_MAX_APPS; i++) {
        if (s_win[i].used && s_win[i].open) {
            if (s_win[i].back_app) {
                purr_catrt_post_ui(i, 0, "back");
            } else {
                purr_catrt_close_slot(i);
            }
            return 1;
        }
    }
    return 0;
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
    purr_theme_title_bar(w->head, arrow, w->title);
#if CONFIG_PURR_SHELL_ANDROID
    lv_obj_set_style_bg_opa(w->head, LV_OPA_COVER, 0);                       /* the Android 2.2 title bar: a dark grey gradient, the app's own icon, white text */
    lv_obj_set_style_bg_color(w->head, lv_color_hex(0x505050), 0);
    lv_obj_set_style_bg_grad_color(w->head, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_bg_grad_dir(w->head, LV_GRAD_DIR_VER, 0);
    const lv_image_dsc_t *ic = and_app_icon(name);
    if (ic != NULL) {
        lv_obj_t *im = lv_image_create(w->head);
        lv_image_set_src(im, ic);
        lv_image_set_scale(im, 128);                                         /* the 48 pixel launcher icon at half size */
        lv_obj_set_size(im, 24, 24);
        lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(im, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_align(w->title, LV_ALIGN_LEFT_MID, 32, 0);
    }
#endif

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
        {&lv_font_montserrat_40, PURR_TH_TEXT},                 /* BIG */
        {&lv_font_montserrat_28, PURR_TH_TEXT},                 /* MEDIUM */
#if LV_FONT_UNSCII_16
        {&lv_font_unscii_16, PURR_TH_TEXT},                     /* MONO */
#else
        {&lv_font_montserrat_14, PURR_TH_TEXT},                 /* MONO, where the monospace face is not built (KittenOS, the loader) */
#endif
    };
    if (role < 0 || role > 6) {
        role = 0;
    }
    lv_obj_t *l = lv_label_create(w->body);
    lv_label_set_text(l, safe(text, MAX_LABEL));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_font(l, look[role].font, 0);
    purr_theme_apply(l, look[role].style);
    if (w->light) {                                                          /* a light list: dark text whatever the theme's base is */
        lv_obj_set_style_text_color(l, lv_color_hex(role == PURR_UI_ROLE_DIM ? 0x666666 : (role == PURR_UI_ROLE_TITLE || role == PURR_UI_ROLE_ACCENT) ? 0xB35C00 : 0x000000), 0);
    }
    return l;
}

static lv_obj_t *make_button_in(lv_obj_t *parent, const char *text)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, safe(text, 64));
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, widget_event, LV_EVENT_CLICKED, NULL);
    return b;
}

static lv_obj_t *make_button(win_t *w, const char *text)
{
    return make_button_in(w->body, text);
}

/* ---- version 3: a grid of buttons, a one-line field, a bar, a slider, a picture frame */

#define GRID_GAP 3

static lv_obj_t *make_grid(win_t *w, int cols, int row_h)
{
    cols = cols < 1 ? 1 : cols > 8 ? 8 : cols;
    row_h = row_h < 20 ? 20 : row_h > 80 ? 80 : row_h;
    lv_obj_t *g = lv_obj_create(w->body);
    lv_obj_remove_style_all(g);
    lv_obj_set_width(g, LV_PCT(100));
    lv_obj_set_height(g, LV_SIZE_CONTENT);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(g, GRID_GAP, 0);
    lv_obj_set_user_data(g, (void *)(intptr_t)((cols << 8) | row_h));
    return g;
}

static lv_obj_t *make_grid_button(win_t *w, lv_obj_t *g, const char *text)
{
    int cfg = (int)(intptr_t)lv_obj_get_user_data(g), cols = cfg >> 8, row_h = cfg & 0xFF;
    if (cols < 1) {
        return NULL;                                           /* not a grid */
    }
    lv_obj_t *b = make_button_in(g, text);
    lv_obj_update_layout(g);
    lv_obj_set_size(b, (lv_obj_get_content_width(g) - (cols - 1) * GRID_GAP) / cols, row_h);
    lv_group_add_obj(w->group, b);                             /* the trackball reaches every key */
    return b;
}

static lv_obj_t *make_input_line(win_t *w, int max_len, const char *placeholder)
{
    lv_obj_t *ta = lv_textarea_create(w->body);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max_len < 1 || max_len > 256 ? 256 : (uint32_t)max_len);
    lv_textarea_set_placeholder_text(ta, safe(placeholder, 64));
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, 0);
    if (w->light) {
        lv_obj_set_style_bg_color(ta, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(ta, lv_color_black(), 0);
        lv_obj_set_style_border_width(ta, 1, 0);
        lv_obj_set_style_border_color(ta, lv_color_hex(0x888888), 0);
        lv_obj_set_style_radius(ta, 3, 0);
        lv_obj_set_style_text_color(ta, lv_color_hex(0x888888), LV_PART_TEXTAREA_PLACEHOLDER);
    }
    lv_obj_add_event_cb(ta, widget_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_group_add_obj(w->group, ta);
    return ta;
}

static lv_obj_t *make_progress(win_t *w, int value, int max)
{
    lv_obj_t *b = lv_bar_create(w->body);
    lv_obj_set_size(b, LV_PCT(100), 10);
    lv_bar_set_range(b, 0, max < 1 ? 1 : max);
    lv_bar_set_value(b, value, LV_ANIM_OFF);
    return b;
}

static lv_color_t avatar_color(const char *name)
{
    static const uint32_t pal[] = {0x3B6EA8, 0x2E7D4F, 0x8A5AB0, 0xB5502F, 0x6B6B6B, 0x2A8F9E};
    uint32_t h = 0;
    for (; name != NULL && *name; name++) {
        h = h * 31 + (uint8_t)*name;
    }
    return lv_color_hex(pal[h % 6]);
}

static lv_color_t tag_color(const char *tag)
{
    return lv_color_hex(!strcmp(tag, "MTS") ? 0x2E7D4F : !strcmp(tag, "RET") ? 0x3B6EA8 : !strcmp(tag, "MC") ? 0x8A5AB0 : !strcmp(tag, "ECHO") ? 0xB5502F : 0x6B6B6B);
}

/* A conversation or person row in the Froyo style (version 4): a coloured square with the first letter, the name over a line of text, the time on the right and a small tag
 * under it. An unread row is tinted with an orange bar at its left edge; a failed send gets a red mark. */
static lv_obj_t *make_conv_row(win_t *w, const char *name, const char *snippet, const char *when, const char *tag, int flags)
{
    name = safe(name, 40);
    snippet = safe(snippet, 120);
    when = safe(when, 12);
    tag = safe(tag, 7);
    int unread = (flags & PURR_UI_CONV_UNREAD) != 0, failed = (flags & PURR_UI_CONV_FAILED) != 0;
#if CONFIG_PURR_SHELL_ANDROID
    int dark = 0;                                                           /* the Android shell draws a conversation list the way Android 2.2 does: a white list, whatever the base */
    if (!w->light) {
        w->light = 1;
        lv_obj_set_style_bg_color(w->sheet, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(w->sheet, LV_OPA_COVER, 0);
    }
#else
    int dark = purr_theme_dark();                                           /* elsewhere the widget follows the system's light or dark base: the app says what, the OS says how it looks */
#endif
    uint32_t c_bg = unread ? (dark ? 0x3A2A10 : 0xFFF1DA) : (dark ? 0x1E1E1E : 0xFFFFFF), c_line = dark ? 0x3A3A3A : 0xD3D3D3, c_name = dark ? 0xFFFFFF : 0x000000,
             c_sub = dark ? 0xB0B0B0 : 0x666666, c_press = dark ? 0x4A3A20 : 0xFFE3B0;
    lv_obj_t *r = lv_obj_create(w->body);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), 44);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(r, lv_color_hex(c_bg), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(c_line), 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(c_press), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(r, lv_color_hex(c_press), LV_STATE_FOCUSED);
    lv_obj_add_event_cb(r, widget_event, LV_EVENT_CLICKED, NULL);
    if (unread) {
        lv_obj_t *bar = lv_obj_create(r);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 3, 44);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0xF08A00), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(bar, LV_ALIGN_LEFT_MID, 0, 0);
    }
    lv_obj_t *av = lv_obj_create(r);
    lv_obj_remove_style_all(av);
    lv_obj_set_size(av, 30, 30);
    lv_obj_set_style_bg_color(av, avatar_color(name), 0);
    lv_obj_set_style_bg_opa(av, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(av, 3, 0);
    lv_obj_remove_flag(av, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(av, LV_ALIGN_LEFT_MID, 8, 0);
    char letter[2] = {name[0] >= 'a' && name[0] <= 'z' ? (char)(name[0] - 32) : (name[0] ? name[0] : '?'), 0};
    lv_obj_t *lt = lv_label_create(av);
    lv_label_set_text(lt, letter);
    lv_obj_set_style_text_font(lt, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lt, lv_color_white(), 0);
    lv_obj_center(lt);
    int32_t mid_w = w10m_disp_w() - 20 - 8 - 30 - 8 - 58;
    lv_obj_t *a = lv_label_create(r);
    lv_label_set_text(a, name);
    lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
    lv_obj_set_size(a, mid_w, 18);                                          /* a fixed height, so a long name ends in "..." instead of wrapping */
    lv_obj_set_style_text_font(a, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(a, lv_color_hex(c_name), 0);
    lv_obj_align(a, LV_ALIGN_TOP_LEFT, 46, 4);
    lv_obj_t *d = lv_label_create(r);
    lv_label_set_text(d, snippet);
    lv_label_set_long_mode(d, LV_LABEL_LONG_DOT);
    lv_obj_set_size(d, mid_w, 15);
    lv_obj_set_style_text_font(d, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(d, lv_color_hex(c_sub), 0);
    lv_obj_align(d, LV_ALIGN_TOP_LEFT, 46, 24);
    if (when[0]) {
        lv_obj_t *t = lv_label_create(r);
        lv_label_set_text(t, when);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(t, lv_color_hex(c_sub), 0);
        lv_obj_align(t, LV_ALIGN_TOP_RIGHT, -6, 4);
    }
    if (tag[0]) {
        lv_obj_t *c = lv_obj_create(r);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(c, tag_color(tag), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(c, 2, 0);
        lv_obj_set_style_pad_hor(c, 3, 0);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *cl = lv_label_create(c);
        lv_label_set_text(cl, tag);
        lv_obj_set_style_text_font(cl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(cl, lv_color_white(), 0);
        lv_obj_align(c, LV_ALIGN_BOTTOM_RIGHT, -6, -4);
    }
    if (failed) {
        lv_obj_t *e = lv_obj_create(r);
        lv_obj_remove_style_all(e);
        lv_obj_set_size(e, 14, 14);
        lv_obj_set_style_bg_color(e, lv_color_hex(0xC62828), 0);
        lv_obj_set_style_bg_opa(e, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(e, LV_RADIUS_CIRCLE, 0);
        lv_obj_remove_flag(e, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *el = lv_label_create(e);
        lv_label_set_text(el, "!");
        lv_obj_set_style_text_font(el, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(el, lv_color_white(), 0);
        lv_obj_center(el);
        lv_obj_align(e, LV_ALIGN_TOP_RIGHT, -(6 + 8 * (int)strlen(when) + 4), 5);
    }
    return r;
}

/* A speech bubble for a message: `side` 0 sits on the left (received, light grey), 1 on the right (sent, light orange); `meta` is the small line under the text (time and state). */
static lv_obj_t *make_bubble(win_t *w, const char *text, int side, const char *meta)
{
    lv_obj_t *row = lv_obj_create(w->body);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, side ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *b = lv_obj_create(row);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
#if CONFIG_PURR_SHELL_ANDROID
    int dark = 0;
    if (!w->light) {                                                        /* a message thread is a white page in the Android shell too */
        w->light = 1;
        lv_obj_set_style_bg_color(w->sheet, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(w->sheet, LV_OPA_COVER, 0);
    }
#else
    int dark = purr_theme_dark();
#endif
    lv_obj_set_style_bg_color(b, lv_color_hex(side ? (dark ? 0x7A4E10 : 0xFFE3B0) : (dark ? 0x3A3A3A : 0xE4E4E4)), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_pad_all(b, 5, 0);
    lv_obj_set_style_pad_row(b, 1, 0);
    int32_t maxw = (w10m_disp_w() * 72) / 100;
    lv_obj_t *t = lv_label_create(b);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_label_set_text(t, safe(text, PURR_CC_UI_MAX_TEXT));
    lv_obj_set_width(t, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(t, maxw, 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(dark ? 0xFFFFFF : 0x000000), 0);
    if (meta != NULL && meta[0] != '\0') {
        lv_obj_t *m = lv_label_create(b);
        lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
        lv_label_set_text(m, safe(meta, 96));
        lv_obj_set_width(m, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(m, maxw, 0);
        lv_obj_set_style_text_font(m, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(m, lv_color_hex(dark ? 0xC8B890 : 0x6B5A3A), 0);
    }
    return row;
}

static lv_obj_t *make_slider(win_t *w, int min, int max, int value)
{
    lv_obj_t *sl = lv_slider_create(w->body);
    lv_obj_set_width(sl, LV_PCT(92));
    lv_slider_set_range(sl, min, max <= min ? min + 1 : max);
    lv_slider_set_value(sl, value, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, widget_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_group_add_obj(w->group, sl);
    return sl;
}

/* A picture's bytes and the descriptor LVGL decodes them from (JPEG, PNG and BMP are decoded on demand); freed with the widget. */
typedef struct {
    lv_image_dsc_t dsc;
    uint8_t *data;
    uint32_t w, h;                                      /* what the file says its size is */
} pic_t;

static void pic_deleted(lv_event_t *e)
{
    pic_t *p = lv_obj_get_user_data(lv_event_get_target(e));
    if (p != NULL) {
        free(p->data);
        free(p);
    }
}

static lv_obj_t *make_image(win_t *w, int height)
{
    lv_obj_t *im = lv_image_create(w->body);
    lv_obj_set_size(im, LV_PCT(100), height < 40 ? 40 : height > 400 ? 400 : height);
    lv_image_set_inner_align(im, LV_IMAGE_ALIGN_CENTER);       /* scaled to fit by set_image */
    lv_obj_add_event_cb(im, pic_deleted, LV_EVENT_DELETE, NULL);
    return im;
}

static lv_obj_t *make_row(win_t *w, const char *title, const char *sub)
{
    lv_obj_t *r = lv_obj_create(w->body);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), 48);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    purr_theme_apply(r, PURR_TH_LIST_ROW);
    if (w->light) {                                                          /* a Froyo list row: white, a thin line under it, orange when pressed */
        lv_obj_set_style_bg_color(r, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(0xD3D3D3), 0);
        lv_obj_set_style_radius(r, 0, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(0xFFE3B0), LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(r, lv_color_hex(0xFFE3B0), LV_STATE_FOCUSED);
    }
    lv_obj_add_event_cb(r, widget_event, LV_EVENT_CLICKED, NULL);
    int has_sub = sub != NULL && sub[0] != '\0';
    lv_obj_t *a = lv_label_create(r);
    lv_label_set_text(a, safe(title, 96));
    lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
    lv_obj_set_width(a, LV_PCT(92));
    lv_obj_set_style_text_font(a, &lv_font_montserrat_14, 0);
    purr_theme_apply(a, PURR_TH_TEXT);
    if (w->light) {
        lv_obj_set_style_text_color(a, lv_color_black(), 0);
    }
    lv_obj_align(a, LV_ALIGN_LEFT_MID, 10, has_sub ? -9 : 0);
    if (has_sub) {
        lv_obj_t *d = lv_label_create(r);
        lv_label_set_text(d, safe(sub, 96));
        lv_label_set_long_mode(d, LV_LABEL_LONG_DOT);
        lv_obj_set_width(d, LV_PCT(92));
        lv_obj_set_style_text_font(d, &lv_font_montserrat_12, 0);
        purr_theme_apply(d, PURR_TH_DIM);
        if (w->light) {
            lv_obj_set_style_text_color(d, lv_color_hex(0x666666), 0);
        }
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
        lv_obj_set_height(ta, w10m_disp_h() - W10M_STATUS_H - W10M_NAV_H - HEAD_H - 16);
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
    case OP_GRID:
        o = make_grid(w, r->i1, r->i2);
        break;
    case OP_GRID_BUTTON: {
        lv_obj_t *g = get_widget(w, r->i1);
        o = g != NULL ? make_grid_button(w, g, r->s1) : NULL;
        break;
    }
    case OP_INPUT_LINE:
        o = make_input_line(w, r->i1, r->s1);
        break;
    case OP_BUBBLE:
        o = make_bubble(w, r->s1, r->i1, r->s2);
        break;
    case OP_CONV_ROW:
        o = make_conv_row(w, r->s1, r->s2, r->s3, r->s4, r->i1);
        break;
    case OP_WIN_STYLE:
#if !CONFIG_PURR_SHELL_ANDROID
        r->rc = 0;                                  /* a light list is the Android shell's way; the other shell keeps following the system's light or dark base */
        break;
#endif
        w->light = r->i1 == PURR_UI_STYLE_LIGHT;
        if (w->light) {
            lv_obj_set_style_bg_color(w->sheet, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(w->sheet, LV_OPA_COVER, 0);
        } else {
            purr_theme_apply(w->sheet, PURR_TH_SHEET);
        }
        r->rc = 0;
        break;
    case OP_PROGRESS:
        o = make_progress(w, r->i1, r->i2);
        break;
    case OP_SLIDER:
        o = make_slider(w, r->i1, r->i2, r->i3);
        break;
    case OP_SET_VALUE:
        if ((o = get_widget(w, r->i1)) != NULL) {
            if (lv_obj_check_type(o, &lv_slider_class)) {
                lv_slider_set_value(o, r->i2, LV_ANIM_OFF);
                r->rc = 0;
            } else if (lv_obj_check_type(o, &lv_bar_class)) {
                lv_bar_set_value(o, r->i2, LV_ANIM_OFF);
                r->rc = 0;
            }
        }
        o = NULL;
        break;
    case OP_GET_VALUE:
        r->rc = 0;
        if ((o = get_widget(w, r->i1)) != NULL) {
            r->rc = lv_obj_check_type(o, &lv_slider_class) ? lv_slider_get_value(o) : lv_obj_check_type(o, &lv_bar_class) ? lv_bar_get_value(o) : 0;
        }
        o = NULL;
        break;
    case OP_IMAGE:
        o = make_image(w, r->i1);
        break;
    case OP_SET_IMAGE: {
        lv_obj_t *im = get_widget(w, r->i1);
        if (im == NULL || !lv_obj_check_type(im, &lv_image_class)) {
            break;                                      /* rc stays -1: the caller keeps its buffer */
        }
        pic_t *p = calloc(1, sizeof(*p));
        if (p == NULL) {
            break;
        }
        p->data = (uint8_t *)r->buf;                    /* the widget owns the bytes from here */
        p->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        p->dsc.header.cf = LV_COLOR_FORMAT_RAW;
        p->dsc.data_size = (uint32_t)r->cap;
        p->dsc.data = p->data;
        p->w = (uint32_t)r->i2;
        p->h = (uint32_t)r->i3;
        pic_t *old = lv_obj_get_user_data(im);
        lv_obj_set_user_data(im, p);
        lv_image_set_src(im, &p->dsc);
        lv_obj_update_layout(im);
        if (p->w > 0 && p->h > 0) {                     /* fit the frame, keeping the shape: LVGL scales 256 = 1x around the middle */
            uint32_t sx = (uint32_t)lv_obj_get_width(im) * 256u / p->w, sy = (uint32_t)lv_obj_get_height(im) * 256u / p->h;
            uint32_t sc = sx < sy ? sx : sy;
            lv_image_set_scale(im, sc < 8 ? 8 : sc > 4096 ? 4096 : sc);
        }
        if (old != NULL) {
            free(old->data);
            free(old);
        }
        r->rc = 0;
        break;
    }
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

/* ---- version 3 */

static int cc_grid(int cols, int row_h) { req_t r = {.op = OP_GRID, .i1 = cols, .i2 = row_h}; return call(&r); }
static int cc_grid_button(int grid, const char *t) { req_t r = {.op = OP_GRID_BUTTON, .i1 = grid, .s1 = t}; return call(&r); }
static int cc_input_line(int max, const char *ph) { req_t r = {.op = OP_INPUT_LINE, .i1 = max, .s1 = ph}; return call(&r); }
static int cc_progress(int value, int max) { req_t r = {.op = OP_PROGRESS, .i1 = value, .i2 = max}; return call(&r); }
static int cc_slider(int min, int max, int value) { req_t r = {.op = OP_SLIDER, .i1 = min, .i2 = max, .i3 = value}; return call(&r); }
static void cc_set_value(int h, int v) { req_t r = {.op = OP_SET_VALUE, .i1 = h, .i2 = v}; call(&r); }
static int cc_get_value(int h) { req_t r = {.op = OP_GET_VALUE, .i1 = h}; return call(&r); }
static int cc_bubble(const char *text, int side, const char *meta) { req_t r = {.op = OP_BUBBLE, .s1 = text, .i1 = side, .s2 = meta}; return call(&r); }
static void cc_win_style(int style) { req_t r = {.op = OP_WIN_STYLE, .i1 = style}; call(&r); }
static int cc_conv_row(const char *name, const char *snippet, const char *when, const char *tag, int flags)
{
    req_t r = {.op = OP_CONV_ROW, .s1 = name, .s2 = snippet, .s3 = when, .s4 = tag, .i1 = flags};
    return call(&r);
}
static int cc_image(int height) { req_t r = {.op = OP_IMAGE, .i1 = height}; return call(&r); }

#define IMG_FILE_MAX    (2u * 1024 * 1024)
#define IMG_MAX_PIXELS  1000000u

/* The size a picture says it has, and what it is: 1 PNG, 2 JPEG, 3 BMP, 0 not one of those. Read from the file's own header, before anything is decoded. */
static int sniff_picture(const uint8_t *d, size_t n, uint32_t *w, uint32_t *h)
{
    if (n >= 24 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) {
        *w = (uint32_t)d[16] << 24 | (uint32_t)d[17] << 16 | (uint32_t)d[18] << 8 | d[19];
        *h = (uint32_t)d[20] << 24 | (uint32_t)d[21] << 16 | (uint32_t)d[22] << 8 | d[23];
        return 1;
    }
    if (n >= 26 && d[0] == 'B' && d[1] == 'M') {
        int32_t bw = (int32_t)((uint32_t)d[18] | (uint32_t)d[19] << 8 | (uint32_t)d[20] << 16 | (uint32_t)d[21] << 24);
        int32_t bh = (int32_t)((uint32_t)d[22] | (uint32_t)d[23] << 8 | (uint32_t)d[24] << 16 | (uint32_t)d[25] << 24);
        *w = bw < 0 ? (uint32_t)-bw : (uint32_t)bw;
        *h = bh < 0 ? (uint32_t)-bh : (uint32_t)bh;
        return 3;
    }
    if (n >= 4 && d[0] == 0xFF && d[1] == 0xD8) {
        size_t i = 2;
        while (i + 9 < n) {
            if (d[i] != 0xFF) {
                i++;
                continue;
            }
            uint8_t m = d[i + 1];
            if (m == 0xFF) {
                i++;
                continue;
            }
            if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {       /* a start-of-frame marker: height, then width */
                *h = (uint32_t)d[i + 5] << 8 | d[i + 6];
                *w = (uint32_t)d[i + 7] << 8 | d[i + 8];
                return 2;
            }
            size_t len = (size_t)d[i + 2] << 8 | d[i + 3];
            i += 2 + (len < 2 ? 2 : len);
        }
    }
    return 0;
}

/* The file is read and checked here, on the app's own task, so the UI task is never kept waiting on a flash read; only the finished buffer goes across. */
static int cc_set_image(int h, const char *path)
{
    uint8_t *data = NULL;
    size_t len = 0;
    int rc = purr_catrt_files_load(path, &data, &len, IMG_FILE_MAX);
    if (rc != PURR_FILES_OK) {
        return rc;
    }
    uint32_t pw = 0, ph = 0;
    if (sniff_picture(data, len, &pw, &ph) == 0) {
        free(data);
        return PURR_UI_IMG_UNSUPPORTED;
    }
    if (pw == 0 || ph == 0 || (uint64_t)pw * ph > IMG_MAX_PIXELS) {
        free(data);
        return PURR_UI_IMG_TOO_BIG;
    }
    req_t r = {.op = OP_SET_IMAGE, .i1 = h, .i2 = (int)pw, .i3 = (int)ph, .buf = (char *)data, .cap = len};
    call(&r);
    if (r.rc != 0) {
        free(data);                                     /* the widget did not take it */
        return PURR_FILES_BAD;
    }
    return 0;
}

static const purr_cc_ui_t s_cc_ui = {
    .version = 5, .win_title = cc_win_title, .win_action = cc_win_action, .win_back = cc_win_back,
    .clear = cc_clear, .label = cc_label, .button = cc_button, .row = cc_row, .textarea = cc_textarea,
    .spacer = cc_spacer, .set_text = cc_set_text, .get_text = cc_get_text, .focus = cc_focus,
    .remove = cc_remove, .status = cc_status,
    .grid = cc_grid, .grid_button = cc_grid_button, .input_line = cc_input_line, .progress = cc_progress, .slider = cc_slider,
    .set_value = cc_set_value, .get_value = cc_get_value, .image = cc_image, .set_image = cc_set_image, .bubble = cc_bubble, .win_style = cc_win_style, .conv_row = cc_conv_row,
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

int purr_catrt_ui_scroll(int dy)
{
    for (int i = 0; i < PURR_RT_MAX_APPS; i++) {
        if (s_win[i].used && s_win[i].open && s_win[i].body != NULL) {
            lv_obj_scroll_by(s_win[i].body, 0, -dy, LV_ANIM_OFF);
            return 1;
        }
    }
    return 0;
}

void purr_catrt_ui_layout(void)
{
    layout_cb(NULL);                                    /* called on the UI task by the shell */
}

void purr_catrt_ui_init(void)
{
    purr_catrt_set_ui(&s_cc_ui, lifecycle);
}
