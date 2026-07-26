#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Render a CGA text-mode framebuffer (80x25 char+attr pairs) into an RGB565
// pixel buffer.
//
// RGB565, not RGB888: catcall_display_t::push_pixels() takes RGB565, and the
// CGA palette below is already in that format - the previous RGB888 output
// existed only for MiniWin's mw_gl_colour_bitmap() and meant converting each
// pixel to a wider format that then had to be converted back.
//
// out_rgb565 must be out_w * out_h * 2 bytes.
// out_w / out_h: destination pixel dimensions (the full screen under game mode).
void magidos_cga_render(const uint8_t *vram, int cols, int rows,
                         uint16_t *out_rgb565, int out_w, int out_h);

// Standard CGA 16-colour palette as RGB565 (for reference / other users)
extern const uint16_t cga_palette_rgb565[16];

#ifdef __cplusplus
}
#endif
