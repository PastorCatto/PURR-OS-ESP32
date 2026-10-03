/*
 * PURR OS boot package: the boot menu (bootloader/SPEC.md section 9).
 *
 * The menu rules are PurrOS/components/coreos/src/purr_menu.c, shared with the host
 * tests. This file draws it and feeds it keys and time.
 */
#include <stdint.h>

#include "board.h"
#include "pkg_hw.h"
#include "purr_abi.h"
#include "purr_menu.h"

#define BLACK   0x0000
#define WHITE   0xFFFF
#define GREY    0x94B2
#define ORANGE  0xFDA0
#define TICK_MS 50

/* No snprintf here: these build the few strings the menu shows. */
static void put_str(char *dst, int *n, const char *s)
{
    while (*s && *n < 47) dst[(*n)++] = *s++;
    dst[*n] = '\0';
}

static void draw_header(void)
{
    hw_fill(0, 0, LCD_W, LCD_H, BLACK);
    hw_text(16, 16, "PURR OS", ORANGE, BLACK, 3);
#ifdef BOARD_CODENAME
    hw_text(16, 48, "boot menu - " BOARD_CODENAME, GREY, BLACK, 1);
#else
    hw_text(16, 48, "boot menu", GREY, BLACK, 1);
#endif
    hw_flush();
}

#if BOARD_HAS_KEYBOARD
static void draw_kbd(void)
{
    hw_fill(120, 48, 200, 8, BLACK);
    if (hw_kbd_ok()) {
        hw_text(120, 48, "keyboard ok", GREY, BLACK, 1);
    } else {
        hw_text(120, 48, "keyboard not answering", 0xF800, BLACK, 1);
    }
    hw_flush();
}
#else
/* No keyboard on this board (BOARD_HAS_KEYBOARD 0) -- the same status line instead names
 * the real input method, rather than reporting on hardware this board doesn't have. */
static void draw_kbd(void)
{
    hw_fill(120, 48, 200, 8, BLACK);
    hw_text(120, 48, "BOOT=down/hold PWR=up", GREY, BLACK, 1);
    hw_flush();
}
#endif

static void draw_secure_boot(void)
{
    hw_fill(16, 60, 300, 8, BLACK);
    if (hw_secure_boot_enabled()) {
        hw_text(16, 60, "Secure Boot: ON", ORANGE, BLACK, 1);
    } else {
        hw_text(16, 60, "Secure Boot: off", GREY, BLACK, 1);
    }
    hw_flush();
}

static void draw_list(const purr_menu_t *m, const char *message)
{
    hw_fill(0, 70, LCD_W, 130, BLACK);
    if (message) {
        hw_text(16, 100, message, WHITE, BLACK, 2);
        hw_flush();
        return;
    }
    if (!purr_menu_visible(m)) {
        hw_text(16, 100, "No system found...", GREY, BLACK, 2);
        hw_flush();
        return;
    }
    if (m->no_boot_options) {
        hw_text(16, 76, "Nothing to boot.", WHITE, BLACK, 1);
    }
    for (int i = 0; i < m->count; i++) {
        int y = 100 + i * 28;
        int sel = (i == m->selected);
        /* Selection is a border, not an inverted fill: text stays WHITE-on-BLACK always,
         * so it reads the same whether selected or not -- a filled/inverted row used to
         * swap to BLACK-on-ORANGE, which on the e-ink path's two-tone threshold
         * (pkg_hw_epd.c's hw_fill/hw_text: any non-zero color reads as WHITE) collapsed
         * the highlight and the glyph color together and made the selected row
         * unreadable. */
        if (sel) {
            int bx = 12, by = y - 4, bw = LCD_W - 24, bh = 24;
            hw_fill(bx, by, bw, 2, ORANGE);
            hw_fill(bx, by + bh - 2, bw, 2, ORANGE);
            hw_fill(bx, by, 2, bh, ORANGE);
            hw_fill(bx + bw - 2, by, 2, bh, ORANGE);
        }
        char line[48];
        int n = 0;
        line[0] = '\0';
        put_str(line, &n, m->entries[i].label);
        hw_text(20, y, line, WHITE, BLACK, 2);
    }
    hw_flush();
}

static void draw_footer(const purr_menu_t *m)
{
    hw_fill(0, LCD_H - 28, LCD_W, 28, BLACK);
    char foot[48];
    int n = 0;
    foot[0] = '\0';
    int s = purr_menu_seconds_left(m);
    if (s >= 0) {
        char d[2] = {(char)('0' + (s % 10)), 0};
        put_str(foot, &n, "Booting in ");
        put_str(foot, &n, d);
        put_str(foot, &n, "...  any key stops");
    } else {
#if BOARD_HAS_KEYBOARD
        put_str(foot, &n, "W up  S down  D or Enter select");
#else
        put_str(foot, &n, "BOOT: down / hold = select  PWR: up");
#endif
    }
    hw_text(16, LCD_H - 24, foot, GREY, BLACK, 1);
    hw_flush();
}

int purr_pkg_entry(const purr_boot_services_t *svc, const purr_boot_part_t *parts,
                   int nparts, int preferred)
{
    hw_init(svc);
    if (hw_display_init() != 0) {
        return -1;
    }
#ifdef BOARD_CODENAME
    svc->log("bootpkg: " BOARD_CODENAME " (" BOARD_NAME ")");
#endif
    svc->log("bootpkg: display up");
    svc->log(hw_secure_boot_enabled() ? "bootpkg: secure boot HW = ON" : "bootpkg: secure boot HW = off");

    for (;;) {
        purr_menu_part_t mp[PURR_PKG_MAX_PARTS];
        int n = nparts > PURR_PKG_MAX_PARTS ? PURR_PKG_MAX_PARTS : nparts;
        for (int i = 0; i < n; i++) {
            int j = 0;
            for (; j < 15 && parts[i].name[j]; j++) mp[i].label[j] = parts[i].name[j];
            mp[i].label[j] = '\0';
            mp[i].bootable = parts[i].bootable;
        }

        purr_menu_t m;
        purr_menu_init(&m, mp, n, preferred);
        draw_header();
        draw_secure_boot();
        hw_key();
        draw_kbd();
        draw_list(&m, NULL);
        draw_footer(&m);

        int last_sel = m.selected, last_secs = purr_menu_seconds_left(&m);
        int last_vis = purr_menu_visible(&m);
        int last_kbd = hw_kbd_ok();
        purr_menu_result_t r = {PURR_MENU_ACT_NONE, -1};
        while (r.action == PURR_MENU_ACT_NONE) {
            hw_delay_ms(TICK_MS);
            hw_tick(TICK_MS);
            r = purr_menu_step(&m, TICK_MS, purr_key_from_char(hw_key()));
            if (hw_kbd_ok() != last_kbd) {
                last_kbd = hw_kbd_ok();
                draw_kbd();
            }
            int secs = purr_menu_seconds_left(&m), vis = purr_menu_visible(&m);
            if (m.selected != last_sel || vis != last_vis) {
                draw_list(&m, NULL);
            }
            if (secs != last_secs || vis != last_vis || m.selected != last_sel) {
                draw_footer(&m);
            }
            last_sel = m.selected;
            last_secs = secs;
            last_vis = vis;
        }

        if (r.action == PURR_MENU_ACT_BOOT) {
            svc->log("bootpkg: booting the chosen slot");
            return r.index;
        }
        svc->log("bootpkg: starting internet recovery");
        return PURR_PKG_CHOICE_INTERNET_RECOVERY;
    }
}
