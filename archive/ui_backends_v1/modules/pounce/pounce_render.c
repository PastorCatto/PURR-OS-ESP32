// pounce_render.c — draw primitives, layout pass, and repaint for the Pounce
// UI backend. Every catcall_ui_t mutator redraws synchronously at the call
// site (pounce_plan.md A3) — there is no dirty-flag/tick-scan architecture
// and no offscreen framebuffer; every draw call here goes straight to
// catcall_display_t's push_pixels()/fill_rect().
#include "sdkconfig.h"

#ifdef CONFIG_PURR_UI_BACKEND_POUNCE

#include "pounce.h"
#include "../../kernel/core/purr_kernel.h"
#include <string.h>
#include <stdio.h>

#define PW_MARGIN 4
#define PW_GAP    2
#define PW_ROW_H  (PW_CHAR_H + 6)

// ── Colour + glyph primitives ────────────────────────────────────────────────

static inline uint16_t rgb565(uint32_t c) {
    uint16_t r = (uint16_t)((c >> 16) & 0xF8);
    uint16_t g = (uint16_t)((c >> 8) & 0xFC);
    uint16_t b = (uint16_t)(c & 0xF8);
    return (uint16_t)((r << 8) | (g << 3) | (b >> 3));
}

void pw_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t rgb888) {
    if (w <= 0 || h <= 0) return;
    const catcall_display_t *disp = purr_kernel_display();
    if (!disp) return;
    disp->fill_rect(x, y, w, h, rgb565(rgb888));
}

// 1px unfilled rectangle outline — four thin fill_rect calls, cheaper than a
// generic line-draw routine for the only shape this backend ever outlines.
static void pw_rect_outline(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t rgb888) {
    if (w <= 0 || h <= 0) return;
    pw_fill_rect(x, y, w, 1, rgb888);
    pw_fill_rect(x, (int16_t)(y + h - 1), w, 1, rgb888);
    pw_fill_rect(x, y, 1, h, rgb888);
    pw_fill_rect((int16_t)(x + w - 1), y, 1, h, rgb888);
}

static void pw_char(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg) {
    const catcall_display_t *disp = purr_kernel_display();
    if (!disp) return;
    if (c < 0x20 || c > 0x7F) c = ' ';
    const uint8_t *col = PW_FONT6x8[(uint8_t)c - 0x20];
    uint16_t buf[PW_CHAR_W * PW_CHAR_H];
    for (int cx = 0; cx < PW_CHAR_W; cx++) {
        uint8_t bits = col[cx];
        for (int row = 0; row < PW_CHAR_H; row++)
            buf[row * PW_CHAR_W + cx] = (bits & (1 << row)) ? fg : bg;
    }
    disp->push_pixels(x, y, PW_CHAR_W, PW_CHAR_H, buf);
}

void pw_draw_string(int16_t x, int16_t y, const char *s, uint32_t fg888, uint32_t bg888) {
    if (!s) return;
    uint16_t fg = rgb565(fg888), bg = rgb565(bg888);
    int16_t cx = x;
    while (*s) {
        if (*s == '\n') break;   // callers that need multi-line split first
        pw_char(cx, y, *s, fg, bg);
        cx = (int16_t)(cx + PW_CHAR_W);
        s++;
    }
}

// Same as pw_draw_string but clips to max_chars — used inside a fixed-width
// widget rect so a long string never bleeds into a neighbour.
void pw_draw_string_clipped(int16_t x, int16_t y, const char *s, int max_chars,
                             uint32_t fg888, uint32_t bg888) {
    if (!s || max_chars <= 0) return;
    uint16_t fg = rgb565(fg888), bg = rgb565(bg888);
    int16_t cx = x;
    for (int i = 0; i < max_chars && s[i] && s[i] != '\n'; i++) {
        pw_char(cx, y, s[i], fg, bg);
        cx = (int16_t)(cx + PW_CHAR_W);
    }
}

