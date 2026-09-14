// miniwin_module.c — PURR OS .purr module wrapper for MiniWin
//
// This is the kernel entry point for the MiniWin windowing system.
// The kernel calls init() after driver_manager has registered catcalls,
// so display and touch are guaranteed to be available by the time we run.
//
// Only activates when CONFIG_PURR_UI_BACKEND_MINIWIN=y (set in device sdkconfig).
// If another UI module has already claimed the catcall_ui slot, init() returns 0
// without starting MiniWin.

#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_kernel.h"
#include "MiniWin/miniwin.h"
#include "MiniWin/hal/hal_timer.h"
#include "MiniWin/hal/hal_non_vol.h"
#include "MiniWin/hal/hal_touch.h"
#include "MiniWin/hal/hal_init.h"
#include "MiniWin/hal/hal_lcd.h"
#include "MiniWin/gl/gl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"
#include <stdio.h>

extern void miniwin_win_register(void);
extern void miniwin_win_process_pending_closes(void);

#include "miniwin_cursor.h"
#include "miniwin_keyboard.h"
#include "miniwin_appmgr.h"
#include "../app_manager/app_manager.h"
#include "../user_mgr/user_mgr.h"

static const char *TAG = "miniwin";

// CONFIG_PURR_UI_WINCE_SHELL devices bake their own WinCE shell directly into
// the kernel (no .purr module wrapper) and provide mw_user_init() /
// mw_user_root_paint_function() / mw_user_root_message_function() /
// the MiniWin task themselves — defining them here too would be a duplicate
// symbol. This module still only compiles in when CONFIG_PURR_UI_BACKEND_MINIWIN
// is set, so non-WinCE devices are unaffected.
//
// CONFIG_PURR_MINIWIN_DESKTOP_WINCE opts a device into the generalized WinCE
// taskbar+start-menu desktop (miniwin_wince_desktop.c) instead of this file's
// icon-grid desktop — same duplicate-symbol concern, same guard.
#if !defined(CONFIG_PURR_UI_WINCE_SHELL) && !defined(CONFIG_PURR_MINIWIN_DESKTOP_WINCE)

// MiniWin framework callbacks — the library calls these at init and repaint time.
void mw_user_init(void) {}

// ── Barebones desktop (2026-09-13) ──────────────────────────────────────────
// Direct instruction: "barebones minwin, start with just a basic apps
// window, nothing else, no taskbar, no nothing, a basic one window, a
// basic minwin instance." This file used to also draw its own status bar
// (free RAM/WiFi/LoRa/battery strip) and a tap-to-launch desktop icon
// grid directly on the root window — real chrome of its own, competing
// with whatever single app window (source/apps/system/home/) is meant to
// be the entire UI. Both removed: the root window is now just a plain
// background fill, nothing drawn or hit-tested on it at all. See
// CoreOS/sdkconfig_tdeck_plus.overrides' own CONFIG_PURR_MINIWIN_DESKTOP_
// WINCE=n comment for the matching reason the WinCE taskbar/lock-screen
// desktop is off too — same instruction, the other desktop style.
//
// Background colour is a plain 24-bit RGB literal, not a new
// MW_HAL_LCD_* constant added to the vendored hal_lcd.h — same "our own
// addition, not a vendored-tree edit" reasoning icon_lib/ already
// follows.
#define PURR_COLOR_SUNSET_ORANGE 0xFD5E53

void mw_user_root_paint_function(const mw_gl_draw_info_t *draw_info)
{
    // Fill the desktop background. MiniWin does not clear the root window
    // automatically — without this the display shows stale content.
    mw_gl_set_solid_fill_colour(PURR_COLOR_SUNSET_ORANGE);
    mw_gl_clear_pattern();
    mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_fill(MW_GL_FILL);
    mw_gl_rectangle(draw_info, 0, 0,
                    mw_hal_lcd_get_display_width(),
                    mw_hal_lcd_get_display_height());
}

