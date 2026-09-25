// login_ui_main.c — claw_personal_init()/claw_personal_deinit(), the two
// fixed entry points claw_loader_load() looks up by name (see claw_loader.h's
// own "Entry point convention" comment). Backend-agnostic glue: drives
// login_core.c's state machine using whichever render backend variant this
// build was compiled with (login_render_fb.c / login_render_lvgl.c define
// the SAME three functions below — login_render_init/_draw/_poll_key —
// guarded by their own SYSCLAW_BACKEND_FB/_LVGL #if, so exactly one is
// ever a real definition per compiled variant).
//
// Deliberately BLOCKS inside claw_personal_init() until login succeeds —
// no separate task is spawned (xTaskCreate isn't in the import table, and
// doesn't need to be: this whole package runs on whatever task its caller
// already dedicated to it). The boot orchestration (kernel_tdp_boot.c)
// calls claw_loader_load_system()+init() from the SAME protected-process
// task shape the console already uses (purr_kernel_start_protected()) —
// this function blocking IS that task's job for as long as login is
// unresolved, exactly like serial_console_task() already blocks running
// its own loop.
#include "login_core.h"
#include <stdbool.h>
#include <stdio.h>

// Declared by whichever render backend variant this build includes.
extern bool login_render_init(void);
extern void login_render_draw(const login_core_t *lc);
extern int  login_render_poll_key(void);

// See purr_kernel.h's own comment on this function for why it exists —
// the loaded object's only way to yield/sleep without raw FreeRTOS access.
extern void purr_kernel_delay_ms(unsigned int ms);
extern bool purr_kernel_sd_available(void);
extern bool purr_kernel_flash_available(void);

// Reads this package's own staged assets/motd.txt (see catstrap.py's
// _stage_sysclaw_assets() and purrstrap.py's _stage_sysclaw_packages()
// "Assets" block) into lc->motd — proof, on real hardware, that a
// sysclaw package can split "main program" (this compiled object) from
// "assets" (a plain file shipped alongside it, never compiled/relocated)
// and read one back at runtime through ordinary fopen()/fread(). Leaves
// lc->motd empty (not an error) if no assets were staged at all — most
// sysclaw packages have none.
//
// Path is a TOP-LEVEL /flash/assets/login_ui/motd.txt (or /sdcard/
// assets/login_ui/motd.txt as an SD override — same precedent claw_
// loader_system_load() established for the code object itself), NOT
// nested under /flash/system/login_ui.assets/ as this originally staged
// it. Real, hardware-found reason for the change: SPIFFS_OBJ_NAME_LEN
// defaults to 32 bytes INCLUDING the null terminator (31 usable
// characters), and spiffsgen.py's own length check has an off-by-one
// that let "/system/login_ui.assets/motd.txt" (exactly 32 characters)
// through unerrored while it silently overflowed the real on-disk field
// — fopen() on the exact intended path returned NULL at runtime with no
// error anywhere in the build. Confirmed via an on-screen diagnostic
// (self-test against the already-proven-readable /flash/system/login_
// ui.claw path succeeded while this one failed) before finding the real
// cause in spiffsgen.py itself — not guessed.
static void load_motd(login_core_t *lc)
{
    lc->motd[0] = '\0';

    const char *candidates[2];
    int n = 0;
    if (purr_kernel_sd_available())    candidates[n++] = "/sdcard/assets/login_ui/motd.txt";
    if (purr_kernel_flash_available()) candidates[n++] = "/flash/assets/login_ui/motd.txt";

    for (int i = 0; i < n; i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (!f) continue;
        size_t got = fread(lc->motd, 1, sizeof(lc->motd) - 1, f);
        fclose(f);
        lc->motd[got] = '\0';
        // Strip a trailing newline — a plain text asset almost always
        // ends with one, and the render backend draws this as a single
        // status-area line, not a multi-line block.
        for (size_t j = 0; j < got; j++) {
            if (lc->motd[j] == '\n' || lc->motd[j] == '\r') { lc->motd[j] = '\0'; break; }
        }
        return;
    }
}

#define POLL_IDLE_DELAY_MS 15   // matches bbq20's own ~20ms poll cadence closely enough (same figure purr_fbtty.c's fbtty_read_byte() already uses)

// Printable ASCII, Enter (CR/LF), and Backspace (BS/DEL) — same keycode
// convention every other console/TTY code in this tree already assumes
// (purr_console.c, purr_fbtty.c): a driver's KEY_DOWN keycode IS the raw
// byte, no HID-to-ASCII translation layer exists.
#define KEY_BACKSPACE_1 0x08
#define KEY_BACKSPACE_2 0x7F
#define KEY_ENTER_CR    0x0D
#define KEY_ENTER_LF    0x0A

int claw_personal_init(void)
{
    if (!login_render_init()) return -1;

    login_core_t lc;
    login_core_init(&lc);
    load_motd(&lc);
    login_render_draw(&lc);

    while (lc.state != LOGIN_STATE_SUCCESS) {
        int k = login_render_poll_key();
        if (k < 0) {
            purr_kernel_delay_ms(POLL_IDLE_DELAY_MS);
            continue;
        }

        if (k == KEY_ENTER_CR || k == KEY_ENTER_LF) {
            login_core_submit(&lc);
        } else if (k == KEY_BACKSPACE_1 || k == KEY_BACKSPACE_2) {
            login_core_backspace(&lc);
        } else if (k >= 0x20 && k <= 0x7E) {
            login_core_key(&lc, (char)k);
        }
        // Anything else (control bytes this UI has no use for) — ignored.

        login_render_draw(&lc);
    }

    return 0;
}

void claw_personal_deinit(void)
{
    // Nothing to tear down — login_render_init() doesn't allocate
    // anything of its own (no heap allocation, no registered catcall_ui_t
    // to unregister — this package draws straight through catcall_
    // display_t, the same way boot_splash.c/purr_fbtty.c already do,
    // never through purr_kernel_register_ui()). The orchestrator that
    // called claw_personal_init() also owns clearing the screen for
    // whatever comes next.
}
