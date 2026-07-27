// purr_crash_guard.c — see purr_crash_guard.h for the full design rationale.

#include "purr_crash_guard.h"
#include "purr_kernel.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "crash_guard";

#define NVS_NS            "purr_crash"
#define NVS_KEY_BREADCRUMB "cur"
#define NVS_KEY_PENDING_RECOVERY "rec"
#define STRIKE_THRESHOLD  5

// Entity names come from purr_module_header_t.name (char[32]) or
// app_entry_t.name (char[48]) — both well over NVS's 15-char key limit, so
// per-entity records are keyed by a hash instead of the name itself. The
// name is also stored inside the record (for the panic screen / logging),
// so nothing is lost by not using it as the key directly.
typedef struct {
    char    name[32];
    uint8_t count;
    uint8_t disabled;
    // Last reason string this entity was struck for — persisted so a
    // recovery panic on the *next* boot (purr_crash_guard_check_reset_
    // reason()) can show the real, specific reason instead of always
    // falling back to a generic "unclean reset / hard crash". Confirmed
    // live as worth having: a first debugging pass on a hang had nothing
    // more specific than "UI TASK UNRESPONSIVE" to go on even after a
    // reboot, whereas a breadcrumbed reason (see purr_kernel_ui_
    // breadcrumb()) survives here and actually says where.
    char    reason[64];
} entry_t;

static uint32_t hash_name(const char *s)
{
    uint32_t h = 2166136261u;   // FNV-1a
    while (s && *s) { h ^= (uint8_t)(*s++); h *= 16777619u; }
    return h;
}

static void entity_key(const char *entity_name, char *out, size_t out_sz)
{
    snprintf(out, out_sz, "e%08lx", (unsigned long)hash_name(entity_name));
}

static bool load_entry(const char *entity_name, entry_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    char key[16];
    entity_key(entity_name, key, sizeof(key));
    size_t sz = sizeof(*out);
    esp_err_t err = nvs_get_blob(h, key, out, &sz);
    nvs_close(h);
    return err == ESP_OK && sz == sizeof(*out);
}

static void save_entry(const char *entity_name, const entry_t *in)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char key[16];
    entity_key(entity_name, key, sizeof(key));
    nvs_set_blob(h, key, in, sizeof(*in));
    nvs_commit(h);
    nvs_close(h);
}

static void clear_breadcrumb(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_BREADCRUMB, "");
    nvs_commit(h);
    nvs_close(h);
}

// ── Pending-recovery marker ──────────────────────────────────────────────
//
// A genuine hang (force_panic path below) reboots via purr_kernel_reboot()
// (esp_restart()), which produces a *clean* ESP_RST_SW reset reason next
// boot — NOT one of the "unclean" reasons purr_crash_guard_check_reset_
// reason() already looks for. Without this separate marker, the very
// reboot this guard triggers would look like an ordinary clean boot and
// the reason would never surface. Same NVS namespace/blob pattern as
// entry_t above, just a fixed key instead of a per-entity hash — there's
// only ever one pending recovery at a time (this boot's, if any).
typedef struct {
    char name[32];
    char reason[64];
} pending_recovery_t;

static void save_pending_recovery(const char *entity_name, const char *reason)
{
    pending_recovery_t p = {0};
    strncpy(p.name, entity_name ? entity_name : "", sizeof(p.name) - 1);
    strncpy(p.reason, reason ? reason : "", sizeof(p.reason) - 1);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_PENDING_RECOVERY, &p, sizeof(p));
    nvs_commit(h);
    nvs_close(h);
}

bool purr_crash_guard_pending_recovery(char *name_out, size_t name_sz,
                                        char *reason_out, size_t reason_sz)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    pending_recovery_t p = {0};
    size_t sz = sizeof(p);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_PENDING_RECOVERY, &p, &sz);
    nvs_close(h);
    if (err != ESP_OK || sz != sizeof(p) || p.name[0] == '\0') return false;
    if (name_out && name_sz) {
        strncpy(name_out, p.name, name_sz - 1);
        name_out[name_sz - 1] = '\0';
    }
    if (reason_out && reason_sz) {
        strncpy(reason_out, p.reason, reason_sz - 1);
        reason_out[reason_sz - 1] = '\0';
    }
    return true;
}

void purr_crash_guard_clear_pending_recovery(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, NVS_KEY_PENDING_RECOVERY);
    nvs_commit(h);
    nvs_close(h);
}

