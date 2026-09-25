#pragma once
// purr_icon_lib.h — general-purpose icon library for the barebones MiniWin
// desktop (and anything else native-MiniWin that wants a real icon).
//
// Real, full-color 32x32 RGB888 bitmaps (mw_gl_colour_bitmap()'s own format
// — see MiniWin/gl/gl.h), NOT MiniWin's native 1-bit monochrome bitmap format
// (MiniWin/bitmaps/*.c) — this whole library is our own addition, living
// outside the vendored MiniWin/ tree entirely (module.pcat's own "MiniWin
// source is upstream-clean" policy stays intact).
//
// Source: mRB0/many-windows-3.1-icons-in-png-format (GitHub, PNG-format
// extractions of real Windows 3.1 icon resources), converted by compositing
// each PNG's own alpha channel onto solid white (mw_gl_colour_bitmap() has no
// alpha/transparency support, unlike its monochrome sibling) and flattening to
// raw row-major RGB888 bytes — see each icon_<name>.c file's own top comment
// for which source PNG it came from.
//
// This is a plain library indexed by NAME, not by app — nothing here knows
// what app_manager is. Whatever wants an icon (the App Manager grid today,
// anything else later) picks whichever name fits and calls purr_miniwin_icon_get()
// itself; the app<->icon-name mapping lives at that call site, not here.
#include <stdint.h>
#include <stdbool.h>

extern const uint16_t purr_icon_calculator_w, purr_icon_calculator_h;
extern const uint8_t  purr_icon_calculator_rgb[];
extern const uint16_t purr_icon_cardfile_w, purr_icon_cardfile_h;
extern const uint8_t  purr_icon_cardfile_rgb[];
extern const uint16_t purr_icon_clock_w, purr_icon_clock_h;
extern const uint8_t  purr_icon_clock_rgb[];
extern const uint16_t purr_icon_control_panel_w, purr_icon_control_panel_h;
extern const uint8_t  purr_icon_control_panel_rgb[];
extern const uint16_t purr_icon_file_manager_w, purr_icon_file_manager_h;
extern const uint8_t  purr_icon_file_manager_rgb[];
extern const uint16_t purr_icon_help_w, purr_icon_help_h;
extern const uint8_t  purr_icon_help_rgb[];
extern const uint16_t purr_icon_mail_w, purr_icon_mail_h;
extern const uint8_t  purr_icon_mail_rgb[];
extern const uint16_t purr_icon_media_player_w, purr_icon_media_player_h;
extern const uint8_t  purr_icon_media_player_rgb[];
extern const uint16_t purr_icon_notepad_w, purr_icon_notepad_h;
extern const uint8_t  purr_icon_notepad_rgb[];
extern const uint16_t purr_icon_paintbrush_w, purr_icon_paintbrush_h;
extern const uint8_t  purr_icon_paintbrush_rgb[];
extern const uint16_t purr_icon_pif_editor_w, purr_icon_pif_editor_h;
extern const uint8_t  purr_icon_pif_editor_rgb[];
extern const uint16_t purr_icon_program_manager_w, purr_icon_program_manager_h;
extern const uint8_t  purr_icon_program_manager_rgb[];
extern const uint16_t purr_icon_recorder_w, purr_icon_recorder_h;
extern const uint8_t  purr_icon_recorder_rgb[];
extern const uint16_t purr_icon_registry_editor_w, purr_icon_registry_editor_h;
extern const uint8_t  purr_icon_registry_editor_rgb[];
extern const uint16_t purr_icon_solitaire_w, purr_icon_solitaire_h;
extern const uint8_t  purr_icon_solitaire_rgb[];
extern const uint16_t purr_icon_sound_w, purr_icon_sound_h;
extern const uint8_t  purr_icon_sound_rgb[];
extern const uint16_t purr_icon_terminal_w, purr_icon_terminal_h;
extern const uint8_t  purr_icon_terminal_rgb[];
extern const uint16_t purr_icon_write_w, purr_icon_write_h;
extern const uint8_t  purr_icon_write_rgb[];

// Looks up an icon by name (see the list above for what's in the library).
// Returns false (and leaves the out-params untouched) for an unknown name —
// callers decide their own fallback (e.g. "program_manager") rather than
// this header silently picking one for them.
bool purr_miniwin_icon_get(const char *icon_name, uint16_t *out_w, uint16_t *out_h, const uint8_t **out_rgb);
