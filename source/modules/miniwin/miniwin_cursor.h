#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../../kernel/catcalls/catcall_input.h"

#ifdef __cplusplus
extern "C" {
#endif

// Cursor position and click state — read by hal_touch to inject pointer events.
void     miniwin_cursor_init(int display_w, int display_h);
// True when the mouse-style cursor overlay is compiled in (CURSOR_ENABLED in
// miniwin_cursor.c). While false, miniwin_keyboard_poll() treats trackball
// roll/click as a D-pad instead — the two modes are mutually exclusive, so
// this is the single switch both halves key off of.
bool     miniwin_cursor_enabled(void);
// Called by miniwin_input_pump for each event — returns true if event was consumed.
bool     miniwin_cursor_handle_event(const input_event_t *ev);
// Called after input pump drains all events — redraws cursor if dirty.
void     miniwin_cursor_poll(void);
uint16_t miniwin_cursor_x(void);
uint16_t miniwin_cursor_y(void);
bool     miniwin_cursor_pressed(void);  // trackball click active
bool     miniwin_cursor_visible(void);  // false when real touch is active

#ifdef __cplusplus
}
#endif