// Increments entity_name's strike count, persists it, and — once the
// threshold is hit (or force_panic is set, for the hang case) — shows the
// blue panic screen. purr_kernel_panic_ex() is noreturn when it actually
// renders/loops, so callers below never return past this in that case.
static void record_strike_and_maybe_panic(const char *entity_name, const char *reason, bool force_panic)
{
    if (!entity_name || !entity_name[0]) return;

    entry_t e;
    if (!load_entry(entity_name, &e)) {
        memset(&e, 0, sizeof(e));
        strncpy(e.name, entity_name, sizeof(e.name) - 1);
    }
    if (e.count < 255) e.count++;
    if (e.count >= STRIKE_THRESHOLD) e.disabled = 1;
    strncpy(e.reason, reason ? reason : "", sizeof(e.reason) - 1);
    e.reason[sizeof(e.reason) - 1] = '\0';
    save_entry(entity_name, &e);

    ESP_LOGW(TAG, "strike %u/%d for '%s': %s", e.count, STRIKE_THRESHOLD,
             entity_name, reason ? reason : "");

    if (force_panic) {
        // A genuine hang — the task that would need to render a panic
        // screen may itself be the thing wedged on the same shared SPI
        // bus the display uses (confirmed live: display/radio/SD all sit
        // on tdeck_plus's SPI2_HOST). purr_kernel_panic_ex() below would
        // call into the display driver to draw the blue screen — if THAT
        // call also hangs, the device is left fully dead with zero
        // user-visible feedback and no way back short of a manual power
        // cycle, which is the actual bug being fixed here. So: never
        // attempt to render for the hang case. Persist the reason (NVS
        // only, no SPI/display touch — safe even mid-wedge) and reboot
        // outright; purr_kernel_reboot() -> esp_restart() performs a full
        // digital-core reset, which resets the wedged SPI2 peripheral's
        // hardware state in lockstep with clearing all software/driver
        // bookkeeping — the only combination that reliably un-sticks a
        // task parked inside a blocking SPI call with no software
        // timeout. The next boot's kernel_tdp_boot.c reads this back via
        // purr_crash_guard_pending_recovery() to stage bring-up and
        // surface the reason once it's actually safe to touch the bus
        // again.
        char panic_reason[128];
        snprintf(panic_reason, sizeof(panic_reason), "%.32s: %.64s (strike %u/%d)",
                 entity_name, reason ? reason : "unknown", e.count, STRIKE_THRESHOLD);
        save_pending_recovery(entity_name, panic_reason);
        purr_kernel_reboot();
        // noreturn in practice — purr_kernel_reboot() delays 100ms then
        // calls esp_restart().
        return;
    }

    if (e.count >= STRIKE_THRESHOLD) {
        char panic_reason[128];
        snprintf(panic_reason, sizeof(panic_reason), "%.32s: %.64s (strike %u/%d)",
                 entity_name, reason ? reason : "unknown", e.count, STRIKE_THRESHOLD);
        purr_kernel_panic_ex(panic_reason, /*recoverable=*/true, entity_name);
        // noreturn in practice — purr_kernel_panic_ex() loops until the
        // user forces a reset.
    }
}

void purr_crash_guard_mark_start(const char *entity_name)
{
    if (!entity_name) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_BREADCRUMB, entity_name);
    nvs_commit(h);
    nvs_close(h);
}

void purr_crash_guard_mark_stop(const char *entity_name, bool ok, const char *reason)
{
    clear_breadcrumb();
    if (ok) return;
    record_strike_and_maybe_panic(entity_name, reason, /*force_panic=*/false);
}

// ── Safe worker task for mark_hang() ────────────────────────────────────────
//
// mark_start()/mark_stop()/is_disabled() are only ever called from confirmed
// internal-RAM-stack tasks (app_manager_stop(), the static module loader,
// launch_native()/launch_meow() — all driven from the active UI backend's
// own pump task, which uses a plain internal-stack xTaskCreate()), so they
// touch NVS directly on the caller's own stack.
//
// mark_hang() is different: it's reachable from genuinely anywhere,
// including MiniWin's own MW_ASSERT handler
// (hal/purr_os/miniwin_debug_purr.c) — which can itself fire on a
// PSRAM-backed stack (e.g. a looping .meow script's meow_task, calling
// purr_win_*() into MiniWin's internals). Touching flash/NVS briefly
// disables the cache; a PSRAM-stack task continuing to execute through
// that window is a confirmed hard crash
// (esp_task_stack_is_sane_cache_disabled()) on this codebase — live,
// during exactly this scenario, before this fix existed. So mark_hang()
// never touches NVS on the caller's own stack: it hands off to a
// dedicated, statically-allocated (guaranteed internal-RAM) worker task
// and parks the caller — whose own state is suspect by definition once
// this is called anyway, so it must not continue running normally, but
// must also never touch flash itself again.

