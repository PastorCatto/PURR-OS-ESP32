#pragma once
// purr_uiconf_render_fb.h — direct catcall_display_t drawing backend for
// the uiconf core (purr_uiconf_core.h). Generalizes the batched-into-one-
// push_pixels() text/list-row primitives epaper_ui.c hand-rolled and
// hardware-verified on the Waveshare154 — same shapes, same reasoning,
// just parameterized over a loaded .puib screen instead of hand-coded
// screens. See that file's own top comment for the two real, hardware-
// found bugs these primitives already fix (per-glyph push_pixels() being
// ~58s slow; per-character x-offset missing in row stamping).
//
// This is the backend that matters for tdeck/tdeck_plus/tab5/waveshare154
// today (all either ui="none" or, for waveshare154, using a plain
// catcall_display_t with no catcall_ui_t) — see the approved plan's own
// "Where the parser/interpreter live" section. A future
// purr_uiconf_render_winapi.c handles the purr_win_*()-registered case
// separately; nothing here assumes or requires one.
#include <stdint.h>
#include <stdbool.h>
#include "purr_uiconf_core.h"
#include "../../kernel/catcalls/catcall_display.h"

typedef struct {
    const catcall_display_t *disp;
    int panel_w, panel_h;
    // Growable scratch line/row buffer, sized to the real panel at first
    // use (realloc'd larger if a future call ever needs more) — NOT a
    // fixed 200x200-sized static buffer like epaper_ui.c's own
    // draw_text()/draw_list_row(), because this backend is generic across
    // whatever panel size the linking device actually has, not just the
    // Waveshare154's own known-fixed 200x200 panel.
    uint16_t *scratch;
    size_t    scratch_cap;   // capacity in PIXELS, not bytes
} purr_uiconf_render_fb_t;

// Binds a display catcall (already init'd/registered elsewhere — this
// backend never calls disp->init()/deinit() itself, same "caller owns the
// catcall lifecycle" contract every other UI module in this tree follows)
// and reads its real panel size via get_info(). Returns false if disp is
// NULL.
bool purr_uiconf_render_fb_init(purr_uiconf_render_fb_t *r, const catcall_display_t *disp);

// Frees the scratch buffer. Never actually called today (same "every
// module keeps a deinit either way" contract this whole tree follows) —
// kept for symmetry and for whichever future caller actually tears one of
// these down (e.g. a screen that owns more than one renderer instance).
void purr_uiconf_render_fb_deinit(purr_uiconf_render_fb_t *r);

// Full-screen redraw of state->screen's current root SCREEN node: title
// (if any `title` attr on the screen), then every top-level `widget`
// child in document order. Used on screen entry (purr_uiconf_open()) —
// same "only on ENTER, not every cycle" discipline draw_app_list() itself
// documents, because a cycle press only ever needs to touch the two rows
// that actually changed (see purr_uiconf_render_fb_move_selection()
// below).
void purr_uiconf_render_fb_draw(purr_uiconf_render_fb_t *r, const purr_uiconf_state_t *state);

// Moves the selection within the screen's ONE unified selectable sequence
// (every top-level `list` widget's own rows plus every top-level `button`
// widget, in document order — see purr_uiconf_sources.h's own
// purr_uiconf_selectable_t) by `delta` (wrapping), redrawing ONLY the two
// affected rows via a single push_pixels() each — the exact optimization
// draw_list_row() proved out on real Waveshare154 hardware (a full
// draw_app_list() on every cycle press felt like a full refresh; two-row
// diffing didn't). No-op if the screen has nothing selectable. Returns
// the new selected index (unchanged if no-op).
int purr_uiconf_render_fb_move_selection(purr_uiconf_render_fb_t *r, purr_uiconf_state_t *state, int delta);

// Resolves the "$item" target placeholder for the currently-selected
// entry, IF it's a list row: for a `source`-backed list, this is the
// source's own natural key for that row (app_manager's app index,
// user_mgr's username, ...) formatted as a string; for a static
// `item{...}` list, it's that item's own `target` attr if it has one,
// else its `label`. Returns false if there is no current selection, OR
// the current selection is a standalone button (which has no "$item" —
// use purr_uiconf_render_fb_selected_node() below for that case instead).
// Callers combine this with purr_uiconf_find_handler() to resolve an
// `on activate { target = "$item" }` handler's real target before
// dispatching the action themselves (see purr_uiconf_core.h's own comment
// on why action dispatch is deliberately not this core's job).
bool purr_uiconf_render_fb_selected_target(const purr_uiconf_render_fb_t *r, const purr_uiconf_state_t *state,
                                            char *out, size_t out_sz);

// Raw access to the currently-selected entry's own widget node (and, for
// a list row, which row within it — -1 for a standalone button, matching
// purr_uiconf_selectable_t.item_index exactly). This is what a caller
// dispatching a NAV_SELECT on a plain top-level `button` widget (e.g. an
// on-screen "< Back" that has its own `on select { action = exit_app }`
// handler, not a list's `on activate`) needs: call
// purr_uiconf_find_handler(&state->screen, *out_node, "select", ...) on
// the node this returns. Returns false if there is no current selection.
bool purr_uiconf_render_fb_selected_node(const purr_uiconf_render_fb_t *r, const purr_uiconf_state_t *state,
                                          int *out_node, int *out_item_index);
