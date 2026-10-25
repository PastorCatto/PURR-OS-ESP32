/*
 * purr_console.h - a text console on the display.
 *
 * Wraps the terminal grid from CoreOS (purr_term) and draws the rows that changed with
 * the built-in 8x8 font. Characters go in through purr_console_put, which has the shape
 * of a shell output sink.
 */
#ifndef PURR_CONSOLE_H
#define PURR_CONSOLE_H

#include "esp_err.h"

#include "purr_board.h"
#include "purr_display.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start a console filling the display. */
esp_err_t purr_console_init(const purr_display_v2_t *d);

/* Starts the console sized from the board profile's own display.width/height, with no real
 * display behind it yet (purr_console_flush() already no-ops with no display set -- this
 * just means text accumulates safely instead of crashing on a NULL d->get_info()). For the
 * window between purr_kernel_init() and the display driver module actually loading/binding
 * (main.c's own comment on why display can't load before the filesystem mounts, unlike
 * keyboard/touch) -- purr_console_init(d) replaces this with the real thing once d exists,
 * same sizing math, so nothing but the few lines printed during that window is ever lost
 * (and those are always also on the serial log via ESP_LOGI/ESP_LOGW). */
esp_err_t purr_console_init_headless(const purr_board_t *board);

/* A shell output sink: ctx is ignored. Changes show at the next flush. */
void purr_console_put(void *ctx, char c);

void purr_console_clear(void);

/* Draw what changed since the last flush. */
void purr_console_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* PURR_CONSOLE_H */
