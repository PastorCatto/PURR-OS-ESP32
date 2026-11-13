/*
 * purr_lvgl.h - LVGL as the shell UI.
 *
 * purr_console.c (the text shell, main.c's run_shell()) is the fallback: main.c
 * (app_main()) tries purr_lvgl_init() first and only runs the text login/shell loop if that
 * fails, or if purr_lvgl_run() later returns because something called
 * purr_lvgl_report_critical_error(). Not yet covered here: the Hyprland-style config
 * language and the asset partition -- later chunks; real content (the config-driven screens)
 * replaces purr_lvgl_demo_screen() once that lands.
 */
#ifndef PURR_LVGL_H
#define PURR_LVGL_H

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sets up LVGL against purr_kernel_display(): allocates the draw buffer (PSRAM), registers
 * the flush callback, starts the tick timer. Returns an error (never touching LVGL's own
 * state further) if there is no display, or the draw buffer can't be allocated -- main.c
 * falls back to the text shell in that case. */
esp_err_t purr_lvgl_init(void);

/* Draws the proof-of-life screen (one label). Call once, after purr_lvgl_init() succeeds. */
void purr_lvgl_demo_screen(void);

/* Runs lv_timer_handler() in a loop. Returns when purr_lvgl_report_critical_error() has been
 * called (check purr_lvgl_critical_error_pending() to confirm that's why, same as main.c
 * does) -- otherwise runs forever, which is the normal case once there's real, working UI
 * content to stay on. */
void purr_lvgl_run(void);

/* Same loop, bounded by time instead -- for the "lvgldemo" shell command (commands.c): proves
 * the render pipeline works without going anywhere near the boot sequence. */
void purr_lvgl_run_for_ms(uint32_t ms);

/* Anything -- now, or a later chunk's config-driven UI code -- that hits a problem severe
 * enough that falling back to the text shell is safer than continuing (a missing/corrupt
 * config or asset the UI can't render without, say) calls this instead of pressing on. Logs
 * `why`. purr_lvgl_run() checks for it between lv_timer_handler() calls and returns promptly
 * once it's set; main.c then falls back to the text login/shell loop. This is cooperative,
 * not a crash handler -- it only helps if the code that hit the problem is still running
 * well enough to call it. */
void purr_lvgl_report_critical_error(const char *why);

/* True once purr_lvgl_report_critical_error() has been called. */
int purr_lvgl_critical_error_pending(void);

/* Screen orientation, in degrees clockwise from the panel's own: 0, 90, 180 or 270. The display
 * is rotated in software and LVGL swaps its width and height for 90 and 270, so a layout that
 * reads lv_display_get_horizontal_resolution() reflows on its own; touch is mapped by LVGL.
 * Must be called on the UI service task (purr_ui_service_run). Returns -1 for any other angle. */
int purr_lvgl_set_rotation(int degrees);

/* The hardware keyboard as an LVGL keypad: what is typed reaches whichever widget has focus in the
 * input device's group (lv_indev_set_group). Enter, backspace, escape and tab are mapped to LVGL's
 * keys. NULL before purr_lvgl_init() has run. */
struct _lv_indev_t;
struct _lv_indev_t *purr_lvgl_keypad(void);

/* The default key hook of a board with a gamepad (purr_tp_nav.c): set by purr_lvgl_init() on Playtime; a shell replaces it with its own. */
int purr_dpad_key(uint32_t key);

/* Called by the UI task before each LVGL pass: checks that the pointer and keypad devices still hold their own read callbacks and, if one was overwritten
 * (a bug seen only in the emulator so far, plan7.md), logs what it found, repairs it and counts it. Returns how many times that has happened. */
int purr_lvgl_guard(void);
int purr_lvgl_get_rotation(void);

/* A swipe up from the bottom edge of the screen (a press in the bottom 18 px that moves up 36 px) calls this on the UI task and cancels the touch, so
 * what is underneath is not tapped. NULL turns it off. */
void purr_lvgl_set_edge_swipe(void (*cb)(void));

/* A hook that sees every key before it is delivered (an LVGL key code, or the ASCII character). Return 1 to consume it. NULL turns it off. UI task. */
void purr_lvgl_set_key_hook(int (*cb)(uint32_t key));

/* The key a device with no Menu button on screen sends for it (the gamepad's X). Home is LVGL's LV_KEY_HOME. Both reach the key hook above. */
#define PURR_KEY_MENU 1

/* A hook for holding the trackball's button down for `ms` milliseconds (called once per hold, on the UI task). While it is set a tap is delivered as
 * Enter when the button is released, not when it is pressed, so a hold is never also a click. NULL turns it off. */
void purr_lvgl_set_trackball_hold(void (*cb)(void), int ms);

/* The trackball (the "trackball" device, see Modules/drivers/trackball_gpio_module.c) acts as arrow keys and Enter on the keypad. Rolling up or
 * down first offers the move to this handler (dy > 0 = further down; return 1 if it scrolled something) unless a text field has focus. */
void purr_lvgl_set_scroll_handler(int (*cb)(int dy));

#ifdef __cplusplus
}
#endif

#endif /* PURR_LVGL_H */
