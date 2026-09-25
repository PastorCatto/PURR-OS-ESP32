// purr_uiconf_render_fb.c — see purr_uiconf_render_fb.h for the full
// picture. Two real, hardware-found techniques are deliberately preserved
// from epaper_ui.c, not reinvented:
//   1. Every draw composes its WHOLE output into one scratch buffer and
//      calls push_pixels() exactly once — an early per-glyph version of
//      that file took ~58s to draw three short lines.
//   2. A selectable list row is redrawn as a real inverted background
//      fill (draw_row_at() below), not a "> " prefix, and cycling the
//      highlight only ever repaints the two affected rows
//      (purr_uiconf_render_fb_move_selection()), not the whole screen —
//      the fix for the exact "felt like a full refresh on every press"
//      complaint that shaped draw_list_row() on real hardware.
//
// First-pass scope, matching purr_uiconf_core.h's own header comment:
// only top-level widgets directly under the screen render (SECTION
// nesting is parsed/stored but skipped here, same as core's own
// purr_uiconf_visible_children() gap) — reintroduced once the first real
// vertical slice is proven on hardware.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "purr_uiconf_render_fb.h"
#include "purr_uiconf_sources.h"

#define FONT_W 6
#define FONT_H 8
#define DEFAULT_SCALE 2   // matches the "system font minimum 2" policy epaper_ui.c established; a widget's own `scale` attr can go lower for a special case (e-reader-style app), same allowance that policy carved out
#define TITLE_SCALE   3
#define ROW_PAD       6
#define TEXT_MAX_CHARS 64

#define COLOR_BLACK 0x0000u
#define COLOR_WHITE 0xFFFFu

