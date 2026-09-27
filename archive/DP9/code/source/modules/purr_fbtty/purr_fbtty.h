#pragma once
// purr_fbtty.h — a purr_console_io_t binding that draws to the physical
// display and reads from the physical keyboard, instead of a serial
// transport. The actual point of "console mode": a device with no UI
// backend installed is still a fully usable standalone computer — type on
// its own keyboard, read its own screen, no cable to another machine
// required. See archive/ui_backends_v1/README.md for why there's
// currently no UI backend to hand off to.
//
// Draws directly via catcall_display_t (push_pixels/fill_rect), the same
// layer boot_splash.c uses — no LVGL, no UI-backend dependency, so this
// works in the exact situation it exists for: no UI backend compiled in
// at all. Reads via catcall_input_t (purr_kernel_input()) — bbq20's own
// driver already hands back real ASCII byte values in input_event_t's
// keycode field (confirmed against bbq20.c's own I2C FIFO read: the
// keyboard's own MCU firmware sends ASCII, not raw scancodes), so no
// keycode-to-character translation table is needed here at all.

#include <stdbool.h>
#include "purr_console.h"

#ifdef __cplusplus
extern "C" {
#endif

// Binds against whatever catcall_display_t/catcall_input_t are currently
// registered (purr_kernel_display()/purr_kernel_input()). Returns false
// (nothing this can draw a console onto) if no display is registered —
// callers should fall back to a serial purr_console_io_t in that case.
// Safe to call even if no input (keyboard) is registered yet — read_byte()
// just returns -1 (no key available) until one is.
bool purr_fbtty_init(void);

// The purr_console_io_t binding — pass this to purr_console_run() once
// purr_fbtty_init() has returned true.
extern const purr_console_io_t purr_fbtty_io;

#ifdef __cplusplus
}
#endif
