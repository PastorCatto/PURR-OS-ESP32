// wince_common.h — shared WinCE-style chrome helpers for the baked-in shell
// and its app windows. Colour palette and bevel drawing ported from
// PURR-OS-0.11/devices/tdeck_plus/purr_app.cpp.
#pragma once
#include "miniwin.h"
#include "gl/gl.h"

#define WCE_DESKTOP 0x008080
#define WCE_BAR     0xC0C0C0
#define WCE_HI      0xFFFFFF
#define WCE_SHD     0x808080
#define WCE_DARK    0x404040
#define WCE_TXT     0x000000
#define WCE_MBKG    0xD4D0C8

#define TASKBAR_H   22

#ifdef __cplusplus
extern "C" {
#endif

void wince_draw_raised(const mw_gl_draw_info_t *d,
                        int16_t x, int16_t y, int16_t w, int16_t h,
                        uint32_t fill);
void wince_draw_sunken(const mw_gl_draw_info_t *d,
                        int16_t x, int16_t y, int16_t w, int16_t h,
                        uint32_t fill);

// Returns the shell's window handle (single full-screen background window),
// used by apps to register/unregister with the taskbar and to repaint.
mw_handle_t wince_shell_handle(void);

#ifdef __cplusplus
}
#endif
