#pragma once
// kittenui_desktop.h — Windows XP-style desktop shell for KittenUI

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the XP shell: desktop, taskbar, Start button.
// Call once from kittenui_task() after LVGL is up.
void kittenui_desktop_init(void);

// Open a new LVGL window on the desktop for a launched app.
// Returns the lv_win object (or NULL on failure).
// Called by the app_manager integration when an app is launched.
struct _lv_obj_t *kittenui_desktop_open_window(const char *title);

// Tick — call from the LVGL timer handler task to update the clock.
void kittenui_desktop_tick(void);

#ifdef __cplusplus
}
#endif
