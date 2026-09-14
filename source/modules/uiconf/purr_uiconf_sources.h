#pragma once
// purr_uiconf_sources.h — list/source resolution shared by EVERY render
// backend (purr_uiconf_render_fb.c today, purr_uiconf_render_winapi.c).
// Split out specifically so the two backends can never quietly diverge on
// what a `source = "app_list"` list widget's Nth row actually is — this
// was originally hand-rolled inside purr_uiconf_render_fb.c alone, then
// pulled out here the moment a second backend needed the identical logic.
//
// Not part of purr_uiconf_core.c itself — see that file's own top comment
// on why app_manager.h/user_mgr.h/pairing.h resolution is deliberately
// renderer-side, not core-side (core stays a small dependency for any
// future backend that only ever needs scalar `bind` values, never a
// list).
#include <stdbool.h>
#include <stddef.h>
#include "purr_uiconf_core.h"

// Index-identical to catstrap/uiconf.py's own SOURCES table — see that
// file's top comment for the reverse pointer back here. Returns -1 for an
// unknown name (should never happen post-validation).
int purr_uiconf_source_id(const char *name);
int purr_uiconf_source_count(int source_id);
bool purr_uiconf_source_label(int source_id, int idx, char *out, size_t sz);
// The "$item" resolution for an `on activate { target = "$item" }`
// handler — each source's own natural key, formatted as a plain string
// the caller's action-dispatch glue can use directly. See
// purr_uiconf_sources.c's own comment on each source's exact shape
// (app_manager's index, a username, or a colon-hex MAC the caller must
// parse back into bytes).
bool purr_uiconf_source_target(int source_id, int idx, char *out, size_t sz);

// Finds the first top-level (direct child of the screen root) `list`-kind
// widget, if any — same "first list widget found" rule
// purr_uiconf_state_t's own selected_index applies to (see
// purr_uiconf_core.h). SECTION nesting is skipped, same first-pass scope
// as everything else here.
int purr_uiconf_find_first_list_widget(const purr_uiconf_screen_t *s);

// Item count/label/target for ONE list widget node — transparently
// source-backed (reads `source` and dispatches to the functions above) or
// a static set of `item{...}` children (reads each one's own `label`/
// `target` attrs), whichever the widget actually has.
int purr_uiconf_list_item_count(const purr_uiconf_screen_t *s, int list_node);
bool purr_uiconf_list_item_label(const purr_uiconf_screen_t *s, int list_node, int idx,
                                  char *out, size_t sz);
bool purr_uiconf_list_item_target(const purr_uiconf_screen_t *s, int list_node, int idx,
                                   char *out, size_t sz);

// ── Unified selection model ──────────────────────────────────────────
// A screen's ONE selectable sequence, in document order, spans every
// top-level `list` widget's own rows AND every top-level `button` widget
// (as one selectable entry each) — added once a real app (diagnostics)
// needed BOTH a scrollable live-data list AND a standalone selectable
// on-screen "< Back" button on the same screen, cycled and activated
// through the same nav_dispatch(). This is what purr_uiconf_state_t's own
// `selected_index` (purr_uiconf_core.h) actually indexes into — a plain
// `text` widget contributes nothing (never selectable), and an empty list
// widget contributes zero rows (its "Empty" placeholder isn't
// selectable). `item_index` is -1 for a button entry (the whole widget is
// the selection) or the row index within a list widget's own items.
typedef struct {
    int node;         // the LIST or BUTTON widget node this entry belongs to
    int item_index;   // row index within a LIST widget; -1 for a BUTTON
} purr_uiconf_selectable_t;

int purr_uiconf_selectable_count(const purr_uiconf_screen_t *s);
bool purr_uiconf_selectable_at(const purr_uiconf_screen_t *s, int flat_index, purr_uiconf_selectable_t *out);
