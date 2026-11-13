/*
 * purr_tp_kbd.c - the Type shell's focus manager, its drawing helpers and the key grid (plan7.md, "The Type shell").
 *
 * Hybrid input without a second set of screens: every control is a clickable object (a finger works as it is), and the focus manager keeps a list of the controls
 * of the screen in front. An arrow key moves the highlight to the nearest control in that direction (wrapping round at the edges), Enter clicks the highlighted
 * one. The highlight is LVGL's own focused state, which the boxes style as an accent fill with a thicker border, the same look a press has.
 *
 * The key grid is one widget for the sign-in keyboard (letters, capitals, symbols) and the PIN pad. Its keys are plain boxes, so the focus manager and a finger use
 * them like anything else.
 */
#include "purr_tp_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FOCUS_MAX 80

static lv_obj_t *s_f[FOCUS_MAX];
static int s_nf;
static lv_obj_t *s_cur;

/* ---------------------------------------------------------------- the accent, and what reads on it */

lv_color_t tp_ink(void) { return lv_color_hex(TP_INK); }
lv_color_t tp_paper(void) { return lv_color_hex(TP_PAPER); }
lv_color_t tp_accent(void) { return purr_theme_accent_color(); }

/* Whichever of ink and paper reads better on the accent: the one with the higher contrast ratio, with the luminance taken as a gamma-2 approximation of the
 * sRGB curve. Every colour in the picker ends up with text above 3:1 (sdk/tests/test_type_shell.py checks all of them). */
static float lum2(lv_color_t c)
{
    float r = c.red / 255.0f, g = c.green / 255.0f, b = c.blue / 255.0f;
    return 0.2126f * r * r + 0.7152f * g * g + 0.0722f * b * b;
}

static float contrast(float x, float y)
{
    return x > y ? (x + 0.05f) / (y + 0.05f) : (y + 0.05f) / (x + 0.05f);
}

lv_color_t tp_on_accent(void)
{
    float a = lum2(tp_accent());
    return contrast(a, lum2(tp_ink())) >= contrast(a, lum2(tp_paper())) ? tp_ink() : tp_paper();
}

/* ---------------------------------------------------------------- the focus manager */

void tp_focus_clear(void)
{
    if (s_cur != NULL) {
        lv_obj_remove_state(s_cur, LV_STATE_FOCUSED);
    }
    s_cur = NULL;
    s_nf = 0;
}

void tp_focus_add(lv_obj_t *obj)
{
    if (obj != NULL && s_nf < FOCUS_MAX) {
        s_f[s_nf++] = obj;
    }
}

static int shown(const lv_obj_t *o)
{
    for (; o != NULL; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
            return 0;
        }
    }
    return 1;
}

void tp_focus_set(lv_obj_t *obj)
{
    if (s_cur != NULL && s_cur != obj) {
        lv_obj_remove_state(s_cur, LV_STATE_FOCUSED);
    }
    s_cur = obj;
    if (obj != NULL) {
        lv_obj_add_state(obj, LV_STATE_FOCUSED);
        lv_obj_scroll_to_view_recursive(obj, LV_ANIM_OFF);               /* a list scrolls so the highlight is on screen */
    }
}

lv_obj_t *tp_focus_get(void)
{
    return s_cur;
}

static void centre(const lv_obj_t *o, int *cx, int *cy)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    *cx = (a.x1 + a.x2) / 2;
    *cy = (a.y1 + a.y2) / 2;
}

/* The control nearest to `from` in the direction of `key`: the distance along the direction, plus three times the distance across it, so a neighbour in line wins
 * over a nearer one off to the side. With nothing in that direction it wraps to the far side, the one most in line. */
static lv_obj_t *neighbour(const lv_obj_t *from, uint32_t key)
{
    int fx, fy;
    centre(from, &fx, &fy);
    lv_obj_t *best = NULL, *wrap = NULL;
    long best_s = 0, wrap_s = 0;
    for (int i = 0; i < s_nf; i++) {
        lv_obj_t *o = s_f[i];
        if (o == from || !shown(o)) {
            continue;
        }
        int x, y;
        centre(o, &x, &y);
        int dx = x - fx, dy = y - fy, along, across;
        switch (key) {
        case LV_KEY_RIGHT: along = dx; across = dy; break;
        case LV_KEY_LEFT:  along = -dx; across = dy; break;
        case LV_KEY_DOWN:  along = dy; across = dx; break;
        default:           along = -dy; across = dx; break;                 /* up */
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
        } else if (along < 0 || across != 0) {                              /* the far side: the most distant one that is most in line */
            long s = 3L * across - (long)(-along);
            if (wrap == NULL || s < wrap_s) {
                wrap = o;
                wrap_s = s;
            }
        }
    }
    return best != NULL ? best : wrap;
}

