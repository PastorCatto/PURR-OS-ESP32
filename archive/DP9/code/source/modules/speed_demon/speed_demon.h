#pragma once
// speed_demon.h — tear the OS down to bare hardware for one app, then put it back.
//
// A game wants what an OS normally refuses to give up: the whole display, all of
// core 1, the shared SPI bus, and whatever internal DRAM it can get. Speed demon
// unloads everything that is not load-bearing — the UI backend, System UI, the
// mesh stack, Bluetooth, WiFi, the proximity family — leaving the drivers, the
// module registry, and the caller.
//
// ── What this actually recovers, in order of what is scarce here ────────────
//
//   1. INTERNAL DRAM. bt_mgr's NimBLE *controller* holds ~26-27KB of internal
//      DRAM that cannot be routed to PSRAM (it is coupled to radio ISR timing —
//      see sdkconfig_tdeck_plus.overrides). On a board whose free internal DRAM
//      flatlines at 1-2KB, that single module is the largest recovery available.
//      PSRAM is NOT the constrained resource: it sits at ~8.15MB free.
//
//   2. THE SHARED SPI BUS. On T-Deck Plus the LoRa radio shares SPI2 with the
//      display. dp7 (23658e42) fixed a hang caused by exactly that contention.
//      Unloading the mesh stack does not merely free memory, it removes a source
//      of unpredictable multi-millisecond stalls on the bus the game is drawing
//      through. For frame CONSISTENCY this is probably worth more than the RAM.
//
//   3. THE RENDER LOOP. This is the architectural point. The UI backend owns a
//      task on core 1 compositing LVGL onto lv_layer_top() — a game drawing
//      underneath would simply be painted over. Unloading it hands the display
//      to the caller outright: no lock to take, nothing to fight.
//
//   4. PSRAM. The draw buffer plus the whole LVGL object tree. Real, and the
//      least scarce of the four.
//
// ── What it does NOT recover ────────────────────────────────────────────────
// Static .bss. A module's task stack declared as `static StackType_t s[8192]`
// is reserved by the linker and stays reserved whether or not the task exists.
// Budget the gain from heap and from the NimBLE controller, not from module
// teardown in general.
//
// ── Display access afterwards ───────────────────────────────────────────────
// There is no framebuffer to map. catcall_display_t is push-only, so a game
// renders into its own buffer and calls purr_kernel_display()->push_pixels().
// Measured ceiling on T-Deck Plus: a full 320x240 frame is 153,600 bytes, sent
// as 10 chunks of 16KB, ~15.4ms of wire time at a measured-true 80MHz — so
// roughly 55-60fps on display push alone.
//
// ── Recovery ────────────────────────────────────────────────────────────────
// Entering sets a crash-guard marker and exiting clears it, so if the game dies
// with the UI unloaded the next boot comes up in NORMAL mode rather than
// straight back into whatever just crashed. Without that, a game that faults on
// startup is an unrecoverable black screen.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Tear down. `label` names the game on the splash ("DOOM"); may be NULL.
//
// Returns the number of modules unloaded, or -1 if already in speed demon.
// Safe to call from an app's own init: the caller is never unloaded, nor are
// drivers, driver_manager or app_manager.
//
// MUST be called from a task that does not belong to a module being unloaded.
// An app's task qualifies; a UI widget callback does NOT — that runs on the
// render task this is about to delete.
int purr_speed_demon_enter(const char *label);

// Restore exactly what enter() unloaded, in the order it was originally loaded,
// showing a progress splash while it happens. Returns modules restored, or -1
// if not in speed demon.
int purr_speed_demon_exit(void);

bool purr_speed_demon_active(void);

// Liveness beacon — call from the game's main loop, at least every 5 seconds.
// Two consecutive misses (10s of silence) is treated as a hang and routed into
// purr_crash_guard's hang path, giving the same reboot-and-recover behaviour a
// UI hang gets.
//
// This is not optional supervision. The kernel's own UI-hang watchdog is gated
// on a registered UI backend, and speed demon unloads it — so between enter() and
// exit() this beacon is the ONLY thing watching the device, at the one time
// nothing else is left running to notice a freeze.
//
// Once per frame is fine; it costs a timestamp write. Harmless outside game
// mode, so a game need not guard the call.
void purr_speed_demon_heartbeat(void);

#ifdef __cplusplus
}
#endif