// ── Widget-tree walk ─────────────────────────────────────────────────────────

void pw_for_each_child(purr_win_t win, int16_t parent, pw_visit_fn visit, void *user) {
    for (int i = 0; i < PW_MAX_WIDS; i++) {
        pw_widget_t *w = &s_pw_wids[i];
        if (w->alive && w->win == win && w->parent == parent) visit(i, user);
    }
}

static int pw_count_children(purr_win_t win, int16_t parent) {
    int n = 0;
    for (int i = 0; i < PW_MAX_WIDS; i++) {
        pw_widget_t *w = &s_pw_wids[i];
        if (w->alive && w->win == win && w->parent == parent) n++;
    }
    return n;
}

// ── Layout pass (A3 — computed once at win_show(), not per widget-add) ──────

static void layout_container(int idx) {
    pw_widget_t *cont = &s_pw_wids[idx];
    int idxs[PW_MAX_WIDS];
    int n = 0;
    for (int i = 0; i < PW_MAX_WIDS; i++) {
        pw_widget_t *w = &s_pw_wids[i];
        if (w->alive && w->win == cont->win && w->parent == idx) idxs[n++] = i;
    }
    if (n == 0) return;

    uint8_t pad = cont->layout.pad;
    if (cont->layout.dir == PURR_LAYOUT_ROW) {
        int16_t cell_w = (int16_t)(cont->w / n);
        for (int k = 0; k < n; k++) {
            pw_widget_t *c = &s_pw_wids[idxs[k]];
            c->x = (int16_t)(cont->x + k * cell_w + pad);
            c->y = (int16_t)(cont->y + pad);
            c->w = (int16_t)(cell_w - 2 * pad);
            c->h = (int16_t)(cont->h - 2 * pad);
        }
    } else {
        int16_t cell_h = (int16_t)(cont->h / n);
        for (int k = 0; k < n; k++) {
            pw_widget_t *c = &s_pw_wids[idxs[k]];
            c->x = (int16_t)(cont->x + pad);
            c->y = (int16_t)(cont->y + k * cell_h + pad);
            c->w = (int16_t)(cont->w - 2 * pad);
            c->h = (int16_t)(cell_h - 2 * pad);
        }
    }
}

void pw_layout_compute(purr_win_t win) {
    const catcall_display_t *disp = purr_kernel_display();
    if (!disp) return;
    display_info_t info;
    disp->get_info(&info);

    int16_t content_x = PW_MARGIN;
    int16_t content_w = (int16_t)(info.width - 2 * PW_MARGIN);
    int16_t avail_h   = (int16_t)(info.height - PW_STATUS_H - 2 * PW_MARGIN);
    int16_t cursor_y  = (int16_t)(PW_STATUS_H + PW_MARGIN);

    for (int i = 0; i < PW_MAX_WIDS; i++) {
        pw_widget_t *w = &s_pw_wids[i];
        if (!w->alive || w->win != win || w->parent != -1) continue;

        int16_t row_h;
        switch (w->kind) {
        case PW_LAYOUT:
            if (w->layout.dir == PURR_LAYOUT_COL) {
                int n = pw_count_children(win, (int16_t)i);
                row_h = (int16_t)((n > 0 ? n : 1) * PW_ROW_H);
            } else {
                row_h = PW_ROW_H;
            }
            w->x = content_x; w->y = cursor_y; w->w = content_w; w->h = row_h;
            layout_container(i);
            break;
        case PW_TEXTAREA:
            row_h = (int16_t)(avail_h * w->textarea.h_pct / 100);
            w->x = content_x; w->y = cursor_y;
            w->w = (int16_t)(content_w * w->textarea.w_pct / 100);
            w->h = row_h;
            break;
        case PW_LIST:
            row_h = (int16_t)(avail_h * w->list.h_pct / 100);
            w->x = content_x; w->y = cursor_y;
            w->w = (int16_t)(content_w * w->list.w_pct / 100);
            w->h = row_h;
            break;
        default:   // PW_LABEL, PW_BUTTON standing alone at the top level
            row_h = PW_ROW_H;
            w->x = content_x; w->y = cursor_y; w->w = content_w; w->h = row_h;
            break;
        }
        cursor_y = (int16_t)(cursor_y + row_h + PW_GAP);
    }
}

