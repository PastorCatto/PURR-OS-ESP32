/*
 * recovery_screen.h - the re-install progress screen of the internet recovery loader. It is the boot package's own
 * drawing code (bootpkg/src/pkg_ui.c) on the kernel's display, so the boot screens and the re-install screen are
 * one look: the mark and wordmark, a stage line, the bar, and the status chips under it.
 *
 * Colour T-Deck boards only; elsewhere every call does nothing and the loader keeps printing text.
 */
#ifndef RECOVERY_SCREEN_H
#define RECOVERY_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

void rs_begin(int wifi_ok);                        /* switch from the text console to the screen */
int  rs_active(void);
void rs_line(const char *text);                    /* the stage line above the bar (cut to fit) */
/* The overall bar: step of steps done, plus how far through the current step (thousandths). */
void rs_progress(int step, int steps, int within_per_mille);
void rs_fail(const char *why);                     /* a FAILED chip and the reason */

/* The same screen while the kernel starts (full profile): chips from the bootloader's hand-off (policy, signed,
 * development mode), stage lines and a bar, with the text console muted. It continues the boot menu's hand-off
 * screen. rs_end() gives the screen back to the text console (cleared and repainted); the graphical shell just
 * draws over it. */
void rs_begin_boot(void);
void rs_end(void);

#ifdef __cplusplus
}
#endif

#endif /* RECOVERY_SCREEN_H */