// Font table copied from epaper_ui.c (itself copied from purr_fbtty.c /
// boot_splash.c) — not shared, same reasoning those files' own comments
// already give: this module must stay independently linkable regardless
// of which other text-drawing modules a given build does or doesn't
// include.
static const uint8_t s_font6x8[][6] = {
    {0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00,0x00},
    {0x00,0x07,0x00,0x07,0x00,0x00}, {0x14,0x7F,0x14,0x7F,0x14,0x00},
    {0x24,0x2A,0x7F,0x2A,0x12,0x00}, {0x23,0x13,0x08,0x64,0x62,0x00},
    {0x36,0x49,0x55,0x22,0x50,0x00}, {0x00,0x05,0x03,0x00,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00,0x00}, {0x00,0x41,0x22,0x1C,0x00,0x00},
    {0x08,0x2A,0x1C,0x2A,0x08,0x00}, {0x08,0x08,0x3E,0x08,0x08,0x00},
    {0x00,0x50,0x30,0x00,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08,0x00},
    {0x00,0x60,0x60,0x00,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02,0x00},
    {0x3E,0x51,0x49,0x45,0x3E,0x00}, {0x00,0x42,0x7F,0x40,0x00,0x00},
    {0x42,0x61,0x51,0x49,0x46,0x00}, {0x21,0x41,0x45,0x4B,0x31,0x00},
    {0x18,0x14,0x12,0x7F,0x10,0x00}, {0x27,0x45,0x45,0x45,0x39,0x00},
    {0x3C,0x4A,0x49,0x49,0x30,0x00}, {0x01,0x71,0x09,0x05,0x03,0x00},
    {0x36,0x49,0x49,0x49,0x36,0x00}, {0x06,0x49,0x49,0x29,0x1E,0x00},
    {0x00,0x36,0x36,0x00,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00,0x00}, {0x14,0x14,0x14,0x14,0x14,0x00},
    {0x00,0x41,0x22,0x14,0x08,0x00}, {0x02,0x01,0x51,0x09,0x06,0x00},
    {0x32,0x49,0x79,0x41,0x3E,0x00}, {0x7E,0x11,0x11,0x11,0x7E,0x00},
    {0x7F,0x49,0x49,0x49,0x36,0x00}, {0x3E,0x41,0x41,0x41,0x22,0x00},
    {0x7F,0x41,0x41,0x22,0x1C,0x00}, {0x7F,0x49,0x49,0x49,0x41,0x00},
    {0x7F,0x09,0x09,0x09,0x01,0x00}, {0x3E,0x41,0x49,0x49,0x7A,0x00},
    {0x7F,0x08,0x08,0x08,0x7F,0x00}, {0x00,0x41,0x7F,0x41,0x00,0x00},
    {0x20,0x40,0x41,0x3F,0x01,0x00}, {0x7F,0x08,0x14,0x22,0x41,0x00},
    {0x7F,0x40,0x40,0x40,0x40,0x00}, {0x7F,0x02,0x04,0x02,0x7F,0x00},
    {0x7F,0x04,0x08,0x10,0x7F,0x00}, {0x3E,0x41,0x41,0x41,0x3E,0x00},
    {0x7F,0x09,0x09,0x09,0x06,0x00}, {0x3E,0x41,0x51,0x21,0x5E,0x00},
    {0x7F,0x09,0x19,0x29,0x46,0x00}, {0x46,0x49,0x49,0x49,0x31,0x00},
    {0x01,0x01,0x7F,0x01,0x01,0x00}, {0x3F,0x40,0x40,0x40,0x3F,0x00},
    {0x1F,0x20,0x40,0x20,0x1F,0x00}, {0x3F,0x40,0x38,0x40,0x3F,0x00},
    {0x63,0x14,0x08,0x14,0x63,0x00}, {0x07,0x08,0x70,0x08,0x07,0x00},
    {0x61,0x51,0x49,0x45,0x43,0x00}, {0x00,0x7F,0x41,0x41,0x00,0x00},
    {0x02,0x04,0x08,0x10,0x20,0x00}, {0x00,0x41,0x41,0x7F,0x00,0x00},
    {0x04,0x02,0x01,0x02,0x04,0x00}, {0x40,0x40,0x40,0x40,0x40,0x00},
    {0x00,0x01,0x02,0x04,0x00,0x00}, {0x20,0x54,0x54,0x54,0x78,0x00},
    {0x7F,0x48,0x44,0x44,0x38,0x00}, {0x38,0x44,0x44,0x44,0x20,0x00},
    {0x38,0x44,0x44,0x48,0x7F,0x00}, {0x38,0x54,0x54,0x54,0x18,0x00},
    {0x08,0x7E,0x09,0x01,0x02,0x00}, {0x0C,0x52,0x52,0x52,0x3E,0x00},
    {0x7F,0x08,0x04,0x04,0x78,0x00}, {0x00,0x44,0x7D,0x40,0x00,0x00},
    {0x20,0x40,0x44,0x3D,0x00,0x00}, {0x7F,0x10,0x28,0x44,0x00,0x00},
    {0x00,0x41,0x7F,0x40,0x00,0x00}, {0x7C,0x04,0x18,0x04,0x78,0x00},
    {0x7C,0x08,0x04,0x04,0x78,0x00}, {0x38,0x44,0x44,0x44,0x38,0x00},
    {0x7C,0x14,0x14,0x14,0x08,0x00}, {0x08,0x14,0x14,0x18,0x7C,0x00},
    {0x7C,0x08,0x04,0x04,0x08,0x00}, {0x48,0x54,0x54,0x54,0x20,0x00},
    {0x04,0x3F,0x44,0x40,0x20,0x00}, {0x3C,0x40,0x40,0x20,0x7C,0x00},
    {0x1C,0x20,0x40,0x20,0x1C,0x00}, {0x3C,0x40,0x30,0x40,0x3C,0x00},
    {0x44,0x28,0x10,0x28,0x44,0x00}, {0x0C,0x50,0x50,0x50,0x3C,0x00},
    {0x44,0x64,0x54,0x4C,0x44,0x00}, {0x00,0x08,0x36,0x41,0x00,0x00},
    {0x00,0x00,0x7F,0x00,0x00,0x00}, {0x00,0x41,0x36,0x08,0x00,0x00},
    {0x08,0x04,0x08,0x10,0x08,0x00},
};

// ── Init/deinit ──────────────────────────────────────────────────────

bool purr_uiconf_render_fb_init(purr_uiconf_render_fb_t *r, const catcall_display_t *disp)
{
    memset(r, 0, sizeof(*r));
    if (!disp) return false;
    r->disp = disp;
    r->panel_w = 200;
    r->panel_h = 200;   // overwritten below if get_info() reports real values
    display_info_t info = {0};
    if (disp->get_info) disp->get_info(&info);
    if (info.width)  r->panel_w = info.width;
    if (info.height) r->panel_h = info.height;
    return true;
}

void purr_uiconf_render_fb_deinit(purr_uiconf_render_fb_t *r)
{
    if (r->scratch) free(r->scratch);
    memset(r, 0, sizeof(*r));
}

static bool ensure_scratch(purr_uiconf_render_fb_t *r, size_t px_needed)
{
    if (r->scratch_cap >= px_needed) return true;
    uint16_t *buf = realloc(r->scratch, px_needed * sizeof(uint16_t));
    if (!buf) return false;
    r->scratch = buf;
    r->scratch_cap = px_needed;
    return true;
}

