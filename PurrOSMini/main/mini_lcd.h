/* mini_lcd.h - the ILI9341 panel of the CYD: init, fill, and writing a rectangle of pixels. No framebuffer: the panel has its own memory. */
#ifndef MINI_LCD_H
#define MINI_LCD_H

#include <stdint.h>

#include "esp_err.h"

esp_err_t mini_lcd_init(void);
void mini_lcd_backlight(int on);
/* c is a host-order RGB565 colour */
void mini_lcd_fill(int x, int y, int w, int h, uint16_t c);
/* host-order RGB565 pixels, w * h of them */
void mini_lcd_write(int x, int y, int w, int h, const uint16_t *px);
/* the wire format of the remote display (RGB565, little-endian bytes), w * h * 2 bytes */
void mini_lcd_write_le(int x, int y, int w, int h, const uint8_t *le);

#endif
