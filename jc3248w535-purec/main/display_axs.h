// Native AXS15231B QSPI display driver for the Guition JC3248W535 (pure C).
// Pinout + vendor init sequence from the working ESP32S3_QR bring-up.
//
// Provides a simple software-framebuffer text/graphics API (no LVGL, no Arduino).
// A background task pushes the full framebuffer to the panel periodically, since
// the AXS15231B QSPI variant ignores CASET/RASET and blanks itself when idle.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AXS_LCD_W 320   // native panel width  (portrait)
#define AXS_LCD_H 480   // native panel height

// RGB565 helper
static inline uint16_t axs_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void display_axs_init(void);
void display_axs_set_brightness(uint8_t level);   // 0 = off, otherwise on

void display_axs_clear(void);
void display_axs_set_text_colors(uint16_t fg565, uint16_t bg565);
void display_axs_text(uint8_t row, const char* text);   // size-2 line, y = 4 + row*18
void display_axs_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void display_axs_draw_hline(int16_t x, int16_t y, int16_t w, uint16_t color);
void display_axs_draw_string(int16_t x, int16_t y, const char* s,
                             uint16_t fg, uint16_t bg, uint8_t size);

// Push the framebuffer to the panel now (the refresh task also does this).
void display_axs_flush(void);

#ifdef __cplusplus
}
#endif
