#pragma once
// purr_uiconf_render_winapi.h — dispatches a `.pui` screen through
// purr_win_*() ONLY (never raw LVGL, matching that header's own contract)
// — see purr_uiconf_core.h for the format this interprets.
//
// FLAGGED, per the approved plan's own framing: no LVGL-backed
// catcall_ui_t implementation was found compiled into any currently-
// buildable device target (every device with `ui="none"` uses the
// framebuffer backend instead — see purr_uiconf_render_fb.h). This
// backend is real, forward-looking infrastructure for a future device
// that DOES register one — purr_win_*() itself already degrades
// gracefully to a working (if less pretty) fallback when no UI backend
// is registered (see purr_win_menu()'s own fallback-to-list, and
// purr_win.h's _UI_CALL/_UI_VOID macros returning a safe default
// otherwise) — but it stays genuinely unverified on real hardware until
// that device exists.
//
// Unlike the framebuffer backend, there is no manual row-diffing or
// invert-highlight here — a native widget manager owns its own redraw and
// highlight behavior; this backend's job is just to describe the screen's
// widgets to it once per full draw, and nudge its selection index on a
// nav event afterward.
#include <stdint.h>
#include <stdbool.h>
#include "purr_uiconf_core.h"
#include "../../kernel/catcalls/purr_win.h"

#define PUI_WINAPI_MAX_ROWS    24   // list rows this backend flattens per screen — same order-of-magnitude LIST_MAX_ITEMS/purr_win_menu_set_sections()'s own `flat[48]` already accept
#define PUI_WINAPI_ROW_LEN     40
#define PUI_WINAPI_MAX_BUTTONS 8    // top-level `button` widgets per screen — generous; nothing real needs more than a couple yet

typedef struct {
    purr_win_t win;
    purr_wid_t menu;             // 0 if this screen has no top-level list widget
    int        menu_node;        // the uiconf LIST widget node purr_wid_t `menu` corresponds to, for dispatch
    int        row_count;
    // purr_win_menu_set_sections()/list_set_items() may defer their own
    // rebuild to the backend's next render tick (see purr_win.h's own
    // comment on list_set_items_icon for why) — the item text arrays must
    // stay valid until then, so they're owned here, not on a caller's
    // stack.
    char        row_bufs[PUI_WINAPI_MAX_ROWS][PUI_WINAPI_ROW_LEN];
    const char *row_ptrs[PUI_WINAPI_MAX_ROWS];

    // Every top-level `button` widget's own purr_wid_t <-> uiconf node,
    // so a fired PURR_EVENT_CLICKED can be traced back to which node's
    // "on select" handler to look up — see purr_uiconf_render_winapi_
    // node_for_wid() below. A native widget manager dispatches by widget
    // handle, not by the flat selection index the framebuffer backend
    // needs (it owns its own focus/highlight entirely; nothing here
    // tracks a "selected index" the way purr_uiconf_render_fb_t does).
    purr_wid_t button_wids[PUI_WINAPI_MAX_BUTTONS];
    int        button_nodes[PUI_WINAPI_MAX_BUTTONS];
    int        button_count;
} purr_uiconf_render_winapi_t;

// Creates the one purr_win_t this screen's whole session reuses (cleared
// and rebuilt on every purr_uiconf_render_winapi_draw() call, never
// destroyed/recreated per draw — same window-reuse shape login_ui's own
// render backends already use). Returns false if purr_win_create() fails
// (e.g. no UI backend registered AND the device has no fallback either —
// see purr_win_create()'s own contract).
bool purr_uiconf_render_winapi_init(purr_uiconf_render_winapi_t *r, const char *title);
void purr_uiconf_render_winapi_deinit(purr_uiconf_render_winapi_t *r);

// Full rebuild of the window's contents from state->screen's current root
// SCREEN node: purr_win_clear() first, then one label per top-level
// `text` widget, one button per top-level `button` widget, and — if the
// screen has a top-level `list` widget — one purr_win_menu() with a
// single unheaded section holding its rows (source-backed or static
// `item{}` children, same resolution purr_uiconf_sources.h gives the fb
// backend). Section/`enabled_if` nesting is out of first-pass scope here
// too, same as the fb backend's own header comment.
//
// `cb`/`user` (either may be NULL to skip wiring — a screen with nothing
// interactive, or a caller not ready to handle events yet) are attached
// to EVERY interactive widget this creates (the menu's own
// purr_win_menu_on_select(), each button's purr_win_button()) — the SAME
// callback for all of them, same shape purr_win.h's own purr_win_cb_t
// already expects. The caller's own callback reads the fired `wid`,
// resolves it back to a uiconf node via purr_uiconf_render_winapi_
// node_for_wid() below, and dispatches through purr_uiconf_find_handler()
// itself — action dispatch is deliberately not this backend's job either,
// same "config-driven shell, not a scripting engine" boundary
// purr_uiconf_core.h's own comment draws.
void purr_uiconf_render_winapi_draw(purr_uiconf_render_winapi_t *r, const purr_uiconf_state_t *state,
                                     purr_win_cb_t cb, void *user);

// Moves the list selection by `delta` (wrapping) and pushes it straight
// to the native widget via purr_win_list_set_selected() — no redraw
// needed, the widget manager owns that. No-op (returns the unchanged
// index) if this screen has no list widget or it's empty. Returns the new
// selected index.
int purr_uiconf_render_winapi_move_selection(purr_uiconf_render_winapi_t *r, purr_uiconf_state_t *state, int delta);

// Same "$item" resolution as purr_uiconf_render_fb_selected_target() —
// see that function's own comment.
bool purr_uiconf_render_winapi_selected_target(const purr_uiconf_render_winapi_t *r, const purr_uiconf_state_t *state,
                                                char *out, size_t out_sz);

// Resolves a fired widget handle (from the `cb` passed to
// purr_uiconf_render_winapi_draw() above) back to its uiconf node: the
// list widget's own node if `wid` is this screen's menu, or a button
// widget's node if `wid` matches one of those. Returns PUI_NONE if `wid`
// doesn't match anything this backend created (e.g. a plain label, which
// is never interactive and never gets a callback wired).
int purr_uiconf_render_winapi_node_for_wid(const purr_uiconf_render_winapi_t *r, purr_wid_t wid);