int tp_focus_key(uint32_t key)
{
    if (key == LV_KEY_ENTER) {
        if (s_cur != NULL && shown(s_cur)) {
            lv_obj_send_event(s_cur, LV_EVENT_CLICKED, NULL);
            return 1;
        }
        return 0;
    }
    if (key != LV_KEY_UP && key != LV_KEY_DOWN && key != LV_KEY_LEFT && key != LV_KEY_RIGHT) {
        return 0;
    }
    if (s_nf == 0) {
        return 0;
    }
    if (s_cur == NULL || !shown(s_cur)) {
        for (int i = 0; i < s_nf; i++) {
            if (shown(s_f[i])) {
                tp_focus_set(s_f[i]);
                return 1;
            }
        }
        return 0;
    }
    lv_obj_t *n = neighbour(s_cur, key);
    if (n != NULL) {
        tp_focus_set(n);
    }
    return 1;
}

/* ---------------------------------------------------------------- drawing helpers */

lv_obj_t *tp_label(lv_obj_t *parent, const char *text, const lv_font_t *font, const lv_color_t *color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    if (font != NULL) {
        lv_obj_set_style_text_font(l, font, 0);
    }
    if (color != NULL) {
        lv_obj_set_style_text_color(l, *color, 0);
    }
    return l;
}

/* A clickable box: ink border; the fill is nothing, sand or ink; its text takes the colour that reads on it; focused or pressed it is the accent with a thicker
 * border and the text colour that reads on the accent. The text colour is set on the box, so the labels inside inherit it. */
lv_obj_t *tp_box(lv_obj_t *parent, int fill)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, tp_ink(), 0);
    lv_obj_set_style_radius(b, 0, 0);
    if (fill == TP_FILL_NONE) {
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(b, tp_ink(), 0);
    } else {
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, fill == TP_FILL_INK ? tp_ink() : lv_color_hex(TP_SAND), 0);
        lv_obj_set_style_text_color(b, fill == TP_FILL_INK ? tp_paper() : tp_ink(), 0);
    }
    static const lv_state_t on[2] = {LV_STATE_FOCUSED, LV_STATE_PRESSED};
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, on[i]);
        lv_obj_set_style_bg_color(b, tp_accent(), on[i]);
        lv_obj_set_style_border_width(b, 2, on[i]);
        lv_obj_set_style_text_color(b, tp_on_accent(), on[i]);
    }
    return b;
}

/* ---------------------------------------------------------------- the key grid */

#define GRID_KEYS 40

typedef struct {
    lv_obj_t *box;
    lv_obj_t *lbl;
    tp_key_kind_t kind;
    char c[3];                          /* the character in each layer: abc, ABC, symbols (0 for a key that is not a character) */
} gkey_t;

struct tp_grid {
    lv_obj_t *root;
    gkey_t k[GRID_KEYS];
    int n;
    int layer;                          /* 0 abc, 1 ABC, 2 symbols */
    int pad;                            /* 1: the PIN pad */
    tp_key_cb cb;
    void *ud;
};

static const lv_font_t *font_for(int small)
{
    return small ? &lv_font_montserrat_12 : &lv_font_montserrat_16;
}

static void key_label(gkey_t *k, int layer)
{
    char t[8];
    switch (k->kind) {
    case TP_KEY_DEL:   snprintf(t, sizeof(t), "DEL"); break;
    case TP_KEY_OK:    snprintf(t, sizeof(t), "OK"); break;
    case TP_KEY_SHIFT: snprintf(t, sizeof(t), "%s", layer == 0 ? "ABC" : (layer == 1 ? "#+=" : "abc")); break;
    default:
        if (k->c[layer] == ' ') {
            snprintf(t, sizeof(t), "SP");
        } else {
            snprintf(t, sizeof(t), "%c", k->c[layer]);
        }
        break;
    }
    lv_label_set_text(k->lbl, t);
    lv_obj_center(k->lbl);
}

static void key_clicked(lv_event_t *e)
{
    tp_grid_t *g = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_target_obj(e);
    for (int i = 0; i < g->n; i++) {
        gkey_t *k = &g->k[i];
        if (k->box != target) {
            continue;
        }
        if (k->kind == TP_KEY_SHIFT) {
            tp_grid_cycle(g);
        }
        if (g->cb != NULL) {
            char ch = k->c[g->layer];
            g->cb(g->ud, k->kind == TP_KEY_CHAR && ch == ' ' ? TP_KEY_SPACE : k->kind, ch);
        }
        return;
    }
}

static gkey_t *add_key(tp_grid_t *g, int x, int y, int w, int h, tp_key_kind_t kind, const char *chars)
{
    if (g->n >= GRID_KEYS) {
        return NULL;
    }
    gkey_t *k = &g->k[g->n++];
    memset(k, 0, sizeof(*k));
    k->kind = kind;
    for (int i = 0; i < 3 && chars != NULL && chars[i] != '\0'; i++) {
        k->c[i] = chars[i];
    }
    k->box = tp_box(g->root, kind == TP_KEY_CHAR ? TP_FILL_SAND : TP_FILL_INK);
    lv_obj_set_pos(k->box, x, y);
    lv_obj_set_size(k->box, w, h);
    k->lbl = tp_label(k->box, "", font_for(kind != TP_KEY_CHAR), NULL);
    if (g->pad && kind == TP_KEY_CHAR) {
        lv_obj_set_style_text_font(k->lbl, &lv_font_montserrat_20, 0);
    }
    key_label(k, 0);
    lv_obj_add_event_cb(k->box, key_clicked, LV_EVENT_CLICKED, g);
    return k;
}

