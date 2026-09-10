#pragma once
// kittenui_theme.h — KittenUI (LVGL) swappable theme API
//
// A theme is a plain C struct. Fill it in, call kittenui_register_theme().
// The last registered theme wins, so themes can override each other at runtime.
// Third-party themes only need to #include this header — no other KittenUI
// internals required.
//
// Built-in themes shipped with KittenUI:
//   kittenui_theme_wce()   — WCE Classic (silver/navy, PURR OS default)
//   kittenui_theme_dark()  — Dark (VS Code dark, easy on OLED panels)

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Color palette ─────────────────────────────────────────────────────────────

typedef struct {
    lv_color_t window_bg;       // main window / screen background
    lv_color_t surface;         // widget surface (buttons, inputs)
    lv_color_t surface_alt;     // alternate surface (e.g. even table rows)
    lv_color_t titlebar;        // title bar background
    lv_color_t titlebar_text;   // title bar text
    lv_color_t selected;        // selection / focus highlight
    lv_color_t selected_text;   // text on selected background
    lv_color_t text;            // body text
    lv_color_t text_muted;      // secondary / hint text
    lv_color_t border;          // widget border
    lv_color_t border_light;    // 3D highlight edge (top-left for raised widgets)
    lv_color_t border_dark;     // 3D shadow edge (bottom-right for raised widgets)
    lv_color_t scrollbar;       // scrollbar thumb
    lv_color_t header_bg;       // app header / status bar
    lv_color_t header_text;
    lv_color_t accent;          // accent color (links, progress bars, badges)
    lv_color_t danger;          // error / destructive action
    lv_color_t success;         // confirmation / success
} kittenui_palette_t;

// ── Typography ────────────────────────────────────────────────────────────────

typedef struct {
    const lv_font_t *small;     // labels, hints  (≈10–12px)
    const lv_font_t *body;      // body text       (≈14px, default)
    const lv_font_t *heading;   // window titles   (≈16px)
    const lv_font_t *mono;      // terminal/code   (≈12px monospace)
} kittenui_fonts_t;

// ── Widget style flags ────────────────────────────────────────────────────────

typedef struct {
    bool raised_buttons;    // 3D raised look (WCE) vs flat (Dark)
    bool rounded_corners;   // corner radius on buttons/windows
    uint8_t corner_radius;  // pixels (0 = square, 4 = WCE slight)
    bool show_scrollbars;   // auto-hide or always-visible scrollbars
    bool shadow_windows;    // drop shadow on popup windows
    uint8_t padding;        // default inner padding for widgets (px)
    uint8_t item_height;    // list / menu item height (px)
} kittenui_style_flags_t;

// ── Full theme descriptor ─────────────────────────────────────────────────────

typedef struct {
    const char           *name;    // display name e.g. "WCE Classic"
    const char           *id;      // short id used in config e.g. "wce"
    kittenui_palette_t    palette;
    kittenui_fonts_t      fonts;
    kittenui_style_flags_t flags;

    // Optional: custom LVGL theme callback. If non-NULL, KittenUI calls
    // apply_fn(theme) after applying the palette — lets you do anything LVGL
    // exposes that doesn't fit the struct above.
    void (*apply_fn)(const struct kittenui_theme_t_tag *theme);
} kittenui_theme_t;

// ── Registration ──────────────────────────────────────────────────────────────

// Register a theme. The pointer must remain valid for the lifetime of the app.
// Call before kittenui_init(), or call kittenui_apply_theme() after to hot-swap.
void kittenui_register_theme(const kittenui_theme_t *theme);

// Apply (or re-apply) the currently registered theme. Call after register if
// KittenUI is already running to do a live swap.
void kittenui_apply_theme(void);

// Get the currently active theme (NULL before any theme is registered).
const kittenui_theme_t *kittenui_active_theme(void);

// ── Built-in themes ───────────────────────────────────────────────────────────

const kittenui_theme_t *kittenui_theme_wce(void);   // WCE Classic
const kittenui_theme_t *kittenui_theme_dark(void);  // Dark

#ifdef __cplusplus
}
#endif
