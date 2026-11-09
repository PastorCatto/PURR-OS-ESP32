/*
 * gamedemo.c - Game demo, a sideloaded sample for the `game` catcall (plan6.md, "The game surface"). No libc.
 *
 * It draws on the panel itself: a dark background, a square that bounces around, and a row of boxes along the bottom, one per button, that light up
 * while the button is held. It shows that the game owns the screen while in front, that the buttons are readable, and what a frame costs: the bar
 * under the title is the time the last frame took (full width = 33 ms, the budget of 30 frames a second). Hold the menu key for a second to leave.
 * One frame per timer event, as the interface suggests: nothing waits inside a callback.
 */
#include <stddef.h>
#include <stdint.h>

#include "purr_app_abi.h"
#include "purr_catcalls.h"

#define NO_OPT __attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))

#define FRAME_MS 33
#define SQ 24
#define BOXES 14
#define BG 0x0841u                  /* a very dark blue-grey */

static const purr_cc_game_t *gm;
static const purr_cc_sched_t *sc;
static int s_timer = -1;
static int s_x = 40, s_y = 40, s_dx = 3, s_dy = 2;
static int s_started;
static uint32_t s_last_buttons = 0xFFFFFFFFu;
static uint32_t s_ms_last;                   /* the last frame's cost, in microseconds */
static uint16_t s_buf[SQ * SQ];
static uint16_t s_strip[320 * 8];

NO_OPT static void fill(uint16_t *p, int n, uint16_t c)
{
    for (int i = 0; i < n; i++) {
        p[i] = c;
    }
}

NO_OPT static void clear_screen(void)
{
    fill(s_strip, 320 * 8, BG);
    for (int y = 0; y < gm->height; y += 8) {
        int h = gm->height - y < 8 ? gm->height - y : 8;
        gm->lcd_write(0, y, gm->width < 320 ? gm->width : 320, h, s_strip);
    }
}

NO_OPT static void box(int i, int lit)
{
    uint16_t c = lit ? 0xFD20u : 0x2104u;       /* orange when held, dark grey when not */
    uint16_t cell[12 * 12];
    fill(cell, 144, c);
    gm->lcd_write(8 + i * 20, gm->height - 20, 12, 12, cell);
}

NO_OPT static void bar(uint32_t us)
{
    int w = (int)(us * 300u / 33000u);
    if (w > 300) {
        w = 300;
    }
    if (w < 1) {
        w = 1;
    }
    fill(s_strip, 320 * 4, BG);
    for (int i = 0; i < 4 * w; i++) {
        s_strip[(i / w) * 320 + (i % w)] = (uint16_t)(us > 33000u ? 0xF800u : 0x07E0u);   /* red when over budget, green otherwise */
    }
    gm->lcd_write(10, 10, 300, 4, s_strip);
}

NO_OPT static void frame(void)
{
    uint64_t t0 = gm->ticks_us();
    int W = gm->width, H = gm->height;
    fill(s_buf, SQ * SQ, BG);
    gm->lcd_write(s_x, s_y, SQ, SQ, s_buf);                 /* erase where it was */
    s_x += s_dx;
    s_y += s_dy;
    if (s_x < 0 || s_x > W - SQ) {
        s_dx = -s_dx;
        s_x += s_dx * 2;
    }
    if (s_y < 20 || s_y > H - SQ - 24) {
        s_dy = -s_dy;
        s_y += s_dy * 2;
    }
    fill(s_buf, SQ * SQ, 0x07FFu);                          /* cyan */
    gm->lcd_write(s_x, s_y, SQ, SQ, s_buf);
    uint32_t b = gm->buttons();
    if (b != s_last_buttons) {
        for (int i = 0; i < BOXES; i++) {
            box(i, (b >> i) & 1u);
        }
        s_last_buttons = b;
    }
    bar(s_ms_last);
    s_ms_last = (uint32_t)(gm->ticks_us() - t0);
}

static int on_create(const purr_app_env_t *env, const void *state, uint32_t len)
{
    (void)state;
    (void)len;
    if (env == NULL || env->catcall_get == NULL) {
        return -1;
    }
    gm = (const purr_cc_game_t *)env->catcall_get("game", 1);
    sc = (const purr_cc_sched_t *)env->catcall_get("sched", 1);
    return gm != NULL && sc != NULL ? 0 : -1;
}

static void on_resume(void)
{
    s_started = 0;                                          /* the screen was used by the shell meanwhile: draw it all again */
    s_last_buttons = 0xFFFFFFFFu;
    if (s_timer < 0) {
        s_timer = sc->timer_start(FRAME_MS, 1);
    }
}

static void on_pause(void)
{
    if (s_timer >= 0) {
        sc->timer_cancel(s_timer);
        s_timer = -1;
    }
}

static void on_event(const purr_app_event_t *ev)
{
    if (gm == NULL || ev->type != PURR_APPEV_TIMER) {
        return;
    }
    if (!s_started) {
        clear_screen();
        s_started = 1;
    }
    frame();
}

static void on_destroy(int reason)
{
    (void)reason;
}

static const purr_app_table2_t s_table = {
    .base = {.abi_version = PURR_APP_ABI_VERSION_2, .on_create = on_create, .on_destroy = on_destroy},
    .on_resume = on_resume, .on_pause = on_pause, .on_event = on_event,
    .memory_needed = 24576, .kind = 3, .flags = 0,
};

__attribute__((used))
const purr_app_table_t *gamedemo_entry(void)
{
    return &s_table.base;
}
