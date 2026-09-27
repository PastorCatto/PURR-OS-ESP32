/*
 * purr_gfx.h - small drawing helpers on top of the display catcall: solid
 * rectangles and text. Enough for a boot menu and a console, not a UI toolkit.
 */
#ifndef PURR_GFX_H
#define PURR_GFX_H

#include <stdint.h>

#include "purr_display.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Draw text with the built-in 8x8 font, scaled by an integer factor. */
esp_err_t purr_gfx_text(const purr_display_v2_t *d, int x, int y, const char *s,
                        uint16_t fg, uint16_t bg, int scale);

/* Width and height in pixels of a string drawn by purr_gfx_text. */
int purr_gfx_text_width(const char *s, int scale);
int purr_gfx_text_height(int scale);

#ifdef __cplusplus
}
#endif

#endif /* PURR_GFX_H */
