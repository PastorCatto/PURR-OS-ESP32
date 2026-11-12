/*
 * purr_ui_config.h - the Hyprland-style declarative UI config (chunk 4 of the LVGL UI plan,
 * see memory/[[project_lvgl_ui_proof_of_life]] for the full chunk list).
 *
 * A screen is one block: `screen "name" { label "x" { ... } button "y" { ... } ... }`.
 * Deliberately small -- two widget types, a flat property list each, at most one animation
 * per widget -- not a general-purpose language. Hand-rolled parser, no third-party format
 * (this project has never pulled in a JSON/YAML/TOML dependency; purr_manifest.c's flat
 * key=value parser is the closest precedent, extended here for nesting and multi-value
 * properties since a single screen needs both).
 */
#ifndef PURR_UI_CONFIG_H
#define PURR_UI_CONFIG_H

#include <stdint.h>

#include "esp_err.h"
#include "purr_cli.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_UI_MAX_WIDGETS 16
#define PURR_UI_NAME_LEN    32
#define PURR_UI_TEXT_LEN    64
#define PURR_UI_WORD_LEN    16

/* 0xFFFFFFFF marks "not set in the config, the builder uses its own default" for every
 * color field below -- a real RGB888 value never has all three bytes 0xFF and the high byte
 * 0xFF at once from a #RRGGBB literal (that would need a 4th byte), so this is unambiguous. */
#define PURR_UI_COLOR_UNSET 0xFFFFFFFFu

typedef enum {
    PURR_UI_WIDGET_LABEL,
    PURR_UI_WIDGET_BUTTON,
    PURR_UI_WIDGET_IMAGE,
    /* Launcher3-style grid (chunk F, memory/[[project_appruntime_fullstack_plan]]): not a
     * single widget like the three above -- the builder expands it at build time into one
     * button per installed app (purr_apps_scan_current_user()), laid out in a grid starting
     * at `x,y` with each cell `w,h` and `columns` per row. Visual only for now: tapping an
     * icon doesn't launch anything yet (chunk F's own scoping note -- that needs a run path
     * that doesn't need a console, which doesn't exist yet). */
    PURR_UI_WIDGET_APP_GRID,
    /* KitKat-style nav bar (chunk G): expands to exactly three fixed buttons (Back/Home/
     * Recents) spanning `w` starting at `x,y`, each `h` tall. Visual only, same reasoning as
     * app_grid: real Back needs a screen-history stack and real Recents needs a running-
     * instance list, neither of which exists yet (the minimal AppRuntime manager, chunk
     * C/D/E, runs one app synchronously to completion -- there is no "running instance" to
     * list). Wiring even Home (reload the launcher screen) from inside the button's own tap
     * callback would call back into the UI service task from itself and deadlock -- real
     * fix needed there too, not attempted here. */
    PURR_UI_WIDGET_NAVBAR,
} purr_ui_widget_type_t;

typedef struct {
    purr_ui_widget_type_t type;
    char name[PURR_UI_NAME_LEN];
    int x, y;                 /* position; 0,0 if not set. app_grid: the grid's origin. */
    int w, h;                 /* size; 0,0 means "auto" (let LVGL size it from content).
                                * app_grid: one cell's size. */
    char text[PURR_UI_TEXT_LEN];
    char asset[PURR_UI_NAME_LEN];   /* image widgets only: looked up via purr_ui_assets_find() */
    uint32_t color;           /* PURR_UI_COLOR_UNSET if not set */
    int scale;                 /* font scale multiplier; 0 means "not set" (use 1) */
    char anim_type[PURR_UI_WORD_LEN];     /* "" = no animation; else e.g. "fade_in" */
    int anim_duration_ms;
    char anim_easing[PURR_UI_WORD_LEN];   /* "linear", "ease_in", "ease_out", "ease_in_out" */
    int columns;                /* app_grid only; 0 means "not set" (the builder defaults it). */
} purr_ui_widget_t;

typedef struct {
    char name[PURR_UI_NAME_LEN];
    uint32_t background;      /* PURR_UI_COLOR_UNSET if not set (builder defaults to black) */
    purr_ui_widget_t widgets[PURR_UI_MAX_WIDGETS];
    int widget_count;
} purr_ui_screen_t;

/* Parses one screen block out of `text` (mutated in place: delimiters become '\0', so the
 * buffer must be writable and must outlive nothing -- values are copied into `out`, not
 * referenced). Returns ESP_OK, or ESP_ERR_INVALID_ARG with the problem logged (line number,
 * what was expected) on any syntax error. Unknown property names/widget types inside an
 * otherwise well-formed block are logged as warnings and skipped, not treated as fatal --
 * a config written against a newer version of this parser should still mostly render on an
 * older build, same "don't block on forward-compatibility" instinct the rest of this project
 * already applies to manifests. */
esp_err_t purr_ui_config_parse(char *text, purr_ui_screen_t *out);

/* Reads `path` from the root filesystem and parses it (purr_ui_config_parse() above).
 * ESP_ERR_NOT_FOUND if the file doesn't exist, ESP_ERR_INVALID_ARG on a parse error -- either
 * way the caller (purr_ui_load_and_build()) treats this as the critical error that falls
 * back to the text shell (purr_lvgl.h's purr_lvgl_report_critical_error()). */
esp_err_t purr_ui_config_load(const char *path, purr_ui_screen_t *out);

/* Builds real LVGL objects on the active screen from a parsed purr_ui_screen_t -- the thing
 * that makes the config actually visible. Call after purr_lvgl_init(). */
void purr_ui_build_screen(const purr_ui_screen_t *screen);

/* purr_ui_config_load() + purr_ui_build_screen() in one call, with the critical-error report
 * wired in on failure -- what main.c/purr_lvgl.c actually call in place of
 * purr_lvgl_demo_screen(). Returns the same way purr_ui_config_load() does. */
esp_err_t purr_ui_load_and_build(const char *path);

/* Writes a default /ui/shell.conf if (and only if) nothing is there yet -- same missing-only
 * convention purr_modules_setup()'s auto-plant already uses, for the same reason: never stomp
 * something a person or a netinstall already put there. Call once at boot, after the root
 * filesystem is mounted, before purr_ui_load_and_build(). */
void purr_ui_config_setup(purr_cli_t *cli);

/* Which top-level UI engine main.c boots into: the official shell (the built-in Android 2.2 one, or an installed one: purr_shell.h, plan7.md) or NAV, this file's
 * own declarative label/button/image/app_grid/navbar screen (purr_ui_load_and_build("/ui/shell.conf")). The card stack is gone. */
typedef enum {
    PURR_UI_MODE_NAV,
    PURR_UI_MODE_W10M,          /* the official shell compiled into the image */
} purr_ui_mode_t;

/* Reads /ui/mode.conf (plain text, just the mode name on its own line) -- missing, empty, or
 * unrecognized content all give PURR_UI_MODE_W10M, the official shell (so a device whose file still says "card_shell" starts it). Call after
 * purr_ui_mode_setup() has had a chance to plant the default, before deciding which engine to
 * start. */
purr_ui_mode_t purr_ui_mode_get(void);

/* Writes a default /ui/mode.conf (CONFIG_PURR_UI_DEFAULT_MODE, "android") if (and only if) nothing is there yet --
 * same missing-only convention every other *_setup() in this file already uses. Call
 * alongside purr_ui_config_setup(), same timing requirement. */
void purr_ui_mode_setup(purr_cli_t *cli);

#ifdef __cplusplus
}
#endif

#endif /* PURR_UI_CONFIG_H */
