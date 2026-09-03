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
#include "../app_manager/app_manager.h"

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

// ── Desktop icons ────────────────────────────────────────────────────────────
// Basic app shortcuts drawn directly on the root window. Tapping/clicking one
// launches that app via app_manager. Reuses app_manager's existing registry —
// no separate icon list to keep in sync.
#define DESKTOP_ICON_W      48
#define DESKTOP_ICON_H      48
#define DESKTOP_ICON_GAP    8
#define DESKTOP_ICON_COLS   5
#define DESKTOP_ICON_LEFT   8

// ── Status bar ───────────────────────────────────────────────────────────────
// Single strip across the top of the desktop: free RAM, WiFi, LoRa, battery.
// MiniWin-only — the mobile_ui kernel has its own separate status bar.
#define STATUS_BAR_H        14
#define DESKTOP_ICON_TOP    (STATUS_BAR_H + 6)

static void draw_status_bar(const mw_gl_draw_info_t *draw_info)
{
    int w = mw_hal_lcd_get_display_width();
    char buf[24];

    mw_gl_set_solid_fill_colour(MW_HAL_LCD_BLACK);
    mw_gl_clear_pattern();
    mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_fill(MW_GL_FILL);
    mw_gl_rectangle(draw_info, 0, 0, w, STATUS_BAR_H);

    snprintf(buf, sizeof(buf), "RAM %uK", (unsigned)(purr_kernel_free_ram() / 1024));
    mw_gl_set_solid_fill_colour(MW_HAL_LCD_WHITE);
    mw_gl_string(draw_info, 2, 3, buf);

    // Notification count — see purr_kernel_notify() in purr_kernel.h. Same
    // kernel-level ring buffer Cardstack already renders; this is the
    // icon-grid MiniWin desktop's equivalent (no popup UI yet, just a
    // visible count so it isn't entirely invisible on this backend).
    int notif_n = purr_kernel_notify_count();
    if (notif_n > 0) {
        snprintf(buf, sizeof(buf), "Notif %d", notif_n);
        mw_gl_set_solid_fill_colour(MW_HAL_LCD_YELLOW);
        mw_gl_string(draw_info, w - 185, 3, buf);
    }

    bool wifi = purr_kernel_wifi_connected();
    mw_gl_set_solid_fill_colour(wifi ? MW_HAL_LCD_GREEN : MW_HAL_LCD_RED);
    mw_gl_string(draw_info, w - 132, 3, "WiFi");

    bool lora = purr_kernel_lora_available();
    mw_gl_set_solid_fill_colour(lora ? MW_HAL_LCD_GREEN : MW_HAL_LCD_RED);
    mw_gl_string(draw_info, w - 92, 3, "LoRa");

    int batt = purr_kernel_battery_percent();
    if (batt >= 0) {
        snprintf(buf, sizeof(buf), "Bat %d%%", batt);
    } else {
        snprintf(buf, sizeof(buf), "Bat --");
    }
    mw_gl_set_solid_fill_colour(MW_HAL_LCD_WHITE);
    mw_gl_string(draw_info, w - 52, 3, buf);
}

void mw_user_root_paint_function(const mw_gl_draw_info_t *draw_info)
{
    ESP_LOGI(TAG, "DBG root_paint_function called, clip=(%d,%d,%d,%d)",
             draw_info->clip_rect.x, draw_info->clip_rect.y,
             draw_info->clip_rect.width, draw_info->clip_rect.height);
    // Fill the desktop background. MiniWin does not clear the root window
    // automatically — without this the display shows stale content.
    mw_gl_set_solid_fill_colour(MW_HAL_LCD_GREY5);
    mw_gl_clear_pattern();
    mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_fill(MW_GL_FILL);
    mw_gl_rectangle(draw_info, 0, 0,
                    mw_hal_lcd_get_display_width(),
                    mw_hal_lcd_get_display_height());

    draw_status_bar(draw_info);

    if (!purr_kernel_get_module("app_manager")) return;

    int count = app_manager_count();
    for (int i = 0; i < count; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (!app) continue;

        int col = i % DESKTOP_ICON_COLS;
        int row = i / DESKTOP_ICON_COLS;
        int x = DESKTOP_ICON_LEFT + col * (DESKTOP_ICON_W + DESKTOP_ICON_GAP);
        int y = DESKTOP_ICON_TOP  + row * (DESKTOP_ICON_H + DESKTOP_ICON_GAP + 12);

        mw_gl_set_solid_fill_colour(MW_HAL_LCD_GREY3);
        mw_gl_set_border(MW_GL_BORDER_ON);
        mw_gl_set_fill(MW_GL_FILL);
        mw_gl_rectangle(draw_info, x, y, DESKTOP_ICON_W, DESKTOP_ICON_H);

        mw_gl_string(draw_info, x, y + DESKTOP_ICON_H + 1, app->name);
    }
}

// Forward declaration — app_manager may not be loaded; guard with get_module.
extern void app_manager_open_launcher(void);

void mw_user_root_message_function(const mw_message_t *message)
{
    if (!message || message->message_id != MW_TOUCH_DOWN_MESSAGE) return;
    if (!purr_kernel_get_module("app_manager")) return;

    int16_t tx = (int16_t)(message->message_data >> 16);
    int16_t ty = (int16_t)(message->message_data & 0xFFFF);

    int count = app_manager_count();
    for (int i = 0; i < count; i++) {
        int col = i % DESKTOP_ICON_COLS;
        int row = i / DESKTOP_ICON_COLS;
        int x = DESKTOP_ICON_LEFT + col * (DESKTOP_ICON_W + DESKTOP_ICON_GAP);
        int y = DESKTOP_ICON_TOP  + row * (DESKTOP_ICON_H + DESKTOP_ICON_GAP + 12);

        if (tx >= x && tx < x + DESKTOP_ICON_W && ty >= y && ty < y + DESKTOP_ICON_H) {
            ESP_LOGI(TAG, "desktop icon %d tapped — launching", i);
            app_manager_launch_idx(i);
            return;
        }
    }
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

    // Initialise HAL subsystems
    mw_hal_non_vol_init();
    mw_hal_timer_init();
    mw_hal_lcd_init();
    mw_hal_touch_init();

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
#endif

    // MiniWin message pump
    TickType_t last_status_redraw = xTaskGetTickCount();
#ifndef CONFIG_PURR_MINIWIN_DESKTOP_WINCE
    mw_util_rect_t status_rect = { 0, 0, (int16_t)disp_w, STATUS_BAR_H };
#endif
#ifdef CONFIG_PURR_MINIWIN_DESKTOP_WINCE
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

        // Refresh the status bar (RAM/WiFi/LoRa/battery, or the WinCE
        // taskbar's RAM/battery corner) once a second — these change
        // slowly, no need to redraw on every tick.
        TickType_t now = xTaskGetTickCount();
        if ((now - last_status_redraw) >= pdMS_TO_TICKS(1000)) {
            last_status_redraw = now;
            purr_kernel_ui_breadcrumb("status_repaint");
#ifdef CONFIG_PURR_MINIWIN_DESKTOP_WINCE
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
#else
            mw_paint_window_client_rect(MW_ROOT_WINDOW_HANDLE, &status_rect);
#endif
        }
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
