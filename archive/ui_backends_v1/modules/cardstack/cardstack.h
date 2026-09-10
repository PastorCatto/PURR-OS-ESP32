#pragma once
// cardstack.h — Cardstack UI public API
//
// Rabbit R1-inspired vertical card stack: one full-screen card per app,
// snap-scrolled with touch or trackball. Home card (clock + wallpaper +
// notifications) is first; an end-of-stack Task Manager card is last.
// Status bar hides above the screen until dragged down (peek), and drags
// further into a full notification center.

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Height of the persistent status-bar strip at the top of the screen —
// shared between cardstack_ui.c (which builds the strip) and cardstack_win.c
// (which must keep app windows, and their close buttons, entirely below it).
#define CARDSTACK_STATUS_PEEK_H 22

int      cardstack_hal_init(void);
uint16_t cardstack_hal_width(void);
uint16_t cardstack_hal_height(void);

// Polls the trackball's accumulated vertical motion since the last call.
// Returns false if there's been no pointer motion. Used to step the card
// stack up/down a page at a time — not registered as an LVGL indev.
bool cardstack_hal_poll_trackball(int16_t *out_dy);

// Builds the card stack: home card, one card per registered app, and the
// end-of-stack Task Manager card. Safe to call once, after the HAL and
// app_manager are both up.
void cardstack_ui_init(void);

// Per-tick housekeeping: refreshes the clock, notification list, and the
// Task Manager's live app list. Call periodically (every ~200ms is plenty).
void cardstack_ui_tick(void);

// Drains the trackball every call — must be called on (close to) every
// main-loop tick, not at cardstack_ui_tick()'s slower cadence. See
// cardstack_ui.c for why.
void cardstack_ui_poll_trackball(void);

// Called by cardstack_win.c's close button handler whenever a hosted app
// window is hidden, so the stack can clear the dim overlay it raised when
// the window was opened from a card tap (see app_card_open_cb). Safe to
// call even if no overlay is currently shown.
void cardstack_ui_on_window_closed(void);

#ifdef __cplusplus
}
#endif