void mw_user_root_message_function(const mw_message_t *message)
{
    (void)message;   // no desktop icons to hit-test against any more — see this section's own top comment
}

#endif  // !CONFIG_PURR_UI_WINCE_SHELL && !CONFIG_PURR_MINIWIN_DESKTOP_WINCE

#if defined(CONFIG_PURR_MINIWIN_DESKTOP_WINCE) && !defined(CONFIG_PURR_UI_WINCE_SHELL)
#include "miniwin_wince_desktop.h"
#include "miniwin_lock.h"
#endif

// miniwin_task() itself is shared by both desktop styles (icon-grid and
// WinCE) — only the periodic repaint target below differs between them.
#ifndef CONFIG_PURR_UI_WINCE_SHELL

static TaskHandle_t s_task = NULL;

static void miniwin_task(void *arg)
{
    (void)arg;

    // Wait for boot.c/kernel_tdp_boot.c to finish loading every static
    // module/app AND for the boot splash's own remaining steps to run —
    // same purr_kernel_boot_ready() wait every other UI backend module
    // already does (cheetah_module.c/cardstack_module.c/tabby_module.c/
    // cupcake_module.c/mochi_module.c/nougat_module.c all have this exact
    // loop; MiniWin never did). Without it, this task — spawned from
    // miniwin_init() while purr_kernel_load_static_modules() is still
    // mid-boot — races ahead of the rest of boot on its own concurrent
    // task: HAL/mw_init()/mw_user_init() (this file's own boot-login gate
    // included) run and the first mw_paint_all() actually hits the
    // display WHILE the boot splash is still mid-progress-bar, so the
    // desktop (and an auto-login decision, if the account has no
    // password) visibly appears before the splash is done. Confirmed
    // live as exactly that symptom.
    while (!purr_kernel_boot_ready()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // Wait for a real login too. This gate was briefly removed
    // (2026-09-13) on the mistaken assumption that it was part of the
    // same "lock screen" the user wanted gone — it isn't. That was
    // CONFIG_PURR_MINIWIN_DESKTOP_WINCE (now off — see
    // CoreOS/sdkconfig_tdeck_plus.overrides), a completely separate
    // mechanism. THIS gate solves a different, still-real problem:
    // MiniWin's own keyboard/touch HAL init and message pump start
    // consuming input the moment boot_ready fires, racing the console's
    // own interactive login prompt for the exact same keyboard catcall
    // and winning — confirmed live, TWICE now (once when this gate didn't
    // exist yet, and again the moment it was removed): with it gone, the
    // console login prompt never sees a single keystroke at all. Gating
    // on a real login, not just boot completion, keeps the keyboard
    // exclusively the console's until someone is actually logged in.
    while (!user_mgr_is_logged_in()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // Initialise HAL subsystems
    mw_hal_non_vol_init();
    mw_hal_timer_init();
    mw_hal_lcd_init();
    mw_hal_touch_init();

    // Root-caused a real hang-then-crash (2026-09-13, real hardware,
    // T-Deck Plus): mw_init() itself calls mw_touch_calibrate() whenever
    // its own persisted settings report "not calibrated" — which is
    // ALWAYS true the very first time this device boots with MiniWin
    // (fresh non-vol storage). That routine is a genuinely blocking,
    // interactive 3-point calibration requiring a human to physically
    // tap the touchscreen (MiniWin/miniwin_touch.c's own
    // `while (mw_hal_touch_get_state() == MW_HAL_TOUCH_STATE_UP) {}`
    // busy-waits, no timeout) — fundamentally incompatible with an
    // unattended boot. Confirmed live via targeted instrumentation: boot
    // hangs forever at this exact call, crash_guard's own separate
    // watchdog eventually force-reboots the whole device, and the
    // MW_ASSERT("Bad window handle") crash_guard went on to report was a
    // downstream symptom of that forced recovery, not the real cause.
    //
    // Fix: seed "already calibrated" with a true IDENTITY matrix BEFORE
    // mw_init() runs, so its own is_initialised()/is_calibrated() check
    // takes the already-set branch and mw_touch_calibrate() never runs.
    // This is the CORRECT value here, not a workaround-shaped hack: GT911
    // is a capacitive controller that already reports real display-pixel
    // coordinates (confirmed: gt911.c's own quirk-driven X/Y_OUTPUT_MAX
    // matches the panel's own resolution), unlike a resistive panel that
    // genuinely needs the affine transform this matrix exists for. Lives
    // here (our own module, not vendored MiniWin/) rather than patching
    // mw_init() itself, keeping module.pcat's own "no per-device forks"
    // policy for the vendored tree intact. Flagged: if a future MiniWin
    // device ever uses a raw/resistive touch driver, this unconditional
    // skip would be wrong for it — gate on a real touch-calibration
    // capability check at that point, none exists to check yet.
    mw_settings_load();
    if (!mw_settings_is_initialised() || !mw_settings_is_calibrated()) {
        mw_settings_set_to_defaults();
        MATRIX_CAL identity = { .An = 1, .Bn = 0, .Cn = 0, .Dn = 0, .En = 1, .Fn = 0, .Divider = 1 };
        mw_settings_set_calibration_matrix(&identity);
        mw_settings_set_calibrated(true);
        mw_settings_save();
        ESP_LOGI(TAG, "seeded identity touch-calibration matrix (GT911 already reports pixel coords)");
    }

    // Initialise MiniWin window manager
    mw_init();
    int disp_w = mw_hal_lcd_get_display_width();
    int disp_h = mw_hal_lcd_get_display_height();
    ESP_LOGI(TAG, "window manager ready (%dx%d)", disp_w, disp_h);

    // Init trackball cursor overlay
    miniwin_cursor_init(disp_w, disp_h);

#ifdef CONFIG_PURR_MINIWIN_DESKTOP_WINCE
    // Stamp the idle clock at "now" right before the loop below starts
    // polling it — see miniwin_lock_init()'s own comment for why.
    miniwin_lock_init();
#endif

#ifndef CONFIG_PURR_MINIWIN_DESKTOP_WINCE
    // Desktop boots empty with app icons (drawn in mw_user_root_paint_function).
    // Launcher now opens on demand: Enter key with nothing focused, or tapping
    // the start-menu icon/desktop icon directly.
    //
    // mw_init() posts a WINDOW_CREATED message for the root window, but that
    // message's handling doesn't actually trigger a paint by itself — without
    // an explicit initial paint here the screen just stays whatever the
    // display driver's own GRAM-clear left it as (black) until SOMETHING
    // else happens to repaint the root window. Force the first paint now.
    mw_paint_window_client(MW_ROOT_WINDOW_HANDLE);

    // Auto-open the App Manager window — direct instruction: "minwin auto
    // open a App Manager window, with all the installed apps in 2 rows
    // with the icons." This IS the desktop now; `startx`/`exec home`
    // still work (app_manager_launch_by_name("home") is untouched) for
    // anyone who wants the plain uiconf/purr_win_*() app list instead,
    // but nobody has to reach for either any more just to see something
    // on screen after logging in.
    miniwin_appmgr_open();
#endif

    // MiniWin message pump
#ifdef CONFIG_PURR_MINIWIN_DESKTOP_WINCE
    TickType_t last_status_redraw = xTaskGetTickCount();
// 5s: clock/battery-icon corner toggle cadence, matched to this loop's own
// 1s status-repaint tick.
#define STATUS_ROTATE_TICKS 5
    int status_ticks = 0;
#endif

    while (1) {
        // app_manager launches each app in its own task, and that task's
        // purr_win_*() calls take purr_kernel_ui_lock() (see purr_win.h's
        // _UI_CALL/_UI_VOID macros) before touching MiniWin. This task's own
        // message pump / repaint calls MiniWin directly and must take the
        // same lock, or the two tasks can both reach the ST7789 SPI driver
        // at once — observed on hardware as
        // "assert failed: spi_device_transmit ... (ret_trans == trans_desc)"
        // followed by a full reset the moment an app launches. KittenUI's
        // task loop already does this (see kittenui_module.c); MiniWin's
        // didn't.
        purr_kernel_ui_breadcrumb("lock");
        purr_kernel_ui_lock();
        purr_kernel_ui_breadcrumb("process_message");
        // mw_process_message() dequeues and handles exactly one message per
        // call, and things like mw_paint_all()/window-open/control-create
        // don't paint synchronously — they just post a message onto that
        // same queue for a later call to actually render. One call per loop
        // tick meant a single user action posting several paint-related
        // messages back-to-back (e.g. opening the Start menu, launching an
        // app) rendered as that many separate visible partial frames one
        // tick apart — confirmed live as "takes a few redraws just to see
        // the taskbar." Draining a bounded batch per tick instead collapses
        // that into effectively one frame, without touching the vendored
        // MiniWin engine itself — bounded (not a plain while-drain) so a
        // pathologically deep queue still can't starve input polling/the
        // watchdog heartbeat below.
        for (int drained = 0; drained < 8 && mw_process_message(); drained++) {}
        // Runs any close-icon teardowns queued by MW_WINDOW_REMOVED_MESSAGE
        // this iteration — deliberately from here, not from inside
        // mw_process_message()'s own callback dispatch. See
        // miniwin_win.c's win_message_func()/miniwin_win_process_pending_
        // closes() comments for why (MiniWin's own reentrancy guard).
        purr_kernel_ui_breadcrumb("pending_closes");
        miniwin_win_process_pending_closes();
        purr_kernel_ui_breadcrumb("keyboard_poll");
        miniwin_keyboard_poll();  // drain all inputs: cursor gets pointer/click, keys → focused win
        purr_kernel_ui_breadcrumb("cursor_poll");
        miniwin_cursor_poll();    // redraw cursor on top of frame if position changed

#ifdef CONFIG_PURR_MINIWIN_DESKTOP_WINCE
        // Refresh the WinCE taskbar's RAM/battery corner once a second —
        // these change slowly, no need to redraw on every tick. No
        // equivalent for the barebones (non-WinCE) desktop any more — see
        // mw_user_root_paint_function()'s own top comment: there is no
        // status bar left to refresh.
        TickType_t now = xTaskGetTickCount();
        if ((now - last_status_redraw) >= pdMS_TO_TICKS(1000)) {
            last_status_redraw = now;
            purr_kernel_ui_breadcrumb("status_repaint");
            // Idle-lock check — same "screen timeout -> lock screen"
            // behavior Cupcake's own ck_lock_check_idle() has, driven by
            // the same portable purr_kernel_screen_timeout_min() Settings'
            // Display screen sets. Activity itself is tracked wherever
            // input is actually handled (miniwin_keyboard.c's
            // miniwin_lock_handle_key()/_other(), the lock windows' own
            // miniwin_lock_handle_touch() calls in miniwin_wince_desktop.c)
            // — not here, this is just the periodic "has enough idle time
            // passed" check.
            miniwin_lock_check_idle();
            // While locked, the overlay owns the screen and repaints
            // itself exactly once per real state change (on_lock_
            // transition()'s own paint calls) — skip the taskbar's RAM/
            // battery rotation entirely while locked instead of
            // redundantly repainting a window that's currently hidden.
            if (!miniwin_lock_is_locked()) {
                // Taskbar corner rotates RAM/battery every
                // STATUS_ROTATE_TICKS repaints (~4s at this 1s cadence).
                if (++status_ticks >= STATUS_ROTATE_TICKS) {
                    status_ticks = 0;
                    wce_desktop_toggle_status();
                }
                mw_util_rect_t wce_status_r;
                wce_status_rect(&wce_status_r);
                mw_paint_window_client_rect(wce_taskbar_handle(), &wce_status_r);
            }
        }
#endif
        purr_kernel_ui_breadcrumb("unlock");
        purr_kernel_ui_unlock();
        purr_kernel_ui_breadcrumb("idle");
        purr_kernel_ui_heartbeat();

        // taskYIELD() only hands off to an equal/higher-priority READY task —
        // with nothing else ready it just spins straight back here, taking
        // and releasing the (now real) ui_lock thousands of times a second
        // and starving any app task waiting on that same lock. KittenUI's
        // task loop already sleeps instead of spinning; match it.
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

#endif  // !CONFIG_PURR_UI_WINCE_SHELL

static int miniwin_init(void)
{
#ifndef CONFIG_PURR_UI_BACKEND_MINIWIN
    ESP_LOGI(TAG, "MiniWin built-in but not selected for this device — skipping");
    return 0;
#endif

#ifdef CONFIG_PURR_UI_WINCE_SHELL
    // WinCE shell is started directly from kernel_atdp_boot.cpp (baked in,
    // no module wrapper) — this module has nothing left to do for it.
    ESP_LOGI(TAG, "WinCE shell baked into kernel — miniwin module skipping");
    return 0;
#else

    if (purr_kernel_ui()) {
        ESP_LOGW(TAG, "UI catcall already registered — skipping MiniWin");
        return 0;
    }

    const catcall_display_t *disp  = purr_kernel_display();
    const catcall_touch_t   *touch = purr_kernel_touch();

    if (!disp) {
        ESP_LOGE(TAG, "no display catcall — miniwin cannot start");
        return -1;
    }
    if (!touch) {
        ESP_LOGW(TAG, "no touch catcall — touch input disabled");
    }

    // Register catcall_ui_t so apps can use purr_win_*() regardless of task state
    miniwin_win_register();

    // Run MiniWin message pump in its own task. Deliberately plain
    // xTaskCreate() — internal-DRAM stack, NOT MALLOC_CAP_SPIRAM like every
    // other background task in this codebase (Milkbar's send task, MSN's
    // refresh task, Settings' BT scan task). Tried moving it to PSRAM to
    // reclaim that internal-DRAM cost; confirmed live it doesn't work here:
    // touch calibration flows into mw_init() -> mw_settings_save() ->
    // nvs_open()/esp_flash_write() on THIS task's own stack, and ESP-IDF's
    // flash-write path asserts esp_task_stack_is_sane_cache_disabled() —
    // it briefly disables the flash cache (which also gates PSRAM access)
    // and requires the calling task's own stack to be entirely in internal
    // RAM at that moment. A PSRAM stack fails that assert outright
    // ("assert failed: spi_flash_disable_interrupts_caches_and_other_cpu"),
    // crashing every time calibration tries to persist. The other
    // background tasks this pattern is copied from never touch NVS/flash
    // directly from their own task context, so they never hit this.
    TaskHandle_t task = NULL;
    BaseType_t ret = xTaskCreate(miniwin_task, "miniwin", 8192, NULL, 5, &task);
    s_task = task;
    return (ret == pdPASS) ? 0 : -1;
#endif  // CONFIG_PURR_UI_WINCE_SHELL
}

static void miniwin_deinit(void)
{
#ifndef CONFIG_PURR_UI_WINCE_SHELL
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
#endif
}

// ── .purr module header ───────────────────────────────────────────────────────

PURR_MODULE_REGISTER(miniwin) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_UI,
    // Explicit: an unset load_priority is 0, which sorted this module BEFORE
    // the P1 display driver it requires — miniwin_init() then failed every
    // boot with "no display catcall" and the crash guard eventually disabled
    // the whole UI (confirmed live on tab5). P2 + type UI orders it after
    // drivers and system modules.
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "miniwin",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = CATCALL_FLAG_DISPLAY,   // touch is optional
    .init              = miniwin_init,
    .deinit            = miniwin_deinit,
};