#define WORKER_STACK_WORDS 8192   // matches app_manager.c's STATIC_STACK_SIZE precedent for flash-touching static-stack tasks
static StackType_t  s_worker_stack[WORKER_STACK_WORDS];
static StaticTask_t s_worker_tcb;
static QueueHandle_t s_worker_queue = NULL;

typedef struct {
    char entity_name[32];
    char reason[64];
} hang_msg_t;

static void worker_task(void *arg)
{
    (void)arg;
    hang_msg_t msg;
    while (1) {
        if (xQueueReceive(s_worker_queue, &msg, portMAX_DELAY) == pdTRUE) {
            clear_breadcrumb();
            record_strike_and_maybe_panic(msg.entity_name, msg.reason, /*force_panic=*/true);
            // noreturn in practice — purr_kernel_panic_ex() loops until the
            // user forces a reset. Nothing after this point ever runs.
        }
    }
}

static void ensure_worker_started(void)
{
    if (!s_worker_queue) s_worker_queue = xQueueCreate(4, sizeof(hang_msg_t));
    static bool started = false;
    if (!started && s_worker_queue) {
        xTaskCreateStatic(worker_task, "crash_wd", WORKER_STACK_WORDS, NULL, 10,
                           s_worker_stack, &s_worker_tcb);
        started = true;
    }
}

void purr_crash_guard_mark_hang(const char *entity_name, const char *reason)
{
    ensure_worker_started();
    hang_msg_t msg = {0};
    strncpy(msg.entity_name, entity_name ? entity_name : "?", sizeof(msg.entity_name) - 1);
    strncpy(msg.reason, reason ? reason : "", sizeof(msg.reason) - 1);
    if (s_worker_queue) {
        xQueueSend(s_worker_queue, &msg, portMAX_DELAY);
    }
    // Park here — see the header comment above for why this task must not
    // continue running normally, nor touch flash itself.
    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}

bool purr_crash_guard_is_disabled(const char *entity_name)
{
    if (!entity_name) return false;
    entry_t e;
    if (!load_entry(entity_name, &e)) return false;
    return e.disabled != 0;
}

// Wipe every strike when the running firmware is not the one that earned them.
//
// Strikes are evidence about a specific build. A new build has, by definition,
// not crashed yet — carrying counts across a reflash means the guard punishes
// code that no longer exists, and it is the FIX that gets blamed.
//
// This is not a theoretical tidiness point. During the DP8 game-mode work this
// counter reached 13/5 and halted the kernel at boot regardless of
// configuration, three separate times in one session. Each recovery needed
// `esptool erase_region 0x9000 0x6000` — knowledge that lives nowhere in the
// device, so an unlucky user simply has a device that no longer boots. It also
// made a boot loop after any change look like the change had caused it, which
// cost a full misdiagnosis (see DP8_CHECKLIST F19's note on the systemui style).
//
// Keyed on the app's ELF SHA-256, not a version string: version strings are
// hand-edited and stay identical across the dozens of builds a debugging session
// produces, which is exactly when this matters most. The SHA changes on every
// recompile, and equally on a genuine OTA update — where resetting strikes is
// also the correct behaviour.
//
// Deliberately NOT erasing the whole namespace: the pending-recovery record is
// written immediately before a reboot and must survive into the next boot to be
// reported. Only strike records are cleared.
static void reset_strikes_if_new_firmware(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    if (!desc) return;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    uint8_t stored[32];
    size_t  len = sizeof(stored);
    bool    same = (nvs_get_blob(h, "fwid", stored, &len) == ESP_OK) &&
                   len == sizeof(stored) &&
                   memcmp(stored, desc->app_elf_sha256, sizeof(stored)) == 0;
    if (same) { nvs_close(h); return; }

    // New or first-seen firmware. Drop every per-entity strike record, then
    // remember this build so the next boot is a no-op.
    nvs_iterator_t it = NULL;
    int cleared = 0;
    if (nvs_entry_find("nvs", NVS_NS, NVS_TYPE_BLOB, &it) == ESP_OK) {
        while (it) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            // Strike records only — leave "fwid", the breadcrumb ("cur") and
            // the pending-recovery record ("rec") alone. entity_key() formats
            // these as "e%08lx", so the shape is a leading 'e' plus 8 hex
            // digits; matching on that rather than a bare prefix keeps a future
            // key beginning with 'e' from being swept up.
            if (info.key[0] == 'e' && strlen(info.key) == 9) {
                if (nvs_erase_key(h, info.key) == ESP_OK) cleared++;
            }
            if (nvs_entry_next(&it) != ESP_OK) break;
        }
        nvs_release_iterator(it);
    }

    nvs_set_blob(h, "fwid", desc->app_elf_sha256, sizeof(stored));
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGW(TAG, "new firmware — cleared %d strike record(s); this build starts clean",
             cleared);
}

