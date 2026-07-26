// game_mode.c — see game_mode.h for what this buys and why.
//
// Deliberately NOT a registered module (no PURR_MODULE_REGISTER), same as
// boot_splash. Two reasons: it must not appear in the registry it is walking and
// unloading, and it has no init/deinit lifecycle worth having — it is a pair of
// functions an app calls.

#include <string.h>
#include <stdio.h>
#include "game_mode.h"
#include "purr_kernel.h"
#include "purr_module.h"
#include "purr_crash_guard.h"
#include "../boot_splash/boot_splash.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "game_mode";

// Enough for every module this OS ships plus room to grow. Overflow degrades to
// "unload fewer things" rather than misbehaving — see the guard in enter().
#define MAX_SUSPENDED 32

static const char *s_suspended[MAX_SUSPENDED];
static int         s_suspended_n = 0;
static bool        s_active      = false;

// Crash-guard entity name. Distinct from any module name so it cannot collide
// with a module's own strike bookkeeping.
#define GAME_MODE_GUARD_ENTITY "game_mode"

// Liveness beacon. The game calls purr_game_mode_heartbeat() at least this
// often; two consecutive misses (10s of silence) is treated as a hang.
#define GAME_MODE_BEAT_MS      5000
#define GAME_MODE_MISSED_BEATS 2

bool purr_game_mode_active(void) { return s_active; }

// Modules that stay no matter what.
//
// Everything else is decided by TYPE rather than by a name list, deliberately:
// a name list silently stops covering new modules as they are added, and the
// failure mode is "game mode quietly got worse over time" with nothing to
// notice it. Types are structural and do not rot.
//
//   PURR_MOD_DRIVER — display, touch, input, radio, gps, battery. The game
//                     needs the display and input; the rest are inert once
//                     nothing is driving them, and unloading a driver would
//                     tear down a catcall that the registry hands out pointers
//                     to. Not worth the risk for the memory involved.
//   PURR_MOD_APP    — apps are inert unless launched, and one of them is the
//                     caller. Unloading the caller would delete the task
//                     executing this function.
//
// driver_manager and app_manager are PURR_MOD_SYSTEM but structural — the same
// two the kernel's own denylist protects (module_is_denylisted()).
static bool is_kept(const purr_module_header_t *h)
{
    // name is a fixed array in the header, never a pointer — so test its
    // contents, not its address.
    if (!h || h->name[0] == '\0') return true;
    if (h->module_type == PURR_MOD_DRIVER) return true;
    if (h->module_type == PURR_MOD_APP)    return true;
    if (strcmp(h->name, "driver_manager") == 0) return true;
    if (strcmp(h->name, "app_manager")    == 0) return true;
    return false;
}

static size_t free_internal(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }

