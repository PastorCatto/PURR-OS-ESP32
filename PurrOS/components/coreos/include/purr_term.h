/*
 * purr_term.h - a text terminal's screen as a grid of characters.
 *
 * Pure state, no drawing: characters go in, and the renderer asks which rows changed.
 * Handles newline, carriage return, backspace (moves left), wrapping and scrolling.
 */
#ifndef PURR_TERM_H
#define PURR_TERM_H

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_TERM_MAX_COLS 64
#define PURR_TERM_MAX_ROWS 40

typedef struct {
    int cols, rows;
    int cx, cy;                                   /* the cursor */
    char cell[PURR_TERM_MAX_ROWS][PURR_TERM_MAX_COLS + 1];
    unsigned char dirty[PURR_TERM_MAX_ROWS];      /* row changed since the last draw */
} purr_term_t;

void purr_term_init(purr_term_t *t, int cols, int rows);   /* clamps to the maximum */
void purr_term_clear(purr_term_t *t);
void purr_term_putc(purr_term_t *t, char c);
void purr_term_puts(purr_term_t *t, const char *s);

/* True once for each changed row, then false until it changes again. */
int purr_term_take_dirty(purr_term_t *t, int row);

#ifdef __cplusplus
}
#endif

#endif /* PURR_TERM_H */