void purr_crash_guard_check_reset_reason(void)
{
    // Before anything reads a strike count this boot.
    reset_strikes_if_new_firmware();

    // Surface the reason for a hang-triggered reboot (see
    // record_strike_and_maybe_panic()'s force_panic branch) — independent
    // of the esp_reset_reason() check below, since that reboot is a clean
    // esp_restart() and would otherwise never be attributed to anything.
    // By the time kernel_tdp_boot.c calls this (after SD mount + display
    // init in Phase 0), both have already had their bounded bring-up
    // attempt for this boot, so it's safe to touch SD/notify here even if
    // the underlying bus is still bad — purr_kernel_notify() is RAM-only,
    // and purr_kernel_panic_dump_logs() is internally gated on
    // purr_kernel_sd_available(), which correctly reflects this boot's
    // real (possibly degraded) SD state.
    char pend_name[32], pend_reason[64];
    if (purr_crash_guard_pending_recovery(pend_name, sizeof(pend_name),
                                           pend_reason, sizeof(pend_reason))) {
        ESP_LOGW(TAG, "recovered from hang: '%s': %s", pend_name, pend_reason);
        purr_kernel_notify("Recovered from crash", pend_reason, pend_name);
        purr_kernel_panic_dump_logs(pend_name, pend_reason);
    }

    esp_reset_reason_t r = esp_reset_reason();
    bool unclean = (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
                    r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
                    r == ESP_RST_BROWNOUT || r == ESP_RST_CPU_LOCKUP ||
                    r == ESP_RST_PWR_GLITCH);
    if (!unclean) {
        // Clean boot (power-on / intentional esp_restart / deep sleep
        // wake) — whatever breadcrumb is left over, if any, is stale
        // (e.g. the device was power-cycled mid-launch) and shouldn't be
        // blamed on anything.
        clear_breadcrumb();
        return;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    char name[48] = {0};
    size_t sz = sizeof(name);
    esp_err_t err = nvs_get_str(h, NVS_KEY_BREADCRUMB, name, &sz);
    nvs_close(h);
    bool have_breadcrumb = (err == ESP_OK && name[0] != '\0');

    // No breadcrumb doesn't mean "nothing to do here" for the watchdog-
    // class reasons specifically (PANIC/TASK_WDT/WDT/INT_WDT/CPU_LOCKUP —
    // deliberately excluding BROWNOUT/PWR_GLITCH, which are power events
    // with no task to blame). mesh_task/cupcake_task's own
    // esp_task_wdt_add() subscriptions (see their own comments) don't go
    // through mark_start()'s NVS breadcrumb — that's a per-iteration write
    // and neither task's loop is a place to add flash wear/latency to. A
    // TASK_WDT trip on THIS device only ever means one of those two tasks,
    // so a generic "watchdog" fallback name is used instead of silently
    // returning and losing the whole recovery signal.
    bool watchdog_class = (r == ESP_RST_PANIC || r == ESP_RST_TASK_WDT ||
                           r == ESP_RST_WDT || r == ESP_RST_INT_WDT ||
                           r == ESP_RST_CPU_LOCKUP);
    if (!have_breadcrumb && !watchdog_class) return;

    const char *entity = have_breadcrumb ? name : "watchdog";
    ESP_LOGW(TAG, "unclean reset (reason=%d) while '%s' was active", (int)r, entity);

    // Prefer whatever specific reason this entity was already struck for
    // (e.g. a breadcrumbed "UI TASK UNRESPONSIVE @ step" from mark_hang())
    // over the generic fallback — makes the *next* boot's recovery panic
    // say where, not just that something crashed.
    entry_t prev;
    const char *reason = have_breadcrumb ? "unclean reset / hard crash"
                                          : "task watchdog panic (see console backtrace)";
    if (have_breadcrumb && load_entry(entity, &prev) && prev.reason[0]) reason = prev.reason;

    // Previously only record_strike_and_maybe_panic()'s force_panic branch
    // (mark_hang()'s live-detection path) saved this — meaning a hang
    // caught here instead (after the fact, via reset-reason inspection,
    // which is the ONLY way a TWDT/panic reset can be attributed at all —
    // there's no hook to run our own code before that kind of reset
    // happens) left the *next* boot completely unprotected, running
    // Phase 0's SD/display bring-up unbounded right after a boot that just
    // proved the shared SPI2 bus can wedge. Confirmed live tonight: this
    // exact gap let a hang-reboot-hang cascade run past what crash_guard's
    // own bookkeeping could see. Saving it here closes that gap for both
    // the breadcrumb-attributed case and the generic watchdog fallback.
    save_pending_recovery(entity, reason);
    record_strike_and_maybe_panic(entity, reason, /*force_panic=*/false);
    clear_breadcrumb();
}
