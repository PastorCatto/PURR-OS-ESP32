#pragma once
// miniwin_appmgr.h — the barebones desktop's one auto-opened window: every
// installed app in a 2-row icon grid, real Windows-3.1-style icons (see
// icon_lib/purr_icon_lib.h), tap to launch. Direct instruction: "minwin
// auto open a App Manager window, with all the installed apps in 2 rows
// with the icons" — this IS the desktop now, opened once right after
// login (see miniwin_module.c's own miniwin_task()), not something the
// user has to reach for via `startx`/`exec home` any more (that plumbing
// stays, this is just what runs it automatically).
//
// Deliberately a native MiniWin window built directly against
// mw_add_window()/mw_gl_colour_bitmap(), NOT through the portable
// purr_win_*() canvas API — that API's own canvas_rect()/canvas_text()
// has no bitmap primitive, and adding one would touch catcall_ui_t (every
// other UI backend's shared contract) for a feature that is, by direct
// instruction, MiniWin-specific.

// Creates (if not already open) and shows the App Manager window, built
// fresh from whatever app_manager_count()/_get() report right now. Safe
// to call more than once — a second call just re-shows/repaints the
// existing window rather than creating a duplicate.
void miniwin_appmgr_open(void);
