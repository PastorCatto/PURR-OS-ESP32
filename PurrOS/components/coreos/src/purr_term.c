#include "purr_term.h"

static void blank_row(purr_term_t *t, int row)
{
    for (int i = 0; i < t->cols; i++) {
        t->cell[row][i] = ' ';
    }
    t->cell[row][t->cols] = '\0';
    t->dirty[row] = 1;
}

void purr_term_clear(purr_term_t *t)
{
    for (int r = 0; r < t->rows; r++) {
        blank_row(t, r);
    }
    t->cx = 0;
    t->cy = 0;
}

void purr_term_init(purr_term_t *t, int cols, int rows)
{
    t->cols = cols < 1 ? 1 : (cols > PURR_TERM_MAX_COLS ? PURR_TERM_MAX_COLS : cols);
    t->rows = rows < 1 ? 1 : (rows > PURR_TERM_MAX_ROWS ? PURR_TERM_MAX_ROWS : rows);
    purr_term_clear(t);
}

static void scroll(purr_term_t *t)
{
    for (int r = 1; r < t->rows; r++) {
        for (int i = 0; i <= t->cols; i++) {
            t->cell[r - 1][i] = t->cell[r][i];
        }
        t->dirty[r - 1] = 1;
    }
    blank_row(t, t->rows - 1);
}

static void newline(purr_term_t *t)
{
    t->cx = 0;
    if (t->cy + 1 >= t->rows) {
        scroll(t);
    } else {
        t->cy++;
    }
}

void purr_term_putc(purr_term_t *t, char c)
{
    switch (c) {
    case '\n':
        newline(t);
        return;
    case '\r':
        t->cx = 0;
        return;
    case '\b':
        if (t->cx > 0) {
            t->cx--;
        }
        return;
    case '\t':
        do {
            purr_term_putc(t, ' ');
        } while (t->cx % 4 != 0);
        return;
    default:
        break;
    }
    if (c < 0x20 || c > 0x7E) {
        return;                                   /* not printable: ignored */
    }
    if (t->cx >= t->cols) {
        newline(t);
    }
    t->cell[t->cy][t->cx++] = c;
    t->dirty[t->cy] = 1;
}

void purr_term_puts(purr_term_t *t, const char *s)
{
    while (*s) {
        purr_term_putc(t, *s++);
    }
}

int purr_term_take_dirty(purr_term_t *t, int row)
{
    if (row < 0 || row >= t->rows || !t->dirty[row]) {
        return 0;
    }
    t->dirty[row] = 0;
    return 1;
}