int purr_game_mode_enter(const char *label)
{
    if (s_active) return -1;

    // Snapshot the names to unload BEFORE unloading anything: the registry is
    // being mutated underneath, so walking and unloading in one pass would skip
    // entries as indices shift.
    //
    // Walked in reverse so the list ends up in reverse load order. Load order
    // already respects dependencies (priority, then registration), so unloading
    // its reverse means nothing is torn out from under a module still running.
    // Concretely: the UI backend must go before systemui, because the backend's
    // task calls purr_systemui_tick() every frame.
    s_suspended_n = 0;
    for (int i = purr_kernel_module_count() - 1; i >= 0; i--) {
        const purr_module_header_t *h = purr_kernel_module_at(i);
        if (is_kept(h)) continue;
        if (s_suspended_n >= MAX_SUSPENDED) {
            ESP_LOGW(TAG, "more than %d suspendable modules — leaving the rest loaded",
                     MAX_SUSPENDED);
            break;
        }
        // Storing the name pointer, not a copy: module headers are static
        // (PURR_MODULE_REGISTER puts them in rodata), so the string outlives
        // the unload. purr_kernel_get_static_module() needs exactly this name
        // to find the header again on the way back.
        s_suspended[s_suspended_n++] = h->name;
    }

    // TAKE THE UI LOCK BEFORE DRAWING OR UNLOADING ANYTHING.
    //
    // This is not defensive; without it the first hardware run crashed every
    // time with:
    //
    //   assert failed: spi_device_release_bus spi_master.c:1359 (ret == ESP_OK)
    //
    // The UI backend's render task calls push_pixels() from inside
    // lv_timer_handler(), which sits between spi_device_acquire_bus() and
    // spi_device_release_bus(). Unloading that module vTaskDelete()s the task —
    // and if the delete lands between those two calls, the SPI bus stays
    // acquired by a task that no longer exists. The next release, from anyone,
    // asserts and panics. That then struck the crash guard, rebooted, and
    // relaunched into the same crash.
    //
    // Every LVGL backend's render loop takes this same lock around its whole
    // iteration (see mochi_task), so holding it here guarantees no draw is in
    // flight and no new one can start. It also stops the splash below fighting
    // the still-running UI for the panel.
    //
    // Safe to hold across module deinit: the mutex is RECURSIVE
    // (xSemaphoreCreateRecursiveMutex in purr_kernel.c), so a deinit that takes
    // it again on this same task — several backends do — re-enters rather than
    // deadlocking.
    purr_kernel_ui_lock();

    // +1 step for the final settle, so the bar reaches the end rather than
    // stopping just short of it.
    char title[48];
    snprintf(title, sizeof(title), "%s", label ? label : "Game Mode");
    purr_splash_show(title, s_suspended_n + 1);
    purr_splash_status("freeing system resources");

    size_t before = free_internal();

    // Marker first. If the game faults during startup — before it ever manages
    // a clean exit — the next boot must not walk straight back into it.
    purr_crash_guard_mark_start(GAME_MODE_GUARD_ENTITY);

    // Liveness watch. The kernel's UI-hang check is gated on a registered UI
    // backend, and the next few lines unload it — so without this the device
    // runs completely unsupervised for the whole of game mode, which is exactly
    // the window where a hang is unrecoverable: one app owns the display and
    // input, and nothing is left running to notice it stopped.
    //
    // 5s beat, react after 2 missed. Deliberately slacker than the UI's 6s: a
    // game legitimately spends longer between beats than a render loop does —
    // loading a level, seeking a WAD on SD — and a false positive here reboots
    // the device out from under someone who is playing.
    purr_kernel_watch_begin(GAME_MODE_GUARD_ENTITY,
                            GAME_MODE_BEAT_MS, GAME_MODE_MISSED_BEATS);

    s_active = true;   // set before unloading: the UI backend's deinit may run
                       // code that asks whether game mode is active.

    for (int i = 0; i < s_suspended_n; i++) {
        purr_splash_status(s_suspended[i]);
        purr_kernel_unload_module(s_suspended[i]);
        purr_splash_advance();
    }

    size_t after = free_internal();
    ESP_LOGW(TAG, "entered: %d modules unloaded, internal DRAM %u -> %u (+%d bytes)",
             s_suspended_n, (unsigned)before, (unsigned)after, (int)(after - before));

    purr_splash_status(label ? label : "loading");
    purr_splash_advance();

    // Released only now. Every UI backend that could have been drawing is gone,
    // so from here the caller owns the panel outright and takes the SPI bus
    // per-push through the display driver like anyone else.
    purr_kernel_ui_unlock();
    return s_suspended_n;
}

int purr_game_mode_exit(void)
{
    if (!s_active) return -1;

    // Same lock, mirrored — see enter() for the SPI assert this prevents.
    // Restoring a UI backend starts its render task, which begins drawing
    // immediately; holding the lock keeps it queued behind this splash until
    // every module is back, instead of interleaving with it on the panel and
    // on the SPI bus.
    purr_kernel_ui_lock();

    // The splash is the only thing on screen from here until a UI backend
    // repaints, which is the whole reason it exists — the game has stopped
    // drawing and nothing has replaced it yet.
    purr_splash_show("Restoring PURR OS", s_suspended_n + 1);
    purr_splash_status("restarting services");

    // Reverse of the suspend list = original load order.
    int restored = 0;
    for (int i = s_suspended_n - 1; i >= 0; i--) {
        purr_splash_status(s_suspended[i]);
        if (purr_kernel_enable_static_module(s_suspended[i]) == 0) restored++;
        else ESP_LOGW(TAG, "failed to restore %s", s_suspended[i]);
        purr_splash_advance();
    }

    s_suspended_n = 0;
    s_active      = false;

    // Watch off before the guard: once the OS is back, the UI backend's own
    // heartbeat resumes covering the device, and leaving this armed would trip
    // on the first slow frame after restore.
    purr_kernel_watch_end();

    // Only now: a clean exit is what proves the game did not take the device
    // down with it.
    purr_crash_guard_mark_stop(GAME_MODE_GUARD_ENTITY, true, NULL);

    ESP_LOGW(TAG, "exited: %d modules restored, internal DRAM %u free",
             restored, (unsigned)free_internal());

    purr_splash_advance();

    // Released last: the restored UI backend's render task has been blocked on
    // this since it started, and takes over the panel the moment it runs.
    purr_kernel_ui_unlock();
    return restored;
}

// The beacon. A game calls this from its main loop — once per frame is fine and
// costs a timestamp write; the requirement is only that no two consecutive
// GAME_MODE_BEAT_MS windows pass without one.
//
// Thin wrapper rather than exposing purr_kernel_watch_beat() directly, so a
// game never has to know which watch it is under, and so calling it outside
// game mode is harmless.
void purr_game_mode_heartbeat(void)
{
    if (!s_active) return;
    purr_kernel_watch_beat();
}
