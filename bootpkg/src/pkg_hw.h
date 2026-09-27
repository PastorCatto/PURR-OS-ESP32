/*
 * pkg_hw.h - the hardware the boot menu needs: an ST7789 panel driven by bit-banged
 * SPI and a keyboard read by bit-banged I2C. Both go straight to the ESP32-S3 GPIO
 * registers, so the package needs no driver code from the bootloader. Slow (a few MHz)
 * but plenty for a menu.
 */
#ifndef PKG_HW_H
#define PKG_HW_H

#include <stdint.h>

#include "purr_abi.h"

void hw_init(const purr_boot_services_t *svc);
int  hw_display_init(void);                 /* 0 on success */
void hw_fill(int x, int y, int w, int h, uint16_t rgb565);
void hw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale);
int  hw_text_width(const char *s, int scale);
char hw_key(void);                          /* next key, or 0 */
int  hw_kbd_ok(void);                       /* did the keyboard answer its last poll */
void hw_delay_ms(uint32_t ms);

#endif
