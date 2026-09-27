#include "purr_console.h"

#include "purr_gfx.h"
#include "purr_term.h"

#define CELL_W   8
#define ROW_H    9                       /* 8 pixel glyphs and a one pixel gap */
#define FG       PURR_RGB565(220, 220, 220)
#define BG       PURR_RGB565(0, 0, 0)
#define CURSOR   PURR_RGB565(255, 180, 0)

static const purr_display_v2_t *s_d;
static purr_term_t s_term;
static int s_cur_x = -1, s_cur_y = -1;   /* where the cursor was drawn last */

esp_err_t purr_console_init(const purr_display_v2_t *d)
{
    purr_display_info_t info;
    if (d == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    d->get_info(&info);
    s_d = d;
    purr_term_init(&s_term, info.width / CELL_W, info.height / ROW_H);
    d->fill(0, 0, info.width, info.height, BG);
    s_cur_x = s_cur_y = -1;
    return ESP_OK;
}

void purr_console_put(void *ctx, char c)
{
    (void)ctx;
    purr_term_putc(&s_term, c);
}

void purr_console_clear(void)
{
    purr_term_clear(&s_term);
}

void purr_console_flush(void)
{
    if (s_d == NULL) {
        return;
    }
    /* A moved cursor means both its old and new rows need drawing. */
    if (s_cur_y >= 0 && (s_cur_x != s_term.cx || s_cur_y != s_term.cy)) {
        s_term.dirty[s_cur_y] = 1;
    }
    for (int r = 0; r < s_term.rows; r++) {
        if (purr_term_take_dirty(&s_term, r)) {
            purr_gfx_text(s_d, 0, r * ROW_H, s_term.cell[r], FG, BG, 1);
            if (r == s_term.cy) {
                s_d->fill(s_term.cx * CELL_W, r * ROW_H + 7, CELL_W, 1, CURSOR);
            }
        }
    }
    /* The cursor row may not have been dirty (the cursor only moved). */
    if (s_cur_x != s_term.cx || s_cur_y != s_term.cy) {
        s_d->fill(s_term.cx * CELL_W, s_term.cy * ROW_H + 7, CELL_W, 1, CURSOR);
    }
    s_cur_x = s_term.cx;
    s_cur_y = s_term.cy;
}
