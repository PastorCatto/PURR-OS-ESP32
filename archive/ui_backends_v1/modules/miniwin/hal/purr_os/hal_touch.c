// hal_touch.c — MiniWin HAL touch backend for PURR OS
//
// Merges real touch (catcall_touch_t) with cursor-click (trackball button).
// Real touch hides the cursor; trackball click synthesizes a touch at the
// cursor position so MiniWin responds as if the user tapped there.

#include "hal/hal_touch.h"
#include "hal/hal_lcd.h"
#include "../../../../kernel/core/purr_kernel.h"
#include "../../miniwin_cursor.h"
#include "../../MiniWin/miniwin_settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Cached touch state — updated by mw_hal_touch_get_state(), consumed by get_point().
// Mirrors the 0.11 pattern: get_state() does the single destructive read+clear,
// get_point() calls get_state() and returns the cache. One I2C sequence per poll,
// no race between release detection and the next "wait for press" in calibration.
static bool     s_pressed  = false;
static uint16_t s_cached_x = 0;
static uint16_t s_cached_y = 0;

void mw_hal_touch_init(void)
{
    // Touch driver already initialised before modules load via baked-in init.
}

bool mw_hal_touch_is_recalibration_required(void)
{
    // mw_init() calls this BEFORE mw_settings_load() runs, so checking
    // mw_settings_is_calibrated() here always reads a zero-initialised
    // in-RAM struct and returns true — forcing mw_settings_set_to_defaults()
    // + mw_settings_save() to clobber the saved NVS calibration on every
    // boot. mw_init()'s own is_calibrated()/is_initialised() check, which
    // runs after the real mw_settings_load(), is the correct place for
    // that decision — we have no separate hardware-level reason to force
    // recalibration, so always defer to it.
    return false;
}

mw_hal_touch_state_t mw_hal_touch_get_state(void)
{
    // MiniWin's calibration routine (miniwin_touch.c) polls this in a tight
    // `while (...) {}` busy-wait with no delay of its own. Without yielding
    // here, that starves the idle task on this core long enough to trip the
    // ESP-IDF idle watchdog and panic-reboot mid-calibration — observed as
    // calibration silently resetting after a couple of taps.
    vTaskDelay(1);

    const catcall_touch_t *touch = purr_kernel_touch();
    s_pressed = false;

    if (touch) {
        uint16_t tx = 0, ty = 0;
        // catcall_touch_t.read_point() is contractually screen-pixel space
        // already (docs/02_Catcalls.md — "no raw digitiser values"), so it's
        // used as-is. MiniWin's vendored 3-point affine calibration
        // (calibrate.c) learns whatever mapping/orientation this panel
        // actually has from taps in this same coordinate space — it doesn't
        // need or want a pre-scale applied here.
        //
        // Breadcrumbed separately from the generic "process_message" step
        // (see purr_kernel_ui_breadcrumb()'s doc comment) — GT911 shares its
        // I2C bus with BBQ20 (confirmed recurring "I2C bus ... already
        // acquired" warning every boot this session) and this I2C read runs
        // on every single miniwin_task() iteration, making it the prime
        // suspect for a live "UI TASK UNRESPONSIVE @ process_message" hang
        // that only appeared once Meshtastic started adding real CPU/bus
        // load — narrowing it to specifically here (vs. elsewhere in
        // mw_process_message()) on the next occurrence.
        purr_kernel_ui_breadcrumb("process_message/touch_read_point");
        bool got = touch->read_point(&tx, &ty);
        purr_kernel_ui_breadcrumb("process_message/touch_read_point_done");
        if (got) {
            s_cached_x = tx;
            s_cached_y = ty;
            s_pressed  = true;
        }
    }

    if (!s_pressed && miniwin_cursor_pressed()) {
        s_cached_x = miniwin_cursor_x();
        s_cached_y = miniwin_cursor_y();
        s_pressed  = true;
    }

    return s_pressed ? MW_HAL_TOUCH_STATE_DOWN : MW_HAL_TOUCH_STATE_UP;
}

bool mw_hal_touch_get_point(uint16_t *x, uint16_t *y)
{
    if (mw_hal_touch_get_state() == MW_HAL_TOUCH_STATE_UP) return false;
    *x = s_cached_x;
    *y = s_cached_y;
    return true;
}