static tp_grid_t *grid_new(lv_obj_t *parent, int x, int y, int w, int h, tp_key_cb cb, void *ud, int pad)
{
    tp_grid_t *g = calloc(1, sizeof(*g));
    if (g == NULL) {
        return NULL;
    }
    g->cb = cb;
    g->ud = ud;
    g->pad = pad;
    g->root = lv_obj_create(parent);
    lv_obj_remove_style_all(g->root);
    lv_obj_clear_flag(g->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g->root, x, y);
    lv_obj_set_size(g->root, w, h);
    return g;
}

tp_grid_t *tp_grid_keyboard(lv_obj_t *parent, int x, int y, int w, int h, tp_key_cb cb, void *ud)
{
    tp_grid_t *g = grid_new(parent, x, y, w, h, cb, ud, 0);
    if (g == NULL) {
        return NULL;
    }
    /* each row lists the character in the three layers: abc, ABC, symbols */
    static const char *const rows[3][10] = {
        {"11!", "22@", "33#", "44$", "55%", "66^", "77&", "88*", "99(", "00)"},
        {"qQ-", "wW_", "eE=", "rR+", "tT[", "yY]", "uU{", "iI}", "oO|", "pP\\"},
        {"aA~", "sS`", "dD<", "fF>", "gG/", "hH?", "jJ;", "kK:", "lL'", "-_\""},
    };
    static const char *const bottom[7] = {"zZ,", "xX.", "cC?", "vV:", "bB;", "nN_", "mM "};
    const int gap = 3;
    int cw = (w - 9 * gap) / 10, rh = (h - 3 * gap) / 4;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 10; c++) {
            add_key(g, c * (cw + gap), r * (rh + gap), cw, rh, TP_KEY_CHAR, rows[r][c]);
        }
    }
    int wide = (3 * cw + gap) / 2;                                           /* 1.5 keys */
    int x0 = 0, y3 = 3 * (rh + gap);
    add_key(g, x0, y3, wide, rh, TP_KEY_SHIFT, NULL);
    x0 += wide + gap;
    for (int c = 0; c < 7; c++) {
        add_key(g, x0, y3, cw, rh, TP_KEY_CHAR, bottom[c]);
        x0 += cw + gap;
    }
    add_key(g, x0, y3, w - x0, rh, TP_KEY_DEL, NULL);
    return g;
}

tp_grid_t *tp_grid_pinpad(lv_obj_t *parent, int x, int y, int w, int h, tp_key_cb cb, void *ud)
{
    tp_grid_t *g = grid_new(parent, x, y, w, h, cb, ud, 1);
    if (g == NULL) {
        return NULL;
    }
    const int gap = 4;
    int cw = (w - 2 * gap) / 3, rh = (h - 3 * gap) / 4;
    static const char *const digits[9] = {"111", "222", "333", "444", "555", "666", "777", "888", "999"};
    for (int i = 0; i < 9; i++) {
        add_key(g, (i % 3) * (cw + gap), (i / 3) * (rh + gap), cw, rh, TP_KEY_CHAR, digits[i]);
    }
    add_key(g, 0, 3 * (rh + gap), cw, rh, TP_KEY_DEL, NULL);
    add_key(g, cw + gap, 3 * (rh + gap), cw, rh, TP_KEY_CHAR, "000");
    add_key(g, 2 * (cw + gap), 3 * (rh + gap), cw, rh, TP_KEY_OK, NULL);
    return g;
}

void tp_grid_free(tp_grid_t *g)
{
    free(g);
}

void tp_grid_cycle(tp_grid_t *g)
{
    if (g == NULL || g->pad) {
        return;
    }
    g->layer = (g->layer + 1) % 3;
    for (int i = 0; i < g->n; i++) {
        key_label(&g->k[i], g->layer);
    }
}

void tp_grid_show(tp_grid_t *g, int on)
{
    if (g == NULL) {
        return;
    }
    if (on) {
        lv_obj_remove_flag(g->root, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g->root, LV_OBJ_FLAG_HIDDEN);
    }
}

void tp_grid_add_focus(tp_grid_t *g)
{
    for (int i = 0; g != NULL && i < g->n; i++) {
        tp_focus_add(g->k[i].box);
    }
}

lv_obj_t *tp_grid_first(tp_grid_t *g)
{
    if (g == NULL || g->n == 0) {
        return NULL;
    }
    return g->k[g->pad ? 4 : 12].box;                   /* the 5 on the pad, a key in the middle of the keyboard */
}
