// moy_draw.c — the drawing verbs (spec 6, 7.1, 7.2).
//
// Everything writes 8-bit palette indices into g_port.fb8, which
// purr_port_present() expands through the 64-entry RGB565 LUT and pushes. That
// split is the whole reason this file has no display code in it: the console
// only ever deals in indices, exactly as the spec describes it.
//
// ── Order of operations ─────────────────────────────────────────────────────
// Every verb applies, in this order:
//   1. camera   — subtract cam (spec 6, camera())
//   2. clip     — reject outside the clip rect (clip())
//   3. pal      — remap the colour index (pal())
// palt() applies only where a source pixel is read (sprites, map), never to a
// primitive's fill colour: a transparent index is a property of the SOURCE ART,
// not of a rectangle you asked for.

#include <string.h>
#include <stdlib.h>

#include "moy.h"

moy_t       g_moy;
purr_port_t g_port;

// ── Plot ────────────────────────────────────────────────────────────────────

// The single point every verb funnels through. Camera and clip applied here so
// no caller can forget; colour remap too. Deliberately not inlined by hand —
// the compiler does it, and one definition means one place to be wrong.
static inline void plot(int x, int y, int c)
{
    x -= g_moy.cam_x;
    y -= g_moy.cam_y;

    if (x < g_moy.clip_x || y < g_moy.clip_y) return;
    if (x >= g_moy.clip_x + g_moy.clip_w) return;
    if (y >= g_moy.clip_y + g_moy.clip_h) return;
    if (x < 0 || y < 0 || x >= MOY_SCREEN_W || y >= MOY_SCREEN_H) return;

    g_port.fb8[y * MOY_SCREEN_W + x] = g_moy.pal_map[c & (MOY_PAL_N - 1)];
}

void moy_draw_reset(void)
{
    g_moy.clip_x = 0; g_moy.clip_y = 0;
    g_moy.clip_w = MOY_SCREEN_W; g_moy.clip_h = MOY_SCREEN_H;
    g_moy.cam_x = 0; g_moy.cam_y = 0;
    for (int i = 0; i < MOY_PAL_N; i++) {
        g_moy.pal_map[i] = (uint8_t)i;
        g_moy.transparent[i] = false;
    }
    // Index 0 transparent by default (spec 7.1): spr() without an explicit
    // colorkey must not paint the background of its tile.
    g_moy.transparent[0] = true;
}

// ── Primitives ──────────────────────────────────────────────────────────────

void moy_cls(int c)
{
    // Bypasses plot(): cls clears the WHOLE screen regardless of camera and
    // clip. Treating it as a full-screen rect() would make it respect both,
    // which is not what "clear the screen" means and would leave stale pixels
    // outside a clip rect the cart had set for something else.
    memset(g_port.fb8, g_moy.pal_map[c & (MOY_PAL_N - 1)],
           (size_t)MOY_SCREEN_W * MOY_SCREEN_H);
}

void moy_pix(int x, int y, int c) { plot(x, y, c); }

void moy_line(int x0, int y0, int x1, int y1, int c)
{
    // Bresenham, integer only. The spec's numbers are doubles (4.2) but every
    // coordinate is floored before it reaches here.
    int dx =  abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        plot(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void moy_rect(int x, int y, int w, int h, int c)
{
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            plot(x + i, y + j, c);
}

void moy_rectb(int x, int y, int w, int h, int c)
{
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < w; i++) { plot(x + i, y, c); plot(x + i, y + h - 1, c); }
    for (int j = 0; j < h; j++) { plot(x, y + j, c); plot(x + w - 1, y + j, c); }
}

void moy_circ(int cx, int cy, int r, int c)
{
    if (r < 0) return;
    // Filled: horizontal spans per row. Midpoint would need span reconstruction
    // anyway, and this is exact with no double-plotted pixels — which matters
    // because plot() is not idempotent once a cart uses a blend-ish pal remap.
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)(__builtin_sqrt((double)(r * r - dy * dy)));
        for (int x = -dx; x <= dx; x++) plot(cx + x, cy + dy, c);
    }
}