// ── Text primitives ──────────────────────────────────────────────────

static int row_height(int scale) { return FONT_H * scale + ROW_PAD; }

// Paints a full glyph-cell rectangle (both fg AND bg pixels) — for text
// drawn directly onto an already-cleared background. See draw_row_at()
// below for the other technique (bg fill once, then only lit pixels) a
// selectable list row uses instead.
static void draw_text(purr_uiconf_render_fb_t *r, int x_px, int y_px, const char *text,
                       int scale, uint16_t fg, uint16_t bg)
{
    if (!r->disp || !r->disp->push_pixels) return;
    int len = (int)strlen(text);
    if (len > TEXT_MAX_CHARS) len = TEXT_MAX_CHARS;
    if (len <= 0) return;

    int gw = FONT_W * scale, gh = FONT_H * scale;
    int line_w = len * gw;
    if (!ensure_scratch(r, (size_t)line_w * gh)) return;
    uint16_t *buf = r->scratch;

    for (int i = 0; i < len; i++) {
        char c = (text[i] < 0x20 || text[i] > 0x7E) ? ' ' : text[i];
        const uint8_t *glyph = s_font6x8[(unsigned char)c - 0x20];
        for (int cx = 0; cx < FONT_W; cx++) {
            uint8_t bits = glyph[cx];
            for (int cy = 0; cy < FONT_H; cy++) {
                uint16_t color = (bits & (1 << cy)) ? fg : bg;
                int base_x = i * gw + cx * scale;
                int base_y = cy * scale;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        buf[(base_y + sy) * line_w + (base_x + sx)] = color;
            }
        }
    }
    r->disp->push_pixels(x_px, y_px, line_w, gh, buf);
}

// Paints one full-width selectable row: background filled solid first
// (black if selected — a real inverted highlight, not a "> " prefix),
// then only the LIT glyph pixels are stamped over it. This is
// draw_list_row()'s own exact technique, generalized over an arbitrary
// panel width instead of a fixed 200px.
static void draw_row_at(purr_uiconf_render_fb_t *r, int y_px, int row_h, const char *label,
                         int scale, bool selected)
{
    if (!r->disp || !r->disp->push_pixels) return;
    int w = r->panel_w;
    if (w <= 0 || row_h <= 0) return;
    if (!ensure_scratch(r, (size_t)w * row_h)) return;
    uint16_t *buf = r->scratch;

    uint16_t fg = selected ? COLOR_WHITE : COLOR_BLACK;
    uint16_t bg = selected ? COLOR_BLACK : COLOR_WHITE;
    for (int i = 0; i < w * row_h; i++) buf[i] = bg;

    int gw = FONT_W * scale, gh = FONT_H * scale;
    int len = (int)strlen(label);
    if (len > TEXT_MAX_CHARS) len = TEXT_MAX_CHARS;
    int text_y0 = (row_h - gh) / 2;
    if (text_y0 < 0) text_y0 = 0;
    for (int i = 0; i < len; i++) {
        int base_x0 = 4 + i * gw;
        if (base_x0 + gw > w) break;   // ran off the right edge — truncate, don't overrun the row buffer
        char c = (label[i] < 0x20 || label[i] > 0x7E) ? ' ' : label[i];
        const uint8_t *glyph = s_font6x8[(unsigned char)c - 0x20];
        for (int cx = 0; cx < FONT_W; cx++) {
            uint8_t bits = glyph[cx];
            for (int cy = 0; cy < FONT_H; cy++) {
                if (!(bits & (1 << cy))) continue;   // background already painted — only stamp lit pixels
                int base_x = base_x0 + cx * scale;
                int base_y = text_y0 + cy * scale;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        buf[(base_y + sy) * w + (base_x + sx)] = fg;
            }
        }
    }
    r->disp->push_pixels(0, y_px, w, row_h, buf);
}

// ── Widget layout ────────────────────────────────────────────────────
// List/source resolution (source_id_of/count/label/target,
// find_first_list_widget, list_item_count/label) now lives in
// purr_uiconf_sources.c/h, shared with purr_uiconf_render_winapi.c — see
// that header's own top comment for why this was pulled out rather than
// kept duplicated here.
// Widgets stack top-to-bottom in document order, each occupying a row (or
// N rows, for a list) sized by its own `scale` attr (default
// DEFAULT_SCALE). Deterministic and shared between the full draw and
// move_selection()'s own row-position math below — one source of truth
// so the two never disagree about where a row actually is on screen.

