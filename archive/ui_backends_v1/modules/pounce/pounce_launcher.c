// pounce_launcher.c — Pounce's home screen: an app grid shown whenever the
// modal window stack is empty (pounce_plan.md never specified one — this
// closes that gap, confirmed live: boot completed but nothing was ever
// drawn below the status strip because no window was ever open).
//
// Built directly on BlackPurr's proven app-grid pattern
// (source/modules/blackpurr/blackpurr_shell.c) — same draw-cell/move-
// selection/launch-on-activate shape, own state (this isn't a purr_win_t
// window, it's shell-level chrome exactly like the status strip is), own
// palette (the hacker-terminal green-on-black look, not BlackPurr's
// orange/white), own font (font6x8.h), and own input path (Pounce's
// existing trackball-delta-to-discrete-step accumulator and keyboard
// dispatch in pounce_focus.c, not BlackPurr's separate touch/pointer/key
// callbacks — no gesture/silkscreen bar needed, this backend is
// keyboard+trackball-first, not touch-first).
#include "sdkconfig.h"

#ifdef CONFIG_PURR_UI_BACKEND_POUNCE

#include "pounce.h"
#include "../../kernel/core/purr_kernel.h"
#include "../app_manager/app_manager.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "pounce_launcher";

// Target cell footprint — actual cols/rows are derived from the real
// display size at draw time (pw_layout_compute() already queries
// display_info_t the same way; a hardcoded 320x240 grid would silently
// misdraw on any other Pounce-capable device later).
#define PW_CELL_TARGET_W 80
#define PW_CELL_TARGET_H 60

#define PW_LAUNCHER_MAX_APPS 64
static app_entry_t s_apps[PW_LAUNCHER_MAX_APPS];
static int         s_app_count = 0;
static int         s_sel  = 0;
static int         s_page = 0;

static int  s_cols = 1, s_rows = 1;
static int16_t s_cell_w = PW_CELL_TARGET_W, s_cell_h = PW_CELL_TARGET_H;
static int16_t s_grid_y = PW_STATUS_H;

static void compute_grid(void) {
    const catcall_display_t *disp = purr_kernel_display();
    if (!disp) return;
    display_info_t info;
    disp->get_info(&info);

    s_cols = (int)info.width / PW_CELL_TARGET_W;
    if (s_cols < 1) s_cols = 1;
    s_cell_w = (int16_t)(info.width / s_cols);

    int16_t grid_h = (int16_t)(info.height - PW_STATUS_H);
    s_rows = grid_h / PW_CELL_TARGET_H;
    if (s_rows < 1) s_rows = 1;
    s_cell_h = (int16_t)(grid_h / s_rows);
    s_grid_y = PW_STATUS_H;
}

static void cell_xy(int idx, int16_t *cx, int16_t *cy) {
    int col = idx % s_cols;
    int row = (idx / s_cols) % s_rows;
    *cx = (int16_t)(col * s_cell_w);
    *cy = (int16_t)(s_grid_y + row * s_cell_h);
}

static void rescan(void) {
    s_app_count = 0;
    int n = app_manager_count();
    if (n > PW_LAUNCHER_MAX_APPS) n = PW_LAUNCHER_MAX_APPS;
    for (int i = 0; i < n; i++) {
        const app_entry_t *e = app_manager_get(i);
        if (e) s_apps[s_app_count++] = *e;
    }
}

static void draw_cell(int idx, bool selected) {
    int16_t cx, cy;
    cell_xy(idx, &cx, &cy);

    uint32_t bg = selected ? PW_COL_FOCUS_BG : PW_COL_BG;
    uint32_t fg = selected ? PW_COL_FOCUS_FG : PW_COL_FG;
    uint32_t border = selected ? PW_COL_ACCENT : PW_COL_DIM;

    pw_fill_rect(cx, cy, s_cell_w, s_cell_h, bg);
    pw_fill_rect(cx, cy, s_cell_w, 1, border);
    pw_fill_rect(cx, (int16_t)(cy + s_cell_h - 1), s_cell_w, 1, border);
    pw_fill_rect(cx, cy, 1, s_cell_h, border);
    pw_fill_rect((int16_t)(cx + s_cell_w - 1), cy, 1, s_cell_h, border);

    int page_base = s_page * s_cols * s_rows;
    int app_idx   = page_base + idx;
    if (app_idx >= s_app_count) return;

    const char *name = s_apps[app_idx].name;
    int max_chars = (s_cell_w - 4) / PW_CHAR_W;
    if (max_chars < 1) max_chars = 1;
    int16_t tx = (int16_t)(cx + 2);
    int16_t ty = (int16_t)(cy + (s_cell_h - PW_CHAR_H) / 2);
    pw_draw_string_clipped(tx, ty, name, max_chars, fg, bg);
}

void pw_launcher_draw(void) {
    const catcall_display_t *disp = purr_kernel_display();
    if (!disp) return;
    display_info_t info;
    disp->get_info(&info);

    rescan();
    compute_grid();

    pw_fill_rect(0, PW_STATUS_H, (int16_t)info.width, (int16_t)(info.height - PW_STATUS_H), PW_COL_BG);

    int per_page = s_cols * s_rows;
    int total_pages = (s_app_count + per_page - 1) / per_page;
    if (total_pages < 1) total_pages = 1;
    if (s_page >= total_pages) s_page = total_pages - 1;
    if (s_sel >= per_page) s_sel = per_page - 1;
    if (s_sel < 0) s_sel = 0;

    for (int i = 0; i < per_page; i++) {
        int app_idx = s_page * per_page + i;
        if (app_idx < s_app_count) draw_cell(i, i == s_sel);
    }

    ESP_LOGI(TAG, "drawn — %d apps, %dx%d grid, page %d/%d",
             s_app_count, s_cols, s_rows, s_page + 1, total_pages);
}

void pw_launcher_move(int ddx, int ddy) {
    int per_page = s_cols * s_rows;
    int old_sel = s_sel;
    int old_page = s_page;

    int row = s_sel / s_cols + ddy;
    int col = s_sel % s_cols + ddx;

    if (col < 0) { col = s_cols - 1; row--; }
    if (col >= s_cols) { col = 0; row++; }

    if (row < 0) {
        if (s_page > 0) { s_page--; row = s_rows - 1; }
        else row = 0;
    } else if (row >= s_rows) {
        int total_pages = (s_app_count + per_page - 1) / per_page;
        if (s_page < total_pages - 1) { s_page++; row = 0; }
        else row = s_rows - 1;
    }

    s_sel = row * s_cols + col;
    int max_on_page = s_app_count - s_page * per_page;
    if (max_on_page > per_page) max_on_page = per_page;
    if (max_on_page < 0) max_on_page = 0;
    if (s_sel >= max_on_page && max_on_page > 0) s_sel = max_on_page - 1;

    if (s_page != old_page) {
        pw_launcher_draw();
    } else if (s_sel != old_sel) {
        draw_cell(old_sel, false);
        draw_cell(s_sel, true);
    }
}

void pw_launcher_activate(void) {
    int idx = s_page * s_cols * s_rows + s_sel;
    if (idx < 0 || idx >= s_app_count) return;
    ESP_LOGI(TAG, "launching: %s", s_apps[idx].name);
    app_manager_launch_idx(idx);
}

#endif  // CONFIG_PURR_UI_BACKEND_POUNCE
