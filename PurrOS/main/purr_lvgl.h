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

#ifdef __cplusplus
}
#endif

#endif /* PURR_LVGL_H */