static int widget_scale(const purr_uiconf_screen_t *s, int node)
{
    float sc;
    if (purr_uiconf_attr_num(s, node, "scale", &sc) && sc >= 1) return (int)sc;
    return DEFAULT_SCALE;
}

static int widget_height(const purr_uiconf_screen_t *s, int node)
{
    int scale = widget_scale(s, node);
    if (purr_uiconf_kind(s, node) == PUI_KIND_WIDGET_LIST) {
        int count = purr_uiconf_list_item_count(s, node);
        if (count <= 0) count = 1;   // the "empty" placeholder still takes one row
        return count * row_height(scale);
    }
    return row_height(scale);   // TEXT / BUTTON
}

// Walks top-level children in order, summing heights, resolving a FLAT
// selectable index (purr_uiconf_selectable_count()/_at()'s own numbering
// — every list row plus every standalone button, in document order) to
// its on-screen y. Must stay in the same order/skip rules as
// purr_uiconf_selectable_at() or the two will disagree about where a row
// actually is.
static int selectable_row_y(const purr_uiconf_screen_t *s, int flat_index)
{
    int y = 0;
    char title[40];
    if (purr_uiconf_attr_str(s, PUI_ROOT, "title", title, sizeof(title)))
        y += FONT_H * TITLE_SCALE + 4;

    int seen = 0;
    for (int child = purr_uiconf_first_child(s, PUI_ROOT); child != PUI_NONE;
         child = purr_uiconf_next_sibling(s, child)) {
        pui_kind_t k = purr_uiconf_kind(s, child);
        if (k == PUI_KIND_ON_HANDLER || k == PUI_KIND_SECTION) continue;
        if (!purr_uiconf_visible(s, child)) continue;

        if (k == PUI_KIND_WIDGET_LIST) {
            int count = purr_uiconf_list_item_count(s, child);
            if (count > 0) {
                int rh = row_height(widget_scale(s, child));
                if (flat_index >= seen && flat_index < seen + count)
                    return y + (flat_index - seen) * rh;
                seen += count;
            }
        } else if (k == PUI_KIND_WIDGET_BUTTON) {
            if (flat_index == seen) return y;
            seen += 1;
        }
        y += widget_height(s, child);
    }
    return y;
}

// A selectable entry's own display label — a list row's real item text,
// or a button's `[ label ]`, same bracket style the full draw below uses.
static bool selectable_label(const purr_uiconf_screen_t *s, purr_uiconf_selectable_t sel, char *out, size_t sz)
{
    if (sel.item_index < 0) {
        char label[40] = {0};
        purr_uiconf_attr_str(s, sel.node, "label", label, sizeof(label));
        snprintf(out, sz, "[ %s ]", label);
        return true;
    }
    return purr_uiconf_list_item_label(s, sel.node, sel.item_index, out, sz);
}

// ── Full-screen draw ─────────────────────────────────────────────────

