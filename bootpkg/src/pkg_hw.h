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
/* Makes whatever hw_fill()/hw_text() drew since the last flush actually visible. A no-op on
 * a panel that already draws immediately (pkg_hw.c's ST7789); on a slow panel (pkg_hw_epd.c)
 * this is the one real refresh -- batching every draw call between two flushes into one
 * refresh instead of one per call, the same lesson the archive's epaper_ui.c already learned
 * the hard way (its own top comment: one refresh per glyph measured at ~58s for three short
 * lines). pkg_main.c calls this once at the end of each logical screen update. */
void hw_flush(void);
char hw_key(void);                          /* next key, or 0 */
int  hw_kbd_ok(void);                       /* did the keyboard answer its last poll */
/* Advances this board's own notion of elapsed time by dt_ms, called once per tick from
 * pkg_main.c's loop (the same TICK_MS it already passes to purr_menu_step()). A no-op
 * where nothing needs it (pkg_hw.c's keyboard reads are immediate); pkg_hw_epd.c uses it
 * for the BOOT button's short-press-vs-hold threshold, since purr_boot_services_t has no
 * clock of its own. */
void hw_tick(uint32_t dt_ms);
void hw_delay_ms(uint32_t ms);
int  hw_secure_boot_enabled(void);          /* the real hardware eFuse, read-only */

#endif
