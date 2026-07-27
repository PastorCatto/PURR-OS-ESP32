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

// Names are COPIED, not pointed at. purr_kernel_module_at() returns
// &s_modules[idx].header — a copy inside a mutable array, not the static
// PURR_MODULE_REGISTER header in rodata. Unloading COMPACTS that array, so a
// stored h->name pointer silently starts referring to whatever slot shifted
// into its place.
//
// That produced a spectacular failure on hardware: the first unload was
// correct, and every one after it targeted a different module than intended.
// Apps live later in the registry, so game mode tore down settings, terminal,
// fileman, msn, milkbar — and magidos itself, the caller — while leaving the UI
// backend and mesh stack running. "unloads nearby twice" in the log was the
// same shift showing up as a duplicate.
//
// 32 matches purr_module_header_t::name.
static char s_suspended[MAX_SUSPENDED][32];
// Which entries actually came out. An entry that failed to unload must not be
// "restored" later - it never left.
static bool s_unloaded[MAX_SUSPENDED];
static int  s_suspended_n = 0;
static bool s_active      = false;

// Crash-guard entity name. Distinct from any module name so it cannot collide
// with a module's own strike bookkeeping.
#define GAME_MODE_GUARD_ENTITY "game_mode"

// Liveness beacon. The game calls purr_game_mode_heartbeat() at least this
// often; two consecutive misses (10s of silence) is treated as a hang.
#define GAME_MODE_BEAT_MS      5000
#define GAME_MODE_MISSED_BEATS 2

// Per-module ceiling on deinit(). Generous - some modules legitimately wait on
// a task to finish - but finite, because a hung deinit with the UI already
// unloaded is an unrecoverable black screen.
#define GAME_MODE_UNLOAD_TIMEOUT_MS 3000

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

// Trampoline for purr_kernel_run_bounded(), which runs its callee on a helper
// task so a deinit that never returns cannot take the caller with it.
static void unload_one(void *arg) { purr_kernel_unload_module((const char *)arg); }

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
    memset(s_unloaded, 0, sizeof(s_unloaded));
    for (int i = purr_kernel_module_count() - 1; i >= 0; i--) {
        const purr_module_header_t *h = purr_kernel_module_at(i);
        if (is_kept(h)) continue;
        if (s_suspended_n >= MAX_SUSPENDED) {
            ESP_LOGW(TAG, "more than %d suspendable modules — leaving the rest loaded",
                     MAX_SUSPENDED);
            break;
        }
        // COPIED — see s_suspended's declaration for the registry-compaction
        // bug that storing the pointer caused.
        snprintf(s_suspended[s_suspended_n], sizeof(s_suspended[0]), "%s", h->name);
        s_suspended_n++;
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

    // PHASE 1 — UI backends, directly, with the lock still held.
    //
    // These are the ones that must not be deleted mid-draw (see the lock
    // comment above), and they must go before systemui, whose tick they call
    // every frame. Unloaded directly rather than through the bounded helper
    // below, because that helper runs the deinit on ANOTHER task — and several
    // backends take purr_kernel_ui_lock() in their own deinit, which this task
    // is currently holding. Recursive mutexes only re-enter for the same task,
    // so a helper task would block on it forever.
    for (int i = 0; i < s_suspended_n; i++) {
        const purr_module_header_t *h = purr_kernel_get_static_module(s_suspended[i]);
        if (!h || h->module_type != PURR_MOD_UI) continue;
        purr_splash_status(s_suspended[i]);
        purr_kernel_unload_module(s_suspended[i]);
        s_unloaded[i] = true;
        purr_splash_advance();
    }

    // Lock released before phase 2 so a deinit that wants it can have it.
    // Safe now: every UI backend is gone, so nothing else is drawing.
    purr_kernel_ui_unlock();

    // PHASE 2 — everything else, BOUNDED.
    //
    // Game mode is the first thing in this OS to call deinit() at runtime, so
    // every one of these paths is effectively unexercised. proximity_deinit()
    // proved the point: it deletes its own task and only THEN unregisters its
    // ESP-NOW callbacks, so the task can be killed mid-callback holding an
    // internal lock that esp_now_deinit() then waits on forever — hanging the
    // whole transition with the UI already gone.
    //
    // One badly-behaved deinit must not be able to wedge the device. A module
    // that overruns is logged and left loaded; game mode continues with less
    // memory freed, which is strictly better than never returning.
    for (int i = 0; i < s_suspended_n; i++) {
        if (s_unloaded[i]) continue;   // already done in phase 1
        purr_splash_status(s_suspended[i]);
        size_t before_one = free_internal();
        if (purr_kernel_run_bounded("gm_unload", unload_one,
                                     s_suspended[i], GAME_MODE_UNLOAD_TIMEOUT_MS)) {
            s_unloaded[i] = true;
            // Paired with the restore-side figure in exit(). A module that gives
            // back far less than it costs to bring back is the leak.
            ESP_LOGW(TAG, "[mem] unload  %-20s %6d bytes", s_suspended[i],
                     (int)free_internal() - (int)before_one);
        } else {
            // Left loaded, and left FALSE in s_unloaded so exit() does not try
            // to restore something that never came out.
            ESP_LOGW(TAG, "%s deinit did not return in %dms — leaving it loaded",
                     s_suspended[i], GAME_MODE_UNLOAD_TIMEOUT_MS);
        }
        purr_splash_advance();
    }

    size_t after = free_internal();
    ESP_LOGW(TAG, "entered: %d modules unloaded, internal DRAM %u -> %u (+%d bytes)",
             s_suspended_n, (unsigned)before, (unsigned)after, (int)(after - before));

    purr_splash_status(label ? label : "loading");
    purr_splash_advance();

    // NOTE: the UI lock was already released after phase 1 — it must be, so
    // phase 2's helper task can take it. Do not add a second unlock here.
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
    //
    // Per-module internal-DRAM accounting (TEMPORARY — remove once the leak
    // below is closed). A round trip loses roughly 9KB of internal DRAM
    // permanently: measured 40619 -> 23087 -> 14071 free across three runs, and
    // the third run exhausts the device and panics. Entry frees only ~4.2KB for
    // 12 modules while exit costs ~9KB, so unload/reload is not symmetric —
    // some deinit() does not release what its init() allocated.
    //
    // Which one is not guessable from the outside, so measure it: log what each
    // module costs to bring back. Anything reporting far more than it should is
    // the culprit. Logged at WARN so it survives the default log level.
    int restored = 0;
    for (int i = s_suspended_n - 1; i >= 0; i--) {
        if (!s_unloaded[i]) continue;   // never came out; nothing to put back
        purr_splash_status(s_suspended[i]);
        size_t before = free_internal();
        if (purr_kernel_enable_static_module(s_suspended[i]) == 0) restored++;
        else ESP_LOGW(TAG, "failed to restore %s", s_suspended[i]);
        size_t after = free_internal();
        ESP_LOGW(TAG, "[mem] restore %-20s %6d bytes", s_suspended[i],
                 (int)before - (int)after);
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
