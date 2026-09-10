// login_ui_main.c — claw_personal_init()/claw_personal_deinit(), the two
// fixed entry points claw_loader_load() looks up by name (see claw_loader.h's
// own "Entry point convention" comment). Backend-agnostic glue: drives
// login_core.c's state machine using whichever render backend variant this
// build was compiled with (login_render_fb.c / login_render_lvgl.c define
// the SAME three functions below — login_render_init/_draw/_poll_key —
// guarded by their own LOGIN_UI_BACKEND_FB/_LVGL #if, so exactly one is
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

// Declared by whichever render backend variant this build includes.
extern bool login_render_init(void);
extern void login_render_draw(const login_core_t *lc);
extern int  login_render_poll_key(void);

// See purr_kernel.h's own comment on this function for why it exists —
// the loaded object's only way to yield/sleep without raw FreeRTOS access.
extern void purr_kernel_delay_ms(unsigned int ms);

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
