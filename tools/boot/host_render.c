/* Runs the real boot package code (pkg_main.c, pkg_ui.c, purr_menu.c, the real font) on the Mac against a fake
 * display, and writes PPM snapshots. Build and run through tools/boot/render_real.py.
 *
 * The fake clock advances in hw_delay_ms(); snapshots are taken at fixed ticks of the menu loop, and again when
 * the entry returns (the hand-off screen). Direct ui_splash() calls draw the verify and re-install screens. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "pkg_hw.h"
#include "pkg_ui.h"
#include "purr_abi.h"

static uint16_t fb[LCD_H][LCD_W];
static int ticks;
static int g_keys_at = -1;              /* tick at which a 's' key arrives */
static const char *g_prefix = "";
static int g_snap_at[4], g_nsnap;
static int g_kbd = 1;
static int g_exit_at = -1;

static void snap(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), "%s%s.ppm", g_prefix, name);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", LCD_W, LCD_H);
    for (int y = 0; y < LCD_H; y++)
        for (int x = 0; x < LCD_W; x++) {
            uint16_t v = fb[y][x];
            unsigned char px[3] = {(unsigned char)((v >> 11) * 255 / 31), (unsigned char)(((v >> 5) & 63) * 255 / 63),
                                   (unsigned char)((v & 31) * 255 / 31)};
            fwrite(px, 1, 3, f);
        }
    fclose(f);
}

void hw_init(const purr_boot_services_t *svc) { (void)svc; }
int hw_display_init(void) { return 0; }
void hw_fill(int x, int y, int w, int h, uint16_t c)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            if (xx >= 0 && yy >= 0 && xx < LCD_W && yy < LCD_H) fb[yy][xx] = c;
}
#include "purr_font.h"
int hw_text_width(const char *s, int scale) { return (int)strlen(s) * 8 * scale; }
void hw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    int n = (int)strlen(s);
    if (n == 0 || x < 0 || y < 0 || x + n * 8 * scale > LCD_W || y + 8 * scale > LCD_H) return;   /* as the device */
    for (int i = 0; i < n; i++)
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++)
                hw_fill(x + (i * 8 + c) * scale, y + r * scale, scale, scale,
                        (purr_font_glyph(s[i])[r] & (0x80 >> c)) ? fg : bg);
}
void hw_flush(void) {}
char hw_key(void) { return (g_keys_at >= 0 && ticks == g_keys_at) ? 's' : 0; }
int hw_kbd_ok(void) { return g_kbd; }
void hw_tick(uint32_t dt) { (void)dt; }
void hw_delay_ms(uint32_t ms)
{
    (void)ms;
    ticks++;
    if (g_exit_at >= 0 && ticks >= g_exit_at) exit(0);
    for (int i = 0; i < g_nsnap; i++)
        if (ticks == g_snap_at[i]) {
            char n[16];
            snprintf(n, sizeof(n), "menu-t%d", ticks);
            snap(n);
        }
}
int hw_secure_boot_enabled(void) { return 0; }

static void svc_log(const char *l) { (void)l; }
int purr_pkg_entry(const purr_boot_services_t *svc, const purr_boot_part_t *parts, int nparts, int preferred);

/* scenario: menu | menukey | dev | verify | reinstall      out: prefix for the PPM files */
int main(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "menu";
    g_prefix = argc > 2 ? argv[2] : "";
    static purr_boot_services_t svc = {.version = 2, .log = svc_log};
    purr_boot_part_t parts[3];
    memset(parts, 0, sizeof(parts));
    strcpy(parts[0].name, "Kernel");   parts[0].bootable = 1;
    strcpy(parts[1].name, "KittenOS"); parts[1].bootable = 1;
    svc.secure_state = 2u | PURR_SECST_SIGNED;                       /* enforce, signed trust block */

    if (!strcmp(what, "verify")) {
        ui_chip_t c[] = {{"ENFORCE", UI_CHIP_OK}, {"SIGNED", UI_CHIP_OK}, {"HW SB off", UI_CHIP_PLAIN}};
        ui_splash(c, 3, "Verifying kernel", "62%", 620);
        snap("verify");
    } else if (!strcmp(what, "reinstall")) {
        ui_chip_t c[] = {{"RECOVERY", UI_CHIP_WARN}, {"WIFI ok", UI_CHIP_OK}, {"SIGNED", UI_CHIP_OK}};
        ui_splash(c, 3, "Reinstalling kernel", "38%", 380);
        snap("reinstall");
    } else if (!strcmp(what, "menukey")) {
        g_keys_at = 5;
        g_snap_at[g_nsnap++] = 12;
        g_exit_at = 13;
        purr_pkg_entry(&svc, parts, 2, 0);
    } else {
        if (!strcmp(what, "dev")) svc.secure_state = 2u | PURR_SECST_DEV;
        g_snap_at[g_nsnap++] = 20;
        purr_pkg_entry(&svc, parts, 2, 0);
        snap("handoff");
    }
    return 0;
}