// ── Per-widget redraw ─────────────────────────────────────────────────────────

static void draw_label(pw_widget_t *w) {
    pw_fill_rect(w->x, w->y, w->w, w->h, PW_COL_BG);
    if (!w->label.text) return;
    int16_t tx = w->x;
    int len = (int)strlen(w->label.text);
    int max_chars = w->w / PW_CHAR_W;
    if (w->label.align == PURR_ALIGN_CENTER) {
        int tw = (len < max_chars ? len : max_chars) * PW_CHAR_W;
        tx = (int16_t)(w->x + (w->w - tw) / 2);
    } else if (w->label.align == PURR_ALIGN_RIGHT) {
        int tw = (len < max_chars ? len : max_chars) * PW_CHAR_W;
        tx = (int16_t)(w->x + w->w - tw);
    }
    pw_draw_string_clipped(tx, (int16_t)(w->y + (w->h - PW_CHAR_H) / 2), w->label.text,
                            max_chars, PW_COL_FG, PW_COL_BG);
}

static void draw_button(pw_widget_t *w) {
    uint32_t fg = w->button.enabled ? PW_COL_FG : PW_COL_DIM;
    pw_fill_rect(w->x, w->y, w->w, w->h, PW_COL_BG);
    pw_rect_outline(w->x, w->y, w->w, w->h, fg);
    if (!w->button.text) return;
    int len = (int)strlen(w->button.text);
    int max_chars = (w->w - 2 * PW_CHAR_W) / PW_CHAR_W;
    int tw = (len < max_chars ? len : max_chars) * PW_CHAR_W;
    int16_t tx = (int16_t)(w->x + (w->w - tw) / 2);
    pw_draw_string_clipped(tx, (int16_t)(w->y + (w->h - PW_CHAR_H) / 2), w->button.text,
                            max_chars, fg, PW_COL_BG);
}

static void draw_textarea(pw_widget_t *w) {
    pw_fill_rect(w->x, w->y, w->w, w->h, PW_COL_BG);
    pw_rect_outline(w->x, w->y, w->w, w->h, PW_COL_FG);
    if (!w->textarea.buf) return;

    int max_chars = (w->w - 4) / PW_CHAR_W;
    int max_lines  = (w->h - 4) / PW_CHAR_H;
    int16_t y = (int16_t)(w->y + 2);
    const char *line = w->textarea.buf;
    int drawn = 0;
    while (line && *line && drawn < max_lines) {
        const char *nl = strchr(line, '\n');
        int linelen = nl ? (int)(nl - line) : (int)strlen(line);
        int show = linelen < max_chars ? linelen : max_chars;
        char tmp[128];
        if (show >= (int)sizeof(tmp)) show = (int)sizeof(tmp) - 1;
        memcpy(tmp, line, (size_t)show);
        tmp[show] = '\0';
        pw_draw_string((int16_t)(w->x + 2), y, tmp, PW_COL_FG, PW_COL_BG);
        y = (int16_t)(y + PW_CHAR_H);
        drawn++;
        line = nl ? nl + 1 : NULL;
    }
    if (w->textarea.editing && drawn <= max_lines) {
        // Simple block cursor after the last drawn line's content — no
        // blink timer (A8 scope cut keeps this deliberately minimal).
        size_t len = w->textarea.len;
        size_t last_line_start = 0;
        for (size_t i = 0; i < len; i++) if (w->textarea.buf[i] == '\n') last_line_start = i + 1;
        int col = (int)(len - last_line_start);
        if (col > max_chars) col = max_chars;
        int16_t cx = (int16_t)(w->x + 2 + col * PW_CHAR_W);
        int16_t cy = (int16_t)(w->y + 2 + (drawn > 0 ? drawn - 1 : 0) * PW_CHAR_H);
        pw_fill_rect(cx, cy, 2, PW_CHAR_H, PW_COL_FG);
    }
}

