/* mini_gfx.c - see mini_gfx.h. */
#include "mini_gfx.h"

#include <string.h>

#include "mini_board.h"
#include "mini_lcd.h"

uint16_t mini_mix(uint16_t a, uint16_t b, int t)
{
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    int r = ar + (br - ar) * t / 255, g = ag + (bg - ag) * t / 255, bl = ab + (bb - ab) * t / 255;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

static const mini_glyph_t *glyph(const mini_font_t *f, char c)
{
    unsigned u = (unsigned char)c;
    if (u < f->first || u > f->last) {
        u = '?';
    }
    return &f->glyphs[u - f->first];
}

int mini_text_width(const mini_font_t *f, const char *s)
{
    int w = 0;
    for (; *s != '\0'; s++) {
        w += glyph(f, *s)->adv;
    }
    return w;
}

#define TEXT_BUF_PX (MINI_W * 26)
static uint16_t s_text[TEXT_BUF_PX];

void mini_draw_text_in(int x, int y, int w, int align, const mini_font_t *f, const char *s, uint16_t fg, uint16_t bg)
{
    int h = f->line_h;
    if (w > MINI_W) {
        w = MINI_W;
    }
    if (w <= 0 || h <= 0 || w * h > TEXT_BUF_PX) {
        return;
    }
    for (int i = 0; i < w * h; i++) {
        s_text[i] = bg;
    }
    int tw = mini_text_width(f, s);
    int pen = align == 1 ? (w - tw) / 2 : (align == 2 ? w - tw : 0);
    for (; *s != '\0'; s++) {
        const mini_glyph_t *g = glyph(f, *s);
        for (int row = 0; row < g->h; row++) {
            int py = g->yo + row;
            if (py < 0 || py >= h) {
                continue;
            }
            for (int col = 0; col < g->w; col++) {
                int px = pen + g->xo + col;
                if (px < 0 || px >= w) {
                    continue;
                }
                int idx = row * g->w + col;
                int a = (f->bits[g->off + idx / 2] >> ((idx & 1) ? 0 : 4)) & 15;
                if (a) {
                    s_text[py * w + px] = mini_mix(s_text[py * w + px], fg, a * 17);
                }
            }
        }
        pen += g->adv;
    }
    mini_lcd_write(x, y, w, h, s_text);
}

void mini_draw_text(int x, int y, const mini_font_t *f, const char *s, uint16_t fg, uint16_t bg)
{
    mini_draw_text_in(x, y, mini_text_width(f, s) + 1, 0, f, s, fg, bg);
}

void mini_fill(int x, int y, int w, int h, uint16_t c)
{
    mini_lcd_fill(x, y, w, h, c);
}

void mini_fill_gradient(int x, int y, int w, int h, uint16_t top, uint16_t bottom)
{
    for (int i = 0; i < h; i++) {
        mini_lcd_fill(x, y + i, w, 1, mini_mix(top, bottom, h > 1 ? i * 255 / (h - 1) : 0));
    }
}

void mini_frame(int x, int y, int w, int h, uint16_t c)
{
    mini_lcd_fill(x, y, w, 1, c);
    mini_lcd_fill(x, y + h - 1, w, 1, c);
    mini_lcd_fill(x, y, 1, h, c);
    mini_lcd_fill(x + w - 1, y, 1, h, c);
}