void moy_circb(int cx, int cy, int r, int c)
{
    if (r < 0) return;
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        plot(cx + x, cy + y, c); plot(cx + y, cy + x, c);
        plot(cx - y, cy + x, c); plot(cx - x, cy + y, c);
        plot(cx - x, cy - y, c); plot(cx - y, cy - x, c);
        plot(cx + y, cy - x, c); plot(cx + x, cy - y, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

// ── Text ────────────────────────────────────────────────────────────────────

void moy_print(const char *s, int x, int y, int c)
{
    if (!s) return;
    for (; *s; s++) {
        unsigned ch = (unsigned char)*s;
        if (ch < 0x20 || ch > 0x7F) { x += 8; continue; }
        const uint8_t *g = &moy_font8x8[(ch - 0x20) * 8];
        // font_petme128_8x8 is COLUMN-major: byte i is column i, bit j is row j.
        for (int col = 0; col < 8; col++) {
            uint8_t bits = g[col];
            for (int row = 0; row < 8; row++)
                if (bits & (1u << row)) plot(x + col, y + row, c);
        }
        x += 8;
    }
}

// ── Sprites ─────────────────────────────────────────────────────────────────

// Is this source index transparent for this draw?
//
// colorkey < 0 means "use the palt() state" (the normal case). colorkey >= 0
// means the cart named one index for this call, which OVERRIDES palt entirely —
// otherwise a cart that had set palt(0,false) could not ask for a one-off key.
static inline bool src_transparent(int idx, int colorkey)
{
    return (colorkey >= 0) ? (idx == colorkey) : g_moy.transparent[idx & (MOY_PAL_N - 1)];
}

void moy_spr(int n, int x, int y, int colorkey, int scale, int flip)
{
    if (!g_moy.sheet || n < 0 || n >= MOY_TILES) return;
    if (scale < 1) scale = 1;

    int tx = (n % 16) * 8;          // spec 3.2 — 16 tiles per row
    int ty = (n / 16) * 8;

    bool fx = (flip & 1) != 0;      // horizontal
    bool fy = (flip & 2) != 0;      // vertical

    for (int j = 0; j < 8; j++) {
        for (int i = 0; i < 8; i++) {
            int si = fx ? (7 - i) : i;
            int sj = fy ? (7 - j) : j;
            int idx = g_moy.sheet[(ty + sj) * MOY_SHEET_W + (tx + si)];
            if (src_transparent(idx, colorkey)) continue;

            if (scale == 1) {
                plot(x + i, y + j, idx);
            } else {
                for (int sy2 = 0; sy2 < scale; sy2++)
                    for (int sx2 = 0; sx2 < scale; sx2++)
                        plot(x + i * scale + sx2, y + j * scale + sy2, idx);
            }
        }
    }
}

void moy_sspr(int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh,
              int colorkey, int flip)
{
    if (!g_moy.sheet || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;

    bool fx = (flip & 1) != 0;
    bool fy = (flip & 2) != 0;

    // Nearest-neighbour, destination-driven so every destination pixel is
    // written exactly once — a source-driven loop leaves gaps when scaling up
    // and double-writes when scaling down.
    for (int j = 0; j < dh; j++) {
        int v = (j * sh) / dh;
        if (fy) v = sh - 1 - v;
        for (int i = 0; i < dw; i++) {
            int u = (i * sw) / dw;
            if (fx) u = sw - 1 - u;

            int px = sx + u, py = sy + v;
            if (px < 0 || py < 0 || px >= MOY_SHEET_W || py >= MOY_SHEET_H) continue;

            int idx = g_moy.sheet[py * MOY_SHEET_W + px];
            if (src_transparent(idx, colorkey)) continue;
            plot(dx + i, dy + j, idx);
        }
    }
}

// ── Map ─────────────────────────────────────────────────────────────────────

int moy_mget(int x, int y)
{
    if (!g_moy.map || x < 0 || y < 0 || x >= g_moy.map_w || y >= g_moy.map_h) return -1;
    uint8_t v = g_moy.map[y * g_moy.map_w + x];
    return (v == 0xFF) ? -1 : v;    // 0xFF is our "empty"; see moy_cart.c
}

void moy_mset(int x, int y, int tile)
{
    if (!g_moy.map || x < 0 || y < 0 || x >= g_moy.map_w || y >= g_moy.map_h) return;
    g_moy.map[y * g_moy.map_w + x] =
        (tile < 0 || tile > MOY_TILE_MAX_MAP) ? 0xFF : (uint8_t)tile;
}

void moy_map(int mx, int my, int w, int h, int sx, int sy, int colorkey, int scale)
{
    if (!g_moy.map) return;
    if (scale < 1) scale = 1;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int t = moy_mget(mx + i, my + j);
            if (t < 0) continue;    // empty cell — draw nothing, not tile 0
            moy_spr(t, sx + i * 8 * scale, sy + j * 8 * scale, colorkey, scale, 0);
        }
    }
}