static void draw_list(pw_widget_t *w) {
    pw_fill_rect(w->x, w->y, w->w, w->h, PW_COL_BG);
    pw_rect_outline(w->x, w->y, w->w, w->h, PW_COL_FG);
    if (!w->list.items || w->list.count == 0) return;

    int max_lines = (w->h - 4) / PW_CHAR_H;
    int max_chars = (w->w - 4) / PW_CHAR_W;
    int top = w->list.top_visible;
    for (int row = 0; row < max_lines && (top + row) < w->list.count; row++) {
        int i = top + row;
        int16_t ry = (int16_t)(w->y + 2 + row * PW_CHAR_H);
        bool sel = (i == w->list.selected);
        if (sel) pw_fill_rect((int16_t)(w->x + 1), ry, (int16_t)(w->w - 2), PW_CHAR_H, PW_COL_FOCUS_BG);
        pw_draw_string_clipped((int16_t)(w->x + 2), ry, w->list.items[i], max_chars,
                                sel ? PW_COL_FOCUS_FG : PW_COL_FG,
                                sel ? PW_COL_FOCUS_BG : PW_COL_BG);
    }
}

void pw_redraw_widget(purr_wid_t wid) {
    pw_widget_t *w = pw_widget(wid);
    if (!w) return;
    switch (w->kind) {
    case PW_LABEL:    draw_label(w);    break;
    case PW_BUTTON:   draw_button(w);   break;
    case PW_TEXTAREA: draw_textarea(w); break;
    case PW_LIST:     draw_list(w);     break;
    case PW_LAYOUT:   break;   // containers draw nothing themselves
    }
    if (w->focused) pw_draw_focus_border(wid, true);
}

void pw_draw_focus_border(purr_wid_t wid, bool on) {
    pw_widget_t *w = pw_widget(wid);
    if (!w) return;
    if (!on) {
        // Restore normal appearance rather than trying to precisely erase
        // just the border pixels — one extra small redraw, always correct.
        switch (w->kind) {
        case PW_LABEL:    draw_label(w);    break;
        case PW_BUTTON:   draw_button(w);   break;
        case PW_TEXTAREA: draw_textarea(w); break;
        case PW_LIST:     draw_list(w);     break;
        default: break;
        }
        return;
    }
    pw_rect_outline(w->x, w->y, w->w, w->h, PW_COL_ACCENT);
    if (w->w > 2 && w->h > 2)
        pw_rect_outline((int16_t)(w->x + 1), (int16_t)(w->y + 1),
                         (int16_t)(w->w - 2), (int16_t)(w->h - 2), PW_COL_ACCENT);
}

// ── Full window repaint (A3 — no offscreen buffer, so the modal stack's top
// change or a first win_show() needs every widget redrawn from scratch) ────

void pw_win_full_repaint(purr_win_t win) {
    const catcall_display_t *disp = purr_kernel_display();
    if (!disp) return;
    display_info_t info;
    disp->get_info(&info);
    pw_fill_rect(0, 0, (int16_t)info.width, (int16_t)info.height, PW_COL_BG);

    for (int i = 0; i < PW_MAX_WIDS; i++) {
        pw_widget_t *w = &s_pw_wids[i];
        if (w->alive && w->win == win && w->kind != PW_LAYOUT)
            pw_redraw_widget(pw_wid_handle(i));
    }
}

#endif  // CONFIG_PURR_UI_BACKEND_POUNCE
