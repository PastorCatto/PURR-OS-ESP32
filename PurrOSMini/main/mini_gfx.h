/*
 * mini_gfx.h - PURR OS Mini's drawing: colours, text in Source Sans 3 (bitmap fonts made by tools/mini/gen_font.py, no LVGL), flat and gradient fills.
 * Everything is drawn straight to the panel (there is no framebuffer): text is composed in a small buffer on its flat background colour and sent as one rectangle.
 */
#ifndef MINI_GFX_H
#define MINI_GFX_H

#include <stdint.h>

#define MINI_RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

/* the Click Wheel palette */
#define MINI_WHITE   MINI_RGB(0xFF, 0xFF, 0xFF)
#define MINI_INK     MINI_RGB(0x15, 0x17, 0x1C)
#define MINI_DIM     MINI_RGB(0x7B, 0x81, 0x8C)
#define MINI_HAIR    MINI_RGB(0xE3, 0xE6, 0xEA)
#define MINI_SILVER0 MINI_RGB(0xF7, 0xF8, 0xFA)
#define MINI_SILVER1 MINI_RGB(0xC8, 0xCC, 0xD3)
#define MINI_EDGE    MINI_RGB(0x8A, 0x90, 0x9B)
#define MINI_KEY0    MINI_RGB(0xF7, 0xF8, 0xFA)
#define MINI_KEY1    MINI_RGB(0xDD, 0xE1, 0xE8)
#define MINI_ACCENT  MINI_RGB(0xE2, 0x67, 0x2B)
#define MINI_ACCENT_LIGHT MINI_RGB(0xEB, 0x93, 0x66)
#define MINI_ACCENT_DARK  MINI_RGB(0xB0, 0x50, 0x21)
#define MINI_GOOD    MINI_RGB(0x2F, 0x9E, 0x6B)
#define MINI_BAD     MINI_RGB(0xC8, 0x3C, 0x32)

typedef struct {
    uint16_t off;                       /* where its bitmap starts (4 bits a pixel, rows not padded, each glyph on a byte) */
    uint8_t w, h;
    int8_t xo, yo;                      /* the bitmap's left edge from the pen, and its top from the top of the line */
    uint8_t adv;
} mini_glyph_t;

typedef struct {
    const mini_glyph_t *glyphs;
    const uint8_t *bits;
    uint8_t first, last, line_h, base;
} mini_font_t;

extern const mini_font_t mini_font_title;      /* 20 px Bold */
extern const mini_font_t mini_font_row;        /* 16 px Semibold */
extern const mini_font_t mini_font_small;      /* 12 px Regular */

uint16_t mini_mix(uint16_t a, uint16_t b, int t);                       /* t 0..255 from a toward b */
int mini_text_width(const mini_font_t *f, const char *s);
void mini_draw_text(int x, int y, const mini_font_t *f, const char *s, uint16_t fg, uint16_t bg);          /* the box is the text's width by the line height, filled with bg */
void mini_draw_text_in(int x, int y, int w, int align, const mini_font_t *f, const char *s, uint16_t fg, uint16_t bg);   /* in a box w wide, 0 left, 1 centre, 2 right; the whole box is bg; clipped */
void mini_fill(int x, int y, int w, int h, uint16_t c);
void mini_fill_gradient(int x, int y, int w, int h, uint16_t top, uint16_t bottom);
void mini_frame(int x, int y, int w, int h, uint16_t c);               /* a 1 pixel outline */

#endif
