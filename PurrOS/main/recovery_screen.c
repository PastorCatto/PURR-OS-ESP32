/* recovery_screen.c - see recovery_screen.h. pkg_ui.c is compiled in here, against the kernel's display. */
#include "recovery_screen.h"

#include "sdkconfig.h"

#if CONFIG_PURR_BOARD_TDECK || CONFIG_PURR_BOARD_TDECK_PLUS

#include <stdint.h>
#include <string.h>

#include "purr_console.h"
#include "purr_display.h"
#include "purr_font.h"
#include "purr_kernel.h"

/* What the boot package's board header and hw_*() would have supplied. */
#define PKG_UI_SHIM 1
#define LCD_W 320
#define LCD_H 240
#define BOARD_HAS_KEYBOARD 1
#if CONFIG_PURR_BOARD_TDECK_PLUS
#define BOARD_CODENAME "BlackDeck+"
#else
#define BOARD_CODENAME "BlackDeck"
#endif
#define PKG_VERSION "recovery"

static const purr_display_v2_t *s_d;

static void hw_fill(int x, int y, int w, int h, uint16_t c)
{
    if (s_d != NULL && w > 0 && h > 0) {
        s_d->fill(x, y, w, h, c);
    }
}

static int hw_text_width(const char *s, int scale) { return (int)strlen(s) * 8 * scale; }

static void hw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    uint16_t cell[8 * 3 * 8 * 3];                           /* one glyph at up to 3x */
    if (s_d == NULL || scale < 1 || scale > 3) {
        return;
    }
    for (int i = 0; s[i]; i++) {
        const uint8_t *g = purr_font_glyph(s[i]);
        int w = 8 * scale;
        for (int r = 0; r < 8 * scale; r++) {
            for (int c = 0; c < w; c++) {
                cell[r * w + c] = (g[r / scale] & (0x80 >> (c / scale))) ? fg : bg;
            }
        }
        s_d->blit(x + i * w, y, w, 8 * scale, cell);
    }
}

static void hw_flush(void) {}

#include "../../bootpkg/src/pkg_ui.c"

static int s_active, s_wifi;
static int s_per_mille;
static char s_line[34];

static void draw_line(void)
{
    char pct[8];
    int n = 0, v = s_per_mille / 10;
    char t[4];
    int k = 0;
    if (v <= 0) t[k++] = '0';
    while (v > 0 && k < 3) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k > 0) pct[n++] = t[--k];
    pct[n++] = '%';
    pct[n] = '\0';
    ui_above(s_line, pct);
}

void rs_begin(int wifi_ok)
{
    s_d = purr_kernel_display();
    if (s_d == NULL || s_active) {
        return;
    }
    s_wifi = wifi_ok;
    purr_console_set_muted(1);                  /* the text console must not paint over the screen */
    ui_chip_t chips[3] = {{"RECOVERY", UI_CHIP_WARN}, {"WIFI ok", UI_CHIP_OK}, {"SIGNED", UI_CHIP_OK}};
    ui_frame();
    ui_bar(0);
    s_per_mille = 0;
    s_line[0] = '\0';
    ui_chips(chips, wifi_ok ? 3 : 1);
    s_active = 1;
}

int rs_active(void) { return s_active; }

void rs_begin_boot(void)
{
    s_d = purr_kernel_display();
    if (s_d == NULL || s_active) {
        return;
    }
    static const char *const modes[] = {"OFF", "WARN", "ENFORCE", "WARN"};
    ui_chip_t chips[3];
    int n = 0;
    purr_handoff_t h;
    if (purr_kernel_handoff(&h) == 0) {
        if (h.secure_state & PURR_SECST_DEV) {
            chips[n].label = "DEV MODE"; chips[n++].kind = UI_CHIP_HOT;
        }
        chips[n].label = modes[PURR_SECST_MODE(h.secure_state)];
        chips[n++].kind = PURR_SECST_MODE(h.secure_state) == 2 ? UI_CHIP_OK : UI_CHIP_WARN;
        if (!(h.secure_state & PURR_SECST_DEV)) {
            chips[n].label = (h.secure_state & PURR_SECST_SIGNED) ? "SIGNED" : "UNSIGNED";
            chips[n++].kind = (h.secure_state & PURR_SECST_SIGNED) ? UI_CHIP_OK : UI_CHIP_WARN;
        }
    }
    purr_console_set_muted(1);                  /* the log text must not paint over the screen */
    ui_frame();
    s_per_mille = 1000;
    ui_bar(1000);                               /* the boot menu's hand-off left the bar full */
    s_line[0] = '\0';
    ui_chips(chips, n);
    s_active = 1;
}

void rs_end(void)
{
    if (!s_active) {
        return;
    }
    s_active = 0;
    s_d->fill(0, 0, LCD_W, LCD_H, 0);
    purr_console_set_muted(0);                  /* every row is repainted */
}

void rs_line(const char *text)
{
    if (!s_active) {
        return;
    }
    strncpy(s_line, text, sizeof(s_line) - 1);  /* 240 px of bar is 30 characters; the percentage needs room */
    s_line[sizeof(s_line) - 1] = '\0';
    if (strlen(s_line) > 22) {
        s_line[22] = '\0';
    }
    draw_line();
}

void rs_progress(int step, int steps, int within)
{
    if (!s_active || steps <= 0) {
        return;
    }
    if (within < 0) within = 0;
    if (within > 1000) within = 1000;
    s_per_mille = (step * 1000 + within) / steps;
    if (s_per_mille > 1000) s_per_mille = 1000;
    ui_bar(s_per_mille);
    draw_line();
}

void rs_fail(const char *why)
{
    if (!s_active) {
        return;
    }
    ui_chip_t chips[2] = {{"FAILED", UI_CHIP_HOT}, {"RECOVERY", UI_CHIP_WARN}};
    strncpy(s_line, why, sizeof(s_line) - 1);
    s_line[sizeof(s_line) - 1] = '\0';
    ui_above(s_line, 0);
    ui_chips(chips, 2);
}

#else  /* not a colour T-Deck: the loader keeps printing text */

void rs_begin(int wifi_ok) { (void)wifi_ok; }
int rs_active(void) { return 0; }
void rs_line(const char *text) { (void)text; }
void rs_progress(int step, int steps, int within) { (void)step; (void)steps; (void)within; }
void rs_fail(const char *why) { (void)why; }
void rs_begin_boot(void) {}
void rs_end(void) {}

#endif
