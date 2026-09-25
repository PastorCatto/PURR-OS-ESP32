#pragma once
// purr_lv_style.h — the one place a claw-loaded LVGL package can ask for
// a REAL color applied to one of its own widgets, without ever touching
// lv_color_t itself. Real code, NOT a claw package (unlike launcher_
// lvgl.c/systemui_lvgl.c) — same reason purr_icons.c/purr_lv_win.h stay
// real: lv_color_t's actual memory layout depends on CONFIG_LV_COLOR_
// DEPTH (a 16-bit RGB565 bitfield struct on this build, but the exact
// bit packing — and whether CONFIG_LV_COLOR_16_SWAP flips it — is not
// something a claw object should ever have to get right by hand), so no
// function here ever takes or returns one. Every function takes only an
// already-established-safe `lv_obj_t *` (an opaque pointer every claw
// LVGL file already crosses the boundary with) and builds/applies the
// real lv_color_t entirely on this side, in purr_lv_style.c.
//
// A plain declaration header, not header-only/static-inline like purr_
// lv_win.h — purrstrap.py's _extract_public_functions() (which is what
// makes a function callable from claw code at all — see that file's own
// _CLAW_IMPORT_HEADERS comment) scans for ordinary `type name(args);`
// prototypes; a `static inline` definition wouldn't match, and its
// address wouldn't even be stable across translation units the way a
// real external symbol's is. Same declaration/definition split as purr_
// icons.h/.c.
//
// Colors below are lifted straight from archive/ui_backends_v1/modules/
// cardstack/cardstack_ui.c's own notification panel (build_status_
// panel()) — launcher_lvgl.c's own notification shade and app-drawer
// sheet pull that panel's actual look, not just its content logic, by
// direct request ("the notification UI is awful, just pull the known
// good one... make it more android-like").

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Dark grey, fully opaque — the launcher's app-drawer sheet background
// (source/apps/system/launcher/launcher_lvgl.c's own s_sheet).
void purr_lv_style_tray(lv_obj_t *obj);

// Pure black, fully opaque — the notification shade's own background,
// matching cardstack_ui.c's build_status_panel() (lv_color_black())
// exactly.
void purr_lv_style_shade_bg(lv_obj_t *obj);

// Dim grey text (0xA0,0xA0,0xA0) — panel title labels on a dark
// background, same as cardstack_ui.c's own s_status_title_lbl.
void purr_lv_style_dim_text(lv_obj_t *obj);

// White text — real content rows on a dark background (plain
// lv_color_white(), same as cardstack_ui.c's own refresh_status_notif_
// box() row labels).
void purr_lv_style_row_text(lv_obj_t *obj);

// Grey rounded pill (0x80,0x80,0x80, radius 3) — a drag-handle visual
// affordance, same look as cardstack_ui.c's own s_status_handle. Purely
// decorative here (this codebase's shade/drawer open and close as
// discrete gesture-driven state flips, not a live-tracked drag — see
// launcher_lvgl.c's own top comment on that scoping), but the same
// "there's something to grab here" cue Android's own sheets use.
void purr_lv_style_handle(lv_obj_t *obj);

// A visible 1px border in a mid-grey that reads on both the wallpaper
// and the dark tray/shade backgrounds — the bordered-card look requested
// ("add borders... make it more android-like").
void purr_lv_style_border(lv_obj_t *obj);

// A lighter-than-black grey, rounded-corner card background — one real
// rectangle per notification (launcher_lvgl.c's own s_shade_cards[]),
// same "rectangles per notification" look cardstack_ui.c/systemui_ios.c
// (Mochi's own backend) uses for its notification/Recents cards, by
// direct request ("make the notifications more like mochi where its
// rectangles per notification"). Distinct from purr_lv_style_shade_bg()
// (pure black, the panel BEHIND the cards) specifically so a card reads
// as its own surface sitting on that panel, not just more black-on-black.
void purr_lv_style_card(lv_obj_t *obj);

// Pure white, fully opaque — the Recents page's own bottom name+close
// strip on each running-app square (launcher_lvgl.c), by direct request
// ("whole strip in white"). Distinct from every other background here
// (all dark/grey) — this one needs BLACK text on top of it, not the
// white/dim text the rest of this header hands out for dark surfaces;
// see purr_lv_style_strip_text() just below.
void purr_lv_style_white_bg(lv_obj_t *obj);

// Black text — readable on purr_lv_style_white_bg()'s own white strip,
// the one surface in this codebase that isn't dark.
void purr_lv_style_strip_text(lv_obj_t *obj);

#ifdef __cplusplus
}
#endif
