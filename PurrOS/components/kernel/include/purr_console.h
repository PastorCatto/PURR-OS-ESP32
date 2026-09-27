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

#include "purr_display.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start a console filling the display. */
esp_err_t purr_console_init(const purr_display_v2_t *d);

/* A shell output sink: ctx is ignored. Changes show at the next flush. */
void purr_console_put(void *ctx, char c);

void purr_console_clear(void);

/* Draw what changed since the last flush. */
void purr_console_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* PURR_CONSOLE_H */
