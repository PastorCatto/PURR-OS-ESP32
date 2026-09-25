#pragma once
// blackpurr.h — BlackPURR text-mode shell shared types and drawing API

#include <stdint.h>
#include <stdbool.h>

// ── Colors (RGB565 native — no LV_COLOR_16_SWAP needed here) ─────────────────
#define BP_COL_BG        ((uint16_t)0x0000)   // black
#define BP_COL_SURFACE   ((uint16_t)0x18E3)   // #181818 dark grey
#define BP_COL_ACCENT    ((uint16_t)0xF320)   // #FF6600 orange
#define BP_COL_SEL       ((uint16_t)0x39C7)   // #383838 selection bg
#define BP_COL_TEXT      ((uint16_t)0xFFFF)   // white
#define BP_COL_DIM       ((uint16_t)0x4208)   // #404040 dim grey
#define BP_COL_GREEN     ((uint16_t)0x07E0)   // status ok
#define BP_COL_RED       ((uint16_t)0xF800)   // error/warn

// ── Layout (320x240) ─────────────────────────────────────────────────────────
#define BP_LCD_W         320
#define BP_LCD_H         240
#define BP_STATUS_H      16       // top status bar
#define BP_BOTTOM_H      18       // bottom hint bar
#define BP_GRID_Y        (BP_STATUS_H + 1)
#define BP_GRID_H        (BP_LCD_H - BP_STATUS_H - BP_BOTTOM_H - 2)
#define BP_COLS          4
#define BP_ROWS          3
#define BP_CELL_W        (BP_LCD_W / BP_COLS)    // 80px
#define BP_CELL_H        (BP_GRID_H / BP_ROWS)   // ~68px

// ── Shell API ─────────────────────────────────────────────────────────────────
void blackpurr_shell_init(void);
void blackpurr_shell_tick(uint32_t uptime_s);
void blackpurr_shell_on_key(uint8_t keycode);
void blackpurr_shell_on_pointer(int16_t dx, int16_t dy);
void blackpurr_shell_on_click(void);
void blackpurr_shell_on_touch(uint16_t x, uint16_t y);

// ── Module entry ──────────────────────────────────────────────────────────────
int  blackpurr_init(void);
void blackpurr_deinit(void);
