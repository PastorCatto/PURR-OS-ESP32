// miniwin_config.h — unified MiniWin configuration for PURR OS
//
// Display dimensions are resolved at runtime via catcall_display_t / hal_lcd,
// so this config works for all devices without per-device forks.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ── Window manager limits ────────────────────────────────────────────────────
#define MW_MAX_WINDOW_COUNT         20U
// Settings alone now builds ~30 controls (Theme, Display, Keyboard Backlight,
// WiFi list+buttons, Bluetooth list+buttons, Wallpaper list, Storage, System,
// About) — bumped from 40 with real headroom for that plus other apps'
// per-buddy/dialog windows (MeshChat, File Manager) accumulating over a
// session. Each control is a small fixed struct — negligible memory cost.
#define MW_MAX_CONTROL_COUNT        96U
#define MW_MAX_TIMER_COUNT          8U
// Widget creation posts more than one message per widget (create + paint/
// invalidate, at least) — Settings' ~30 controls, built in one synchronous
// burst on the app's own task while MiniWin's render task drains the queue
// one message at a time, exhausted the old 100-slot queue and hit
// MW_ASSERT(items_in_queue < MW_MESSAGE_QUEUE_SIZE, "Message queue full").
// Bumped with real headroom — mw_message_t is a handful of small fields, so
// even 256 slots costs only a few KB.
#define MW_MESSAGE_QUEUE_SIZE       256U

// ── Display rotation ─────────────────────────────────────────────────────────
// Override at build time with -DMW_DISPLAY_ROTATION_90 etc. if needed.
#if !defined(MW_DISPLAY_ROTATION_0)  && !defined(MW_DISPLAY_ROTATION_90) && \
    !defined(MW_DISPLAY_ROTATION_180) && !defined(MW_DISPLAY_ROTATION_270)
#define MW_DISPLAY_ROTATION_0
#endif

// Root dimensions come from the registered display catcall at runtime.
#if defined(MW_DISPLAY_ROTATION_0) || defined(MW_DISPLAY_ROTATION_180)
#define MW_ROOT_WIDTH   mw_hal_lcd_get_display_width()
#define MW_ROOT_HEIGHT  mw_hal_lcd_get_display_height()
#else
#define MW_ROOT_WIDTH   mw_hal_lcd_get_display_height()
#define MW_ROOT_HEIGHT  mw_hal_lcd_get_display_width()
#endif

// ── UI text limits ────────────────────────────────────────────────────────────
#define MW_MAX_TITLE_SIZE           14U

// ── Timing ───────────────────────────────────────────────────────────────────
#define MW_TICK_PERIOD_MS           20
#define MW_TICKS_PER_SECOND         50U
#define MW_CONTROL_DOWN_TIME        4U
#define MW_KEY_DOWN_TIME            3U
#define MW_WINDOW_MIN_MAX_EFFECT_TIME 5U
#define MW_CURSOR_PERIOD_TICKS      10U
#define MW_TOUCH_INTERVAL_TICKS     2U
#define MW_HOLD_DOWN_DELAY_TICKS    10U

// ── Fonts ────────────────────────────────────────────────────────────────────
#define MW_FONT_12_INCLUDED
#define MW_FONT_16_INCLUDED
#define MW_FONT_20_INCLUDED
#define MW_FONT_24_INCLUDED

// ── Colours ──────────────────────────────────────────────────────────────────
#define MW_TITLE_BAR_COLOUR_FOCUS       MW_HAL_LCD_BLUE
#define MW_TITLE_BAR_COLOUR_NO_FOCUS    MW_HAL_LCD_GREY5
#define MW_TITLE_BAR_COLOUR_MODAL       MW_HAL_LCD_RED
#define MW_TITLE_TEXT_COLOUR_FOCUS      MW_HAL_LCD_WHITE
#define MW_TITLE_TEXT_COLOUR_NO_FOCUS   MW_HAL_LCD_WHITE
#define MW_TITLE_TEXT_COLOUR_MODAL      MW_HAL_LCD_WHITE
#define MW_CONTROL_UP_COLOUR            MW_HAL_LCD_GREY2
#define MW_CONTROL_SEPARATOR_COLOUR     MW_HAL_LCD_GREY3
#define MW_CONTROL_DOWN_COLOUR          MW_HAL_LCD_GREY4
#define MW_CONTROL_DISABLED_COLOUR      MW_HAL_LCD_GREY5

// ── Misc ─────────────────────────────────────────────────────────────────────
#define MW_DRAG_THRESHOLD_PIXELS    2
#define MW_BUSY_TEXT                "BUSY..."
#define MW_CALIBRATE_TEXT           "Touch centre of cross"

#ifdef __cplusplus
}
#endif