void purr_uiconf_render_fb_draw(purr_uiconf_render_fb_t *r, const purr_uiconf_state_t *state)
{
    const purr_uiconf_screen_t *s = &state->screen;
    if (!r->disp || !s->data) return;
    if (r->disp->fill_rect) r->disp->fill_rect(0, 0, r->panel_w, r->panel_h, COLOR_WHITE);

    int y = 0;
    char title[40];
    if (purr_uiconf_attr_str(s, PUI_ROOT, "title", title, sizeof(title))) {
        draw_text(r, 4, 0, title, TITLE_SCALE, COLOR_BLACK, COLOR_WHITE);
        y += FONT_H * TITLE_SCALE + 4;
    }

    // Running index into the unified selectable sequence (every list row
    // plus every standalone button, in document order) — see
    // purr_uiconf_sources.h's own comment. Only WIDGET_LIST/WIDGET_BUTTON
    // advance it; a plain WIDGET_TEXT is never selectable.
    int flat = 0;

    for (int child = purr_uiconf_first_child(s, PUI_ROOT); child != PUI_NONE;
         child = purr_uiconf_next_sibling(s, child)) {
        pui_kind_t k = purr_uiconf_kind(s, child);
        if (k == PUI_KIND_ON_HANDLER || k == PUI_KIND_SECTION) continue;
        if (!purr_uiconf_visible(s, child)) continue;

        int scale = widget_scale(s, child);

        if (k == PUI_KIND_WIDGET_TEXT) {
            char buf[64] = {0};
            char bind_name[24], format[32];
            bool have_bind = purr_uiconf_attr_str(s, child, "bind", bind_name, sizeof(bind_name));
            bool have_format = purr_uiconf_attr_str(s, child, "format", format, sizeof(format));
            if (have_bind) {
                purr_uiconf_resolve_bind(bind_name, have_format ? format : NULL, buf, sizeof(buf));
            } else {
                purr_uiconf_attr_str(s, child, "text", buf, sizeof(buf));
            }
            char align[16];
            bool centered = purr_uiconf_attr_str(s, child, "align", align, sizeof(align))
                             && strcmp(align, "center") == 0;
            int text_w = (int)strlen(buf) * FONT_W * scale;
            int x = centered ? (r->panel_w - text_w) / 2 : 4;
            if (x < 0) x = 0;
            draw_text(r, x, y, buf, scale, COLOR_BLACK, COLOR_WHITE);
            y += row_height(scale);

        } else if (k == PUI_KIND_WIDGET_BUTTON) {
            char label[40] = {0};
            purr_uiconf_attr_str(s, child, "label", label, sizeof(label));
            char bracketed[46];
            snprintf(bracketed, sizeof(bracketed), "[ %s ]", label);
            bool sel = (flat == state->selected_index);
            draw_row_at(r, y, row_height(scale), bracketed, scale, sel);
            y += row_height(scale);
            flat += 1;

        } else if (k == PUI_KIND_WIDGET_LIST) {
            int count = purr_uiconf_list_item_count(s, child);
            int rh = row_height(scale);
            if (count <= 0) {
                char empty[40];
                if (!purr_uiconf_attr_str(s, child, "empty_text", empty, sizeof(empty)))
                    snprintf(empty, sizeof(empty), "Empty");
                draw_text(r, 4, y, empty, scale, COLOR_BLACK, COLOR_WHITE);
                y += rh;
            } else {
                for (int i = 0; i < count; i++) {
                    char label[40] = {0};
                    purr_uiconf_list_item_label(s, child, i, label, sizeof(label));
                    bool sel = (flat + i == state->selected_index);
                    draw_row_at(r, y + i * rh, rh, label, scale, sel);
                }
                y += count * rh;
                flat += count;
            }
        }
    }
}

// ── Selection ────────────────────────────────────────────────────────

// Redraws whichever single row `flat_index` refers to (a list row or a
// standalone button, per the unified model) with the given highlight
// state — the two-row-diff step move_selection() below uses for both the
// row losing the highlight and the one gaining it.
static void redraw_selectable_row(purr_uiconf_render_fb_t *r, const purr_uiconf_screen_t *s,
                                   int flat_index, bool selected)
{
    purr_uiconf_selectable_t sel;
    if (!purr_uiconf_selectable_at(s, flat_index, &sel)) return;
    char label[40];
    if (!selectable_label(s, sel, label, sizeof(label))) return;
    int scale = widget_scale(s, sel.node);
    draw_row_at(r, selectable_row_y(s, flat_index), row_height(scale), label, scale, selected);
}

int purr_uiconf_render_fb_move_selection(purr_uiconf_render_fb_t *r, purr_uiconf_state_t *state, int delta)
{
    const purr_uiconf_screen_t *s = &state->screen;
    int total = purr_uiconf_selectable_count(s);
    if (total <= 0) return state->selected_index;

    int old = state->selected_index;
    int new_idx = ((old + delta) % total + total) % total;
    if (new_idx == old) return old;
    state->selected_index = new_idx;

    redraw_selectable_row(r, s, old, false);
    redraw_selectable_row(r, s, new_idx, true);
    return new_idx;
}

bool purr_uiconf_render_fb_selected_target(const purr_uiconf_render_fb_t *r, const purr_uiconf_state_t *state,
                                            char *out, size_t out_sz)
{
    (void)r;
    const purr_uiconf_screen_t *s = &state->screen;
    purr_uiconf_selectable_t sel;
    if (!purr_uiconf_selectable_at(s, state->selected_index, &sel)) return false;
    if (sel.item_index < 0) return false;   // a standalone button has no "$item" — see purr_uiconf_render_fb_selected_node()
    return purr_uiconf_list_item_target(s, sel.node, sel.item_index, out, out_sz);
}

bool purr_uiconf_render_fb_selected_node(const purr_uiconf_render_fb_t *r, const purr_uiconf_state_t *state,
                                          int *out_node, int *out_item_index)
{
    (void)r;
    purr_uiconf_selectable_t sel;
    if (!purr_uiconf_selectable_at(&state->screen, state->selected_index, &sel)) return false;
    if (out_node) *out_node = sel.node;
    if (out_item_index) *out_item_index = sel.item_index;
    return true;
}
