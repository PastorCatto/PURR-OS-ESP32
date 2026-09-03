// app_manager.c — PURR OS app manager

#include "app_manager.h"
#include "speed_demon.h"
#include "user_mgr.h"
#include "sig_mgr.h"
#include "claw_loader.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_crash_guard.h"
#include "../../kernel/catcalls/purr_win.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "app_mgr";

#define MAX_APPS 64

static const char *s_scan_paths[] = {
    "/flash/apps",
    "/sdcard/apps",
    NULL
};

// PSRAM-backed instead of a plain static array — app_entry_t's path[256]/
// error[96]/name[48] make this ~26KB for MAX_APPS=64 entries, which was the
// single largest static consumer of this board's scarce internal SRAM after
// Cupcake's display buffers and this file's own static launch stacks (see
// the memory-pressure investigation this was found in). Pure data, no DMA/
// ISR access, so PSRAM is safe — allocated once in app_manager_init(),
// never freed (lives for the device's whole runtime, same as a static would
// have).
static app_entry_t *s_apps;
static int          s_app_count = 0;

// app_manager_on_window_created() (see below) needs to know which app is
// currently inside its synchronous init() call, to attribute a
// purr_win_create() call to the right app_entry_t without every app needing
// to report its own window handle. This USED to be a single shared global
// (s_launching_app), set right before calling init() and cleared right
// after — but each app launches on its own FreeRTOS task, and with the
// ESP32-S3 being dual-core, two launches close together in time (e.g. two
// quick taps) can genuinely run their init() calls concurrently on the two
// cores. The second task's assignment stomped the first one mid-flight,
// silently attributing the first app's window to the second app's entry —
// the first app's ->window stayed 0 forever, and re-tapping its icon later
// hit the "already running, just re-show the tracked window" path on a
// handle that was never actually set. Nothing visible ever happened.
//
// Fixed by keying off the calling task's own handle instead of a shared
// global — see find_app_by_current_task() below. xTaskGetCurrentTaskHandle()
// is inherently race-free (it only ever returns the caller's own identity),
// and s_ctxs[idx].task is written by xTaskCreate() in the ORIGINAL launching
// context, before the new task can possibly start running — so by the time
// that task calls purr_win_create(), its own ctx->task entry is guaranteed
// to already be correct, regardless of how many other launches overlap.

// ── Tier detection ─────────────────────────────────────────────────────────────

static app_tier_t tier_from_ext(const char *filename)
{
    const char *ext = strrchr(filename, '.');
    if (!ext) return APP_TIER_PAWS;
    if (strcmp(ext, ".meow") == 0) return APP_TIER_MEOW;
    if (strcmp(ext, ".hiss") == 0) return APP_TIER_HISS;
    if (strcmp(ext, ".claw") == 0) return APP_TIER_CLAW;
    if (strcmp(ext, ".kitten") == 0) return APP_TIER_KITTEN;
    return APP_TIER_PAWS;   // .paws and anything else
}

static const char *tier_name(app_tier_t t)
{
    switch (t) {
    case APP_TIER_MEOW:   return "meow";
    case APP_TIER_HISS:   return "hiss";
    case APP_TIER_PAWS:   return "paws";
    case APP_TIER_CLAW:   return "claw";
    case APP_TIER_KITTEN: return "kitten";
    case APP_TIER_PERSONAL: return "personal";
    default:              return "?";
    }
}

// ── Placement declarations ───────────────────────────────────────────────
// app.pcat's `placement = "local" | "hybrid"` key (default "remote",
// unset) is where this is AUTHORED — but a compiled/pre-linked app has
// no runtime-readable path back to its own app.pcat: purr_module_header_t
// (purr_module.h) just spent its last spare pad byte on speed_demon, and
// growing it is a real ABI change across every already-compiled artifact
// including claw_loader's own runtime-loaded .claw format, so it wasn't
// grown for this. Hand-maintained here instead, by display name, until a
// real catstrap codegen step exists to generate this table FROM app.pcat
// automatically — a real, deliberate simplification for this pass, not
// an oversight: every app not listed here keeps the APP_PLACE_REMOTE
// default (see app_placement_t's own doc comment) with zero risk of
// drifting from what's actually compiled in.
static const struct { const char *name; app_placement_t placement; } s_placement_table[] = {
    // { "some_app", APP_PLACE_HYBRID },
};

static app_placement_t declared_placement_for(const char *name)
{
    // Table starts empty (see its own comment) — GCC constant-folds an
    // inline sizeof(arr)/sizeof(arr[0]) loop bound of 0 and flags the
    // resulting `i < 0`-shaped comparison (-Wtype-limits); a named
    // `count` local sidesteps that without changing anything once the
    // table actually has entries.
    size_t count = sizeof(s_placement_table) / sizeof(s_placement_table[0]);
    for (size_t i = 0; i < count; i++) {
        if (strcmp(s_placement_table[i].name, name) == 0) return s_placement_table[i].placement;
    }
    return APP_PLACE_REMOTE;
}

// Strip extension and path prefix to get a display name
static void make_display_name(const char *filename, char *out, size_t out_sz)
{
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    strncpy(out, base, out_sz - 1);
    out[out_sz - 1] = '\0';
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
}

// ── Scan ──────────────────────────────────────────────────────────────────────

// Returns the s_apps[] index already holding this display name, or -1.
static int find_app_slot_by_name(const char *name) {
    for (int i = 0; i < s_app_count; i++) {
        if (strncmp(s_apps[i].name, name, sizeof(s_apps[i].name)) == 0) {
            return i;
        }
    }
    return -1;
}

static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *ext = strrchr(ent->d_name, '.');
        if (!ext) continue;
        if (strcmp(ext, ".meow") != 0 &&
            strcmp(ext, ".hiss") != 0 &&
            strcmp(ext, ".paws") != 0 &&
            strcmp(ext, ".claw") != 0 &&
            strcmp(ext, ".kitten") != 0) continue;

        char name[sizeof(((app_entry_t *)0)->name)];
        make_display_name(ent->d_name, name, sizeof(name));

        // Later scan paths shadow earlier ones — including a pre-linked app
        // of the same name — per docs/06_Apps.md ("SD apps with the same
        // name as a flash app shadow the flash version"). Previously every
        // path was appended unconditionally, so a same-named app on both
        // /flash/apps and /sdcard/apps produced two confusing entries in the
        // launcher instead of the SD copy taking over.
        int existing = find_app_slot_by_name(name);
        app_entry_t *app;
        if (existing >= 0) {
            app = &s_apps[existing];
        } else {
            if (s_app_count >= MAX_APPS) {
                ESP_LOGW(TAG, "app table full — skipping %s/%s", dir, ent->d_name);
                continue;
            }
            app = &s_apps[s_app_count++];
        }
        memset(app, 0, sizeof(*app));

        snprintf(app->path, sizeof(app->path), "%s/%.200s", dir, ent->d_name);
        strncpy(app->name, name, sizeof(app->name) - 1);
        app->tier      = tier_from_ext(ent->d_name);
        app->state     = APP_STATE_IDLE;
        app->placement = declared_placement_for(app->name);

        if (existing >= 0) {
            ESP_LOGI(TAG, "found [%s] %s (shadows earlier entry)", tier_name(app->tier), app->name);
        } else {
            ESP_LOGI(TAG, "found [%s] %s", tier_name(app->tier), app->name);
        }
    }
    closedir(d);
}

// ── Per-app task context ──────────────────────────────────────────────────────

typedef struct {
    app_entry_t            *app;
    const purr_module_header_t *mod;      // non-null for pre-linked native apps
    TaskHandle_t            task;
    SemaphoreHandle_t       done;
    // Which deletion function this task's own self-delete (and
    // app_manager_stop()'s force-delete path) must use — see the static
    // stack pool comment below for why this can't be a single constant.
    bool                     static_stack;
} app_task_ctx_t;

// Also PSRAM-backed (see s_apps' matching comment) — small per-entry
// (pointers/handles/one bool), but no reason to leave even ~1KB behind on
// internal SRAM when it's just as safe in PSRAM.
static app_task_ctx_t *s_ctxs;

// ── Remote mode ─────────────────────────────────────────────────────────────
// See app_manager.h's own doc comment on app_manager_set_remote() for the
// design (provider-injection, no proximity_rpc/pairing dependency here).
// This is a SECOND, separate registry (s_remote_apps, own count) — the local
// one (s_apps/s_app_count) is never touched while remote mode is on, so
// app_manager_clear_remote() needs no re-scan to bring it back.
#define MAX_REMOTE_APPS      32
#define REMOTE_REFRESH_MS    3000

static bool          s_remote_mode  = false;
static uint8_t        s_remote_mac[6];
static app_manager_remote_provider_t s_remote_provider;
static app_entry_t   *s_remote_apps;        // PSRAM-backed, allocated lazily on first use
static int             s_remote_app_count = 0;
static TaskHandle_t    s_remote_task = NULL;
static volatile bool   s_remote_task_running = false;
static SemaphoreHandle_t s_remote_task_done = NULL;

// Defined near the bottom of this file, alongside the rest of the remote-
// mode implementation — forward-declared here because app_manager_launch_
// idx()/app_manager_stop() (both defined well above that section) need it.
static bool remote_launch_stop_task(int idx, bool launch);

// ── Local unlock gate ─────────────────────────────────────────────────────
// See app_manager.h's own doc comment on app_manager_notify_unlocked() for
// the design. Only app_manager_count()/get()'s LOCAL branch (local_count()/
// local_get() below) read this — launch_idx()/launch_path()/launch_by_name()
// deliberately do NOT gate on it, since autorun_oobe()/autorun_kitten() (both
// called from app_manager_init(), before any unlock) and the synthetic
// "Server Manager" entry's app_manager_launch_by_name("server_manager") call
// (from inside REMOTE mode, a wholly separate axis from local unlock) both
// need to keep working regardless of this flag's state.
static bool s_local_unlocked = false;

// Forward-declared here (real definitions much further down, alongside
// local_count()/get()) because local_launch_idx()/local_stop_idx() below
// need translate_local_idx() too — every UI-facing LOCAL index (from
// app_manager_launch_idx()/stop(), always ultimately driven by whatever
// app_manager_count()/get() reported) must be translated the SAME way
// local_get() translates one, or "the Nth icon shown" and "the Nth real
// s_apps[] entry" silently drift apart the moment any entry is hidden
// from the former without also being skipped by the latter. Confirmed
// live as a real, reported bug: local_launch_idx() indexed s_apps[idx]
// directly while local_count()/get() had already started skipping
// "server_manager" — tapping what LOOKED like Settings launched whatever
// real entry happened to sit at that same raw offset instead.
static bool is_hidden_local_app(const char *name);
static int  translate_local_idx(int idx);

// ── Static stack pool for apps that touch NVS/flash directly ────────────────
// See launch_native()'s comment for the full story: settings/fileman must run
// with an internal-RAM stack (PSRAM stacks crash when their task disables
// cache for a flash/NVS op), but a *dynamic* internal allocation competes with
// whatever else has fragmented internal DRAM by the time the user taps the
// icon — confirmed live to fail within a session even at just 8KB. A
// dedicated static buffer per app sidesteps runtime fragmentation entirely:
// its address and size are fixed at link time, not requested from the heap.
#define STATIC_STACK_SIZE 8192
static StackType_t  s_static_stack_settings[STATIC_STACK_SIZE];
static StaticTask_t s_static_tcb_settings;
static StackType_t  s_static_stack_fileman[STATIC_STACK_SIZE];
static StaticTask_t s_static_tcb_fileman;
// milkbar joined this list once milkbar_app_init() started calling
// purr_app_config_read() directly (loading /config/milkbar.cfg's remembered
// device selection) — confirmed live crash without this: "assert failed:
// spi_flash_disable_interrupts_caches_and_other_cpu ...
// (esp_task_stack_is_sane_cache_disabled())", same class this comment
// already describes for settings/fileman, just newly true for a third app.
static StackType_t  s_static_stack_milkbar[STATIC_STACK_SIZE];
static StaticTask_t s_static_tcb_milkbar;

// Every out-of-memory launch failure below funnels through here so the user
// actually finds out why nothing happened, instead of a silent no-op —
// which is exactly what the original "apps never open" bug looked like
// before any of tonight's fixes.
static void report_launch_oom(app_entry_t *app)
{
    app->state = APP_STATE_ERROR;
    snprintf(app->error, sizeof(app->error), "out of memory");
    purr_kernel_notify("Low memory",
                        "Too much open right now — close something and try again.",
                        "app_mgr");
}

// ── Lua VM dispatch ───────────────────────────────────────────────────────────
//
// .meow apps run via the lua_runtime module. That module must be loaded first
// and register itself under the name "lua_runtime". We find it in the kernel
// module registry and call its init() which bootstraps the VM for one script.
// The path of the script is passed via a thin task-arg struct written into NVS
// (or a global, since only one Lua VM runs at a time on these boards).

static char   s_meow_pending_path[128];
// Script source preloaded into PSRAM by launch_meow() — see its comment and
// app_manager.h's accessor doc for why this exists instead of meow_task()
// calling fopen() itself.
static char   *s_meow_pending_code = NULL;
static size_t  s_meow_pending_len  = 0;
// True when the pending script is .hiss-tier — read by lua_runtime_init()
// to decide whether to register kitt.*/radio.*/gps.*. Set in launch_meow().
static bool    s_meow_pending_privileged = false;

static void meow_task(void *arg) {
    app_task_ctx_t *ctx = (app_task_ctx_t *)arg;
    const purr_module_header_t *lua_rt = purr_kernel_get_module("lua_runtime");
    if (!lua_rt || !lua_rt->init) {
        ESP_LOGE(TAG, "lua_runtime module not loaded — cannot run .meow");
        ctx->app->state = APP_STATE_ERROR;
        snprintf(ctx->app->error, sizeof(ctx->app->error), "lua_runtime not loaded");
        if (s_meow_pending_code) { heap_caps_free(s_meow_pending_code); s_meow_pending_code = NULL; s_meow_pending_len = 0; }
        xSemaphoreGive(ctx->done);
        vTaskDeleteWithCaps(NULL);
        return;
    }
    // lua_runtime.init() picks up the preloaded buffer via
    // app_manager_get_pending_meow_code() (falls back to
    // app_manager_get_pending_meow_path() only if no buffer is pending).
    int rc = lua_rt->init();
    // The buffer's job is done the instant init() returns — lua_run_code()
    // compiles it into Lua bytecode via luaL_loadbuffer() and doesn't retain
    // the raw source afterward, whether the script ran clean or errored out.
    if (s_meow_pending_code) { heap_caps_free(s_meow_pending_code); s_meow_pending_code = NULL; s_meow_pending_len = 0; }
    // Only mark ERROR on failure — a successful init() means the script is up
    // and running, not finished. Previously this immediately overwrote the
    // RUNNING state set at launch with STOPPED the instant init() returned,
    // which for every app here is almost instantly — so the Running Apps
    // list and the re-launch guard below never actually worked. state now
    // only becomes STOPPED via the explicit app_manager_stop().
    if (rc != 0) {
        ctx->app->state = APP_STATE_ERROR;
        snprintf(ctx->app->error, sizeof(ctx->app->error), "lua_runtime init failed (%d)", rc);
    }
    xSemaphoreGive(ctx->done);
    vTaskDeleteWithCaps(NULL);
}

// The old scan_purr_sig() honor-system tag scanner lived here — replaced
// by real Ed25519 verification, see the sig_mgr_classify_buffer() call in
// launch_meow() below and sig_mgr.h for the tier model. catstrap.py's own
// build-time reads of the same tag are a separate, still-open cleanup
// (they never gated anything on-device — this was always the copy that
// mattered).

static int launch_meow(app_entry_t *app, int idx)
{
    if (purr_crash_guard_is_disabled(app->name)) {
        app->state = APP_STATE_ERROR;
        snprintf(app->error, sizeof(app->error), "disabled after repeated crashes");
        ESP_LOGW(TAG, "app '%s' disabled after repeated crashes — not launching", app->name);
        return -1;
    }

    const purr_module_header_t *lua_rt = purr_kernel_get_module("lua_runtime");
    if (!lua_rt) {
        app->state = APP_STATE_ERROR;
        snprintf(app->error, sizeof(app->error), "lua_runtime module not loaded");
        ESP_LOGW(TAG, "cannot run .meow '%s': lua_runtime not in module registry", app->name);
        return -1;
    }

    strncpy(s_meow_pending_path, app->path, sizeof(s_meow_pending_path) - 1);
    // .kitten is "like a .hiss" — same privileged kitt.*/radio.*/gps.* Lua
    // namespace access, not just the same file-discovery/launch treatment.
    s_meow_pending_privileged = (app->tier == APP_TIER_HISS || app->tier == APP_TIER_KITTEN);

    // Preload the script into a PSRAM buffer here, on this function's own
    // caller's stack — for the only launch path that exists today (a
    // launcher tap), that's the UI backend's own dispatch task
    // (e.g. Cupcake's cupcake_task), which already runs on a static
    // internal-RAM stack for exactly this reason (see cupcake_module.c).
    // fopen()/fread() briefly disable the flash cache; doing that here
    // instead of inside meow_task() means meow_task() never touches flash
    // at all and can safely run on an ordinary PSRAM stack below — a
    // PSRAM-stack meow_task() calling fopen() directly was confirmed live to
    // crash with esp_task_stack_is_sane_cache_disabled(), the same class
    // launch_native()'s "EXCEPTION" comment documents for settings/fileman.
    if (s_meow_pending_code) {
        heap_caps_free(s_meow_pending_code);
        s_meow_pending_code = NULL;
        s_meow_pending_len  = 0;
    }
    FILE *f = fopen(app->path, "rb");
    if (!f) {
        app->state = APP_STATE_ERROR;
        snprintf(app->error, sizeof(app->error), "cannot open script");
        // DMA pool numbers correlate this with the boot-time "esp_dma_capable_
        // malloc(172): Not enough heap memory" -> "sdmmc_read_blocks failed"
        // pattern (see kernel_tdp_boot.c's phase-2 DMA snapshot) — fopen()/
        // fread() of an /sdcard script goes through the same diskio_sdmmc
        // path, and its per-transaction DMA-capable scratch buffer can fail
        // the exact same way well after boot, whenever this reserved pool
        // is contended. TEMPORARY diagnostic, same as purr_kernel.c's
        // heapwatch — remove once the SD/DMA contention issue is found.
        ESP_LOGE(TAG, "launch .meow: fopen failed for '%s' (dma_free=%u largest_dma=%u internal_free=%u)",
                 app->path,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        app->state = APP_STATE_ERROR;
        snprintf(app->error, sizeof(app->error), "empty or unreadable script");
        return -1;
    }
    char *code = heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_SPIRAM);
    if (!code) {
        fclose(f);
        ESP_LOGE(TAG, "launch .meow: PSRAM alloc failed for '%s' (%ld bytes)", app->name, sz);
        report_launch_oom(app);
        return -1;
    }
    size_t nread = fread(code, 1, (size_t)sz, f);
    fclose(f);
    code[nread] = '\0';

    // A short read here previously went completely unnoticed — the script
    // would silently run truncated instead of erroring. Same DMA-pool
    // contention this file's fopen() failure branch above documents can
    // fail *mid-read* instead of on open, which fread() surfaces as
    // nread < sz rather than a NULL return. TEMPORARY diagnostic (matches
    // the fopen-failure log above) — remove once the SD/DMA contention
    // issue is found, or promote to a real error+retry if this fires.
    if (nread != (size_t)sz) {
        ESP_LOGW(TAG, "launch .meow: short read for '%s' (%u/%ld bytes, dma_free=%u largest_dma=%u internal_free=%u)",
                 app->name, (unsigned)nread, sz,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }

    // Signature classification — sig_mgr_classify_buffer() hashes `code`
    // directly rather than re-reading app->path from SD a second time (see
    // sig_mgr.h's own doc comment on why classify_buffer() exists). Real
    // Ed25519 verification now, replacing the old scan_purr_sig() honor-
    // system string tag — see sig_mgr.h for the tier model.
    //
    // TAMPERED is hard-blocked unconditionally, for EVERY .meow/.hiss/
    // .kitten launch, dev mode or not: a .sig + co-located .pub that fails
    // to verify means the artifact changed after someone signed it, which
    // is a real integrity violation, not the "never got signed" case the
    // Developer Mode gate below exists for.
    //
    // The Developer Mode gate itself keeps its EXACT prior scope —
    // .hiss/.kitten only, only blocks SIG_TIER_UNSIGNED — deliberately not
    // widened to .meow here; that would be a real policy change, not a
    // hardening of this one check. .kitten needs it even more than .hiss
    // does: app_manager_init() autoruns the first .kitten found on the SD
    // card at *every* boot with no user interaction at all, so without
    // this gate an unsigned .kitten dropped onto the card would silently
    // get full kitt.*/radio.*/gps.* privilege on every power-on.
    sig_tier_t sig = sig_mgr_classify_buffer((const uint8_t *)code, nread, app->path);
    if (sig == SIG_TIER_TAMPERED ||
        ((app->tier == APP_TIER_HISS || app->tier == APP_TIER_KITTEN) &&
         sig == SIG_TIER_UNSIGNED && !purr_kernel_dev_mode_enabled())) {
        heap_caps_free(code);
        // Clear pending state fully (not just the code buffer) — a
        // dangling s_meow_pending_path with no matching code would make
        // lua_runtime_init()'s "nothing pending" check (empty code AND
        // empty path) false, letting a *later*, unrelated init() call
        // fall through to its fopen()-based fallback and run this exact
        // script anyway, bypassing the rejection above.
        s_meow_pending_path[0]   = '\0';
        s_meow_pending_privileged = false;
        app->state = APP_STATE_ERROR;
        if (sig == SIG_TIER_TAMPERED) {
            snprintf(app->error, sizeof(app->error), "TAMPERED — signature does not match content");
            ESP_LOGE(TAG, "launch .%s: '%s' rejected — signature present but INVALID (tampered)",
                     tier_name(app->tier), app->name);
        } else {
            snprintf(app->error, sizeof(app->error),
                     "unsigned .%s — enable Developer Mode in Settings", tier_name(app->tier));
            ESP_LOGW(TAG, "launch .%s: '%s' rejected — unsigned, Developer Mode off",
                     tier_name(app->tier), app->name);
        }
        return -1;
    }

    s_meow_pending_code = code;
    s_meow_pending_len  = nread;

    app_task_ctx_t *ctx = &s_ctxs[idx];
    ctx->app  = app;
    ctx->mod  = lua_rt;
    ctx->done = xSemaphoreCreateBinary();
    if (!ctx->done) {
        // See launch_native()'s matching check for why this is necessary.
        ESP_LOGE(TAG, "xSemaphoreCreateBinary failed for '%s' — out of memory", app->name);
        heap_caps_free(s_meow_pending_code);
        s_meow_pending_code = NULL;
        s_meow_pending_len  = 0;
        report_launch_oom(app);
        return -1;
    }

    app->mem_free_at_launch = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "launch .meow: %s (%u bytes preloaded to PSRAM)", app->path, (unsigned)nread);

    ctx->static_stack = false;
    // 8192 -> 16384: a looping .meow script (e.g. clock.meow) that calls
    // back into the Lua VM every iteration (string.format/math.floor/
    // win.* table lookups via luaV_finishget's slow path) crashed live
    // with EXCCAUSE InstructionFetchError and a CORRUPTED backtrace after
    // running for 1.4-3.5s — the classic signature of a corrupted return
    // address, i.e. stack exhaustion, not caught cleanly since this task
    // has no guard-page. This stack is PSRAM-backed (MALLOC_CAP_SPIRAM,
    // abundant — unlike this board's tight internal DRAM budget
    // documented elsewhere in this session's work), so doubling it here
    // is cheap. Scripts that build a UI and return (the documented
    // "supported" .meow pattern) never ran long enough to hit this.
    // Priority 4, not 5 — originally chosen to match cupcake_task's own
    // render-loop priority when both shared a core (avoiding this task's
    // synchronous script-run work fully preempting rendering). Now pinned
    // to core 0 while cupcake_task moves to core 1 (see that module's own
    // comment) — the two no longer contend for the same core at all, but
    // the priority is left as-is since it's still harmless.
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(meow_task, app->name, 16384, ctx, 4, &ctx->task, 0, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreateWithCaps failed for '%s' — out of PSRAM too?", app->name);
        report_launch_oom(app);
        vSemaphoreDelete(ctx->done);
        ctx->done = NULL;
        heap_caps_free(s_meow_pending_code);
        s_meow_pending_code = NULL;
        s_meow_pending_len  = 0;
        return -1;
    }
    purr_crash_guard_mark_start(app->name);
    app->state = APP_STATE_RUNNING;
    return 0;
}

// ── Native app dispatch (.paws / .claw) ───────────────────────────────────────
//
// .paws and .claw apps are compiled C that must be pre-linked into the firmware
// as .purr system modules. The app_manager looks them up in the kernel module
// registry by name (filename without extension). If the module is registered and
// running, we call init() directly on a new FreeRTOS task.
//
// True hot-loading from SD blobs requires position-independent compilation and
// an ELF relocator — tracked as future work.

// Trampoline for purr_kernel_run_bounded() — see its call site in native_task
// for why entering speed demon may not happen on that task's PSRAM stack.
static void speed_demon_enter_trampoline(void *arg) {
    purr_speed_demon_enter((const char *)arg);
}

static void native_task(void *arg) {
    app_task_ctx_t *ctx = (app_task_ctx_t *)arg;
    ESP_LOGI(TAG, "native app task start: %s (core=%d prio=%u)",
             ctx->app->name, xPortGetCoreID(), (unsigned)uxTaskPriorityGet(NULL));

    // Speed demon, if this app declared it (purr_module_header_t::speed_demon).
    //
    // Run on purr_kernel_run_bounded()'s helper task, NOT inline here. This
    // task's stack is PSRAM (xTaskCreatePinnedToCoreWithCaps with
    // MALLOC_CAP_SPIRAM, below), and entering unloads a dozen modules whose
    // deinit() paths write NVS. Writing NVS disables the flash cache, and a
    // PSRAM stack is unreachable while it is disabled — so doing it inline
    // faulted on this task's own stack:
    //
    //   assert failed: esp_task_stack_is_sane_cache_disabled()
    //   (cache_utils.c:152), stack at 0x3c21xxxx — PSRAM
    //
    // run_bounded's helper is deliberately INTERNAL-stack for exactly this
    // reason (see its own comment), which makes it the right vehicle rather
    // than a workaround. Giving native_task an internal stack instead is not an
    // option: a CLAW app asks for 16384 bytes and the largest free internal
    // block at launch time is ~15872.
    //
    // It still must not be the app's own init(): init() can run on the UI
    // render task, and entering unloads the UI backend — deleting the very
    // task making the call. Doing it centrally is also what stops an app from
    // forgetting the matching exit.
    //
    // 30s ceiling: entering nests its own 3s-per-module bounded unloads across
    // ~12 modules, so the outer bound has to clear the sum comfortably.
    if (ctx->app->speed_demon) {
        purr_kernel_run_bounded("speed_demon_enter", speed_demon_enter_trampoline,
                                (void *)ctx->app->name, 30000);
    }

    int rc = ctx->mod->init();
    ESP_LOGI(TAG, "native app task init() returned: %s rc=%d window=%u",
             ctx->app->name, rc, (unsigned)ctx->app->window);

    // Only mark ERROR on failure — see meow_task()'s comment on why this no
    // longer overwrites RUNNING with STOPPED just because the (non-blocking)
    // init() call returned.
    if (rc != 0) {
        ctx->app->state = APP_STATE_ERROR;
        snprintf(ctx->app->error, sizeof(ctx->app->error), "init() returned %d", rc);
    }
    xSemaphoreGive(ctx->done);
    // Must match how this task's stack was allocated — a statically-created
    // task's stack isn't heap memory at all, so vTaskDeleteWithCaps() (which
    // assumes a WithCaps-created task) would be operating on the wrong
    // assumption; plain vTaskDelete() is correct for both a normal dynamic
    // task and one created with xTaskCreateStatic().
    if (ctx->static_stack) vTaskDelete(NULL);
    else                   vTaskDeleteWithCaps(NULL);
}

static int launch_native(app_entry_t *app, int idx)
{
    if (purr_crash_guard_is_disabled(app->name)) {
        app->state = APP_STATE_ERROR;
        snprintf(app->error, sizeof(app->error), "disabled after repeated crashes");
        ESP_LOGW(TAG, "app '%s' disabled after repeated crashes — not launching", app->name);
        return -1;
    }

    const purr_module_header_t *mod = purr_kernel_get_module(app->name);
    if (!mod) {
        // App not pre-linked — report clearly so the user knows what to do
        app->state = APP_STATE_ERROR;
        snprintf(app->error, sizeof(app->error), "not pre-linked: %.48s", app->name);
        ESP_LOGW(TAG, "app '%s' not found in kernel module registry — it must be pre-linked into firmware", app->name);
        return -1;
    }
    if (!mod->init) {
        app->state = APP_STATE_ERROR;
        snprintf(app->error, sizeof(app->error), "module has no init()");
        return -1;
    }

    app_task_ctx_t *ctx = &s_ctxs[idx];
    ctx->app  = app;
    ctx->mod  = mod;
    ctx->done = xSemaphoreCreateBinary();
    if (!ctx->done) {
        // Under extreme internal-DRAM exhaustion this can fail too — without
        // this check, native_task()'s xSemaphoreGive(ctx->done) and this
        // function's own vSemaphoreDelete(ctx->done) further down both hit
        // FreeRTOS's own configASSERT(pxQueue) on a NULL handle and abort.
        // Confirmed live at internal=23 largest_internal_block=0.
        ESP_LOGE(TAG, "xSemaphoreCreateBinary failed for '%s' — out of memory", app->name);
        report_launch_oom(app);
        return -1;
    }

    app->mem_free_at_launch = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "launch .%s: %s (pre-linked module) — free heap: internal=%u largest_internal_block=%u",
             tier_name(app->tier), app->name,
             (unsigned)app->mem_free_at_launch,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    // Stack size: .claw apps get more stack (kernel access). Root-caused live:
    // internal DRAM is fragmented enough by boot (WiFi/BT/LoRa/LVGL buffers
    // all resident) that its largest free block was ~9KB — smaller than even
    // the 8KB .paws stack, so a plain xTaskCreate() (internal-DRAM-only)
    // failed on every single launch, every app, unconditionally. This board
    // has 8MB of otherwise-idle PSRAM, so the stack is allocated there
    // instead via xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM) for most apps —
    // must be paired with vTaskDeleteWithCaps() (used above and in
    // app_manager_stop()) rather than plain vTaskDelete().
    //
    // EXCEPTION: apps that directly touch NVS/flash (settings' nvs_load(),
    // fileman's /flash browsing) must NOT run on a PSRAM-backed stack —
    // confirmed live via a hard crash: "assert failed:
    // spi_flash_disable_interrupts_caches_and_other_cpu ...
    // (esp_task_stack_is_sane_cache_disabled())". Any flash/NVS op briefly
    // disables the cache (which is what makes PSRAM reachable at all), and
    // that assert exists specifically to catch a task whose OWN stack lives
    // in the now-unreachable PSRAM continuing to execute through it.
    //
    // A *dynamic* internal-RAM allocation for these two isn't good enough
    // either — confirmed live it can fail within the same session once
    // internal DRAM fragments below ~8KB (from other apps' LVGL widgets
    // accumulating), correctly reporting APP_STATE_ERROR instead of
    // crashing, but still failing to launch. Each gets its own dedicated
    // static stack buffer instead (declared above) — fixed at link time, so
    // it can never be starved out by runtime fragmentation.
    ctx->static_stack = (strcmp(app->name, "settings") == 0) ||
                        (strcmp(app->name, "fileman")  == 0) ||
                        (strcmp(app->name, "milkbar")  == 0);
    if (ctx->static_stack) {
        StackType_t  *stack_buf;
        StaticTask_t *tcb_buf;
        if (strcmp(app->name, "settings") == 0) {
            stack_buf = s_static_stack_settings;
            tcb_buf   = &s_static_tcb_settings;
        } else if (strcmp(app->name, "fileman") == 0) {
            stack_buf = s_static_stack_fileman;
            tcb_buf   = &s_static_tcb_fileman;
        } else {
            stack_buf = s_static_stack_milkbar;
            tcb_buf   = &s_static_tcb_milkbar;
        }
        // Priority 4 — see the meow_task creation site's comment above.
        // Pinned to core 0 alongside every other app task — see that same
        // comment for the cupcake_task/mesh_task core-1 grouping this pairs
        // with.
        ctx->task = xTaskCreateStaticPinnedToCore(native_task, app->name, STATIC_STACK_SIZE, ctx, 4, stack_buf, tcb_buf, 0);
        if (!ctx->task) {
            // Can't happen in practice (a static buffer never "runs out"),
            // but xTaskCreateStatic() can still return NULL on bad params.
            ESP_LOGE(TAG, "xTaskCreateStatic failed for '%s'", app->name);
            report_launch_oom(app);
            vSemaphoreDelete(ctx->done);
            ctx->done = NULL;
            return -1;
        }
    } else {
        uint32_t stack = (app->tier == APP_TIER_CLAW) ? 16384 : 8192;
        // Priority 4, pinned to core 0 — see the meow_task creation site's
        // comment above.
        BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(native_task, app->name, stack, ctx, 4, &ctx->task, 0, MALLOC_CAP_SPIRAM);
        if (ok != pdPASS) {
            // xTaskCreate's return value was never checked before — a silent
            // failure here left `state` stuck at RUNNING forever with no task
            // ever created, so every later tap on ANY app silently no-op'd via
            // the `state == APP_STATE_RUNNING` guard below — exactly matching
            // "apps just don't open" with zero error or crash.
            ESP_LOGE(TAG, "xTaskCreateWithCaps failed for '%s' (stack=%u)", app->name, (unsigned)stack);
            report_launch_oom(app);
            vSemaphoreDelete(ctx->done);
            ctx->done = NULL;
            return -1;
        }
    }
    purr_crash_guard_mark_start(app->name);
    app->state = APP_STATE_RUNNING;
    return 0;
}

// ── Personal (loaded .claw) apps ────────────────────────────────────────────
// Only one loaded module active at a time — claw_loader.h's own top comment
// documents this as claw_loader's own constraint (same "one at a time" the
// Lua VM already has for .meow/.hiss/.kitten), but claw_loader_load() itself
// has no guard against being called a second time before the first module
// is unloaded — doing so would silently orphan the first module's flash
// mapping/RAM allocations while overwriting the same claw_slot partition
// out from under it. Enforced here instead, at the one call site that can
// actually launch a personal app.
static claw_loaded_module_t s_personal_loaded;
// Synthetic — wraps s_personal_loaded's init/deinit function pointers so
// native_task()/app_manager_stop() can treat a personal app exactly like a
// pre-linked one (both dispatch through nothing but ctx->mod->init/deinit).
// Every other field is irrelevant on this path: nothing downstream of
// launch_personal() inspects magic/abi_version/version/catcalls/etc. for a
// module reached this way, only for one found via purr_kernel_get_module().
static purr_module_header_t s_personal_hdr;
// s_apps[] index currently holding the loaded module, -1 if none — lets
// app_manager_stop() know whether ITS app is the one to unload, and lets a
// second launch attempt (a different personal app, or the same one again
// while state is still transitioning) be refused cleanly instead of
// silently corrupting the one already running.
static int s_personal_running_idx = -1;

static int launch_personal(app_entry_t *app, int idx)
{
    if (purr_crash_guard_is_disabled(app->name)) {
        app->state = APP_STATE_ERROR;
        snprintf(app->error, sizeof(app->error), "disabled after repeated crashes");
        ESP_LOGW(TAG, "app '%s' disabled after repeated crashes — not launching", app->name);
        return -1;
    }

    if (s_personal_running_idx >= 0 && s_personal_running_idx != idx) {
        app->state = APP_STATE_ERROR;
        snprintf(app->error, sizeof(app->error), "another personal app is already running");
        ESP_LOGW(TAG, "app '%s' not launched — '%s' (personal) is already running",
                 app->name, s_apps[s_personal_running_idx].name);
        return -1;
    }

    // app->path is "personal:/<username>/<appname>" — built in
    // app_manager_scan_ex()'s personal-app block. Parsed back out here
    // rather than kept in a second field: app_entry_t has no spare string,
    // and every other tier already round-trips everything through `path`.
    const char *prefix = "personal:/";
    const char *rest = app->path + strlen(prefix);
    const char *slash = strchr(rest, '/');
    if (strncmp(app->path, prefix, strlen(prefix)) != 0 || !slash) {
        app->state = APP_STATE_ERROR;
        snprintf(app->error, sizeof(app->error), "malformed personal path");
        ESP_LOGE(TAG, "app '%s' has a malformed personal path: %s", app->name, app->path);
        return -1;
    }
    char username[USER_MGR_USERNAME_MAX + 1];
    size_t ulen = (size_t)(slash - rest);
    if (ulen >= sizeof(username)) ulen = sizeof(username) - 1;
    memcpy(username, rest, ulen);
    username[ulen] = '\0';
    const char *appname = slash + 1;

    if (!claw_loader_personal_load(username, appname, &s_personal_loaded)) {
        app->state = APP_STATE_ERROR;
        snprintf(app->error, sizeof(app->error), "load failed");
        ESP_LOGW(TAG, "app '%s' (personal) failed to load", app->name);
        return -1;
    }

    memset(&s_personal_hdr, 0, sizeof(s_personal_hdr));
    s_personal_hdr.module_type = PURR_MOD_APP;
    strncpy(s_personal_hdr.name, app->name, sizeof(s_personal_hdr.name) - 1);
    s_personal_hdr.init   = s_personal_loaded.init;
    s_personal_hdr.deinit = s_personal_loaded.deinit;

    app_task_ctx_t *ctx = &s_ctxs[idx];
    ctx->app  = app;
    ctx->mod  = &s_personal_hdr;
    ctx->done = xSemaphoreCreateBinary();
    if (!ctx->done) {
        ESP_LOGE(TAG, "xSemaphoreCreateBinary failed for '%s' — out of memory", app->name);
        claw_loader_unload(&s_personal_loaded);
        report_launch_oom(app);
        return -1;
    }

    app->mem_free_at_launch = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "launch .personal: %s (%s) — free heap: internal=%u largest_internal_block=%u",
             app->name, app->path,
             (unsigned)app->mem_free_at_launch,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    // Same PSRAM-backed-stack treatment as every non-static-stack native
    // app (see launch_native()'s matching comment) — nothing about a
    // personal app's own init() touches flash/NVS directly (it can't: the
    // import table is the only host code it can reach at all, and
    // purr_kernel_uptime_ms() does no such I/O), so it doesn't need the
    // static-stack exception settings/fileman/milkbar require.
    ctx->static_stack = false;
    uint32_t stack = 16384;   // same as CLAW-tier native apps — see launch_native()'s matching comment
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(native_task, app->name, stack, ctx, 4, &ctx->task, 0, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreateWithCaps failed for '%s' (stack=%u)", app->name, (unsigned)stack);
        report_launch_oom(app);
        vSemaphoreDelete(ctx->done);
        ctx->done = NULL;
        claw_loader_unload(&s_personal_loaded);
        return -1;
    }

    s_personal_running_idx = idx;
    purr_crash_guard_mark_start(app->name);
    app->state = APP_STATE_RUNNING;
    return 0;
}

// ── Public API ────────────────────────────────────────────────────────────────

int app_manager_scan_ex(bool include_sd)
{
    // s_apps/s_ctxs are heap-allocated by app_manager_init(), not static
    // arrays — still NULL if init() never ran (e.g. crash-guard disabled
    // this module after repeated strikes and skipped its init() outright,
    // or an allocation genuinely failed there and returned early). Nothing
    // here checked that before writing through &s_apps[s_app_count], which
    // for s_app_count==0 is a literal NULL-pointer store. Confirmed live on
    // Tab5: app_manager_count()'s lazy re-scan (see its own comment) is a
    // plain exported function call with no such gate, and any UI backend
    // that calls it — Mochi's springboard build does, unconditionally —
    // reaches this the instant app_manager is in a disabled/unloaded state,
    // producing a Guru Meditation "Store access fault" at address 0x0 that
    // repeats every boot (crash-guard sees it as another app_manager
    // failure and re-disables it, forever).
    if (!s_apps || !s_ctxs) {
        ESP_LOGW(TAG, "scan skipped — app_manager not initialised (s_apps/s_ctxs NULL)");
        return 0;
    }

    s_app_count = 0;

    // Discover pre-linked apps registered in the kernel module table
    int n = purr_kernel_module_count();
    for (int i = 0; i < n && s_app_count < MAX_APPS; i++) {
        const purr_module_header_t *hdr = purr_kernel_module_at(i);
        if (!hdr || hdr->module_type != PURR_MOD_APP) continue;

        app_entry_t *app = &s_apps[s_app_count];
        memset(app, 0, sizeof(*app));
        strncpy(app->name, hdr->name, sizeof(app->name) - 1);
        snprintf(app->path, sizeof(app->path), "prelinked:/%s", hdr->name);
        app->tier  = APP_TIER_CLAW;
        app->state = APP_STATE_IDLE;
        // Declared by the app itself — see purr_module_header_t::speed_demon.
        app->speed_demon = (hdr->speed_demon != 0);
        app->placement   = declared_placement_for(app->name);

        ESP_LOGI(TAG, "found [claw/pre-linked] %s", app->name);
        s_app_count++;
    }

    // Also scan filesystem paths for .meow / .paws / .claw files (SD extras).
    // scan_dir() has no timeout of its own — its opendir()/readdir() calls
    // ride entirely on whatever bound the caller is under. include_sd=false
    // (see app_manager_init()/kernel_tdp_boot.c's recovering-boot callers)
    // skips "/sdcard/..." paths specifically: a hang-triggered reboot resets
    // the ESP32 but not necessarily a still-degraded SD card/bus, and this
    // was confirmed live as the source of app_manager repeatedly blowing its
    // bounded module-init timeout during recovery boots, cascading into a
    // reboot loop instead of ever actually recovering.
    for (int i = 0; s_scan_paths[i]; i++) {
        if (!include_sd && strncmp(s_scan_paths[i], "/sdcard", 7) == 0) {
            ESP_LOGW(TAG, "skipping SD app scan (%s) — recovering from a hang-triggered reboot",
                     s_scan_paths[i]);
            continue;
        }
        scan_dir(s_scan_paths[i]);
    }

    // Personal apps — per-user loaded (.claw) apps living outside any
    // flash/SD apps/ directory, at /sdcard/personal/<username>/*.claw (see
    // claw_loader.h's "Personal-space storage" section). Only scanned for
    // whoever is CURRENTLY LOGGED IN — no cross-user browsing at this
    // layer (that's a milkbar/remote-app-manager concern, not app_manager's:
    // "the ability to add apps from the server ... move that into a
    // personal space under their user"). Same include_sd gate as the
    // filesystem scan above — this is SD too, same recovery-boot hazard.
    if (include_sd && user_mgr_is_logged_in()) {
        const char *username = user_mgr_current_user();
        int n_personal = claw_loader_personal_count(username);
        for (int i = 0; i < n_personal && s_app_count < MAX_APPS; i++) {
            char appname[48];
            if (!claw_loader_personal_at(username, i, appname, sizeof(appname))) continue;

            // Same shadowing rule scan_dir() already applies between flash
            // and SD — a personal app never overrides an existing entry of
            // the same display name, it's just skipped (ambiguous which one
            // a tap should mean, and a system/SD app is more likely to be
            // the one the user expects).
            if (find_app_slot_by_name(appname) >= 0) {
                ESP_LOGW(TAG, "personal app '%s' shadowed by an existing entry — skipping", appname);
                continue;
            }

            app_entry_t *app = &s_apps[s_app_count++];
            memset(app, 0, sizeof(*app));
            strncpy(app->name, appname, sizeof(app->name) - 1);
            snprintf(app->path, sizeof(app->path), "personal:/%s/%s", username, appname);
            app->tier  = APP_TIER_PERSONAL;
            app->state = APP_STATE_IDLE;

            ESP_LOGI(TAG, "found [personal] %s (user=%s)", app->name, username);
        }
    }

    ESP_LOGI(TAG, "scan complete: %d apps found", s_app_count);
    return s_app_count;
}

int app_manager_scan(void)
{
    return app_manager_scan_ex(true);
}

// ── Server Manager synthetic entry ──────────────────────────────────────
// See app_manager.h's own doc comment on app_manager_remote_mac(). Only
// while remote mode is on AND the current session is an admin on that
// server — user_mgr_is_admin(user_mgr_current_user()) already reflects
// the REMOTE identity's own admin flag once pairing_verify_user() has run
// (user_mgr_create_remote() syncs it on every login, see pairing_module.c),
// so this needs no new state of its own, just the two existing checks.
static bool server_manager_entry_active(void)
{
    return s_remote_mode && user_mgr_is_admin(user_mgr_current_user());
}

static int local_launch_idx(int idx)
{
    int real = translate_local_idx(idx);
    if (real < 0) return -1;
    return app_manager_launch_path(s_apps[real].path);
}

static int remote_launch_idx(int idx)
{
    if (server_manager_entry_active() && idx == s_remote_app_count) {
        // The LOCAL launch path, not remote_launch_stop_task() below —
        // "Server Manager" is a client-side app (source/apps/system/
        // server_manager/), not something the server runs. Declared in
        // app_manager.h, included at the top of this file, so this is
        // visible here regardless of where its definition happens to
        // sit further down.
        return app_manager_launch_by_name("server_manager");
    }
    if (idx < 0 || idx >= s_remote_app_count) return -1;
    // cheetah_home.c's icon_click_cb()/systemui_xp.c's task_btn_click_
    // cb() call this directly from the LVGL render task — a live
    // proximity_rpc_call() (the provider's actual implementation) must
    // never block there, same rule as every other proximity_rpc_call()
    // site in this codebase. remote_launch_stop_task() below fires it
    // on its own short-lived task instead and this returns immediately,
    // optimistically — same "return 0 the instant the work is handed
    // off, real state observed later" shape launch_native() already has
    // for a LOCAL launch (it doesn't wait for init() to finish either).
    // The next periodic list() refresh (remote_refresh_task()) is what
    // actually shows the app as running once the launch really lands.
    return remote_launch_stop_task(idx, /*launch=*/true) ? 0 : -1;
}

int app_manager_launch_idx(int idx)
{
    return s_remote_mode ? remote_launch_idx(idx) : local_launch_idx(idx);
}

int app_manager_launch_path(const char *path)
{
    for (int i = 0; i < s_app_count; i++) {
        if (strcmp(s_apps[i].path, path) == 0) {
            app_entry_t *app = &s_apps[i];
            if (app->state == APP_STATE_RUNNING) return 0;
            if (app->tier == APP_TIER_MEOW || app->tier == APP_TIER_HISS ||
                app->tier == APP_TIER_KITTEN) return launch_meow(app, i);
            if (app->tier == APP_TIER_PERSONAL) return launch_personal(app, i);
            return launch_native(app, i);
        }
    }
    return -1;
}

// For a pre-linked (PURR_MOD_APP) app, app->name is the module's own
// hdr->name (see app_manager_scan() above) — matches the module's
// PURR_MODULE_REGISTER() name, not necessarily its display name. Intended
// for a specialized kernel boot.c to auto-launch a specific built-in app
// (e.g. a device's diagnostics screen) once app_manager_init() has scanned;
// calling this before that scan has run always returns -1 (nothing found).
int app_manager_launch_by_name(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < s_app_count; i++) {
        if (strcmp(s_apps[i].name, name) == 0) {
            app_entry_t *app = &s_apps[i];
            if (app->state == APP_STATE_RUNNING) return 0;
            if (app->tier == APP_TIER_MEOW || app->tier == APP_TIER_HISS ||
                app->tier == APP_TIER_KITTEN) return launch_meow(app, i);
            if (app->tier == APP_TIER_PERSONAL) return launch_personal(app, i);
            return launch_native(app, i);
        }
    }
    return -1;
}

static void remote_stop_idx(int idx)
{
    if (idx < 0 || idx >= s_remote_app_count) return;
    // Same fire-and-forget shape as the launch branch in
    // app_manager_launch_idx() just above, same reason — never block
    // the LVGL render task on a live network round trip.
    remote_launch_stop_task(idx, /*launch=*/false);
}

// Takes a REAL (raw) s_apps[] index, never a UI-facing compacted one —
// see local_stop_idx() below for the wrapper that translates one of
// those, and app_manager_on_win_close() (further down) for why a second,
// raw-index entry point is needed at all: it already has the real index
// via pointer arithmetic on the app_entry_t itself, and re-running that
// through translate_local_idx() as if it were a compacted index would be
// exactly the same class of bug this whole translation scheme exists to
// fix, just from a different call site. Confirmed live as a real,
// reported regression: the window close button stopped actually closing
// apps the moment local_stop_idx() started expecting a compacted index.
static void local_stop_real(int real)
{
    if (real < 0 || real >= s_app_count) return;
    app_entry_t *app = &s_apps[real];
    if (app->state != APP_STATE_RUNNING) return;

    app_task_ctx_t *ctx = &s_ctxs[real];

    // Wait for the task to finish (or force-kill it) BEFORE calling
    // deinit() — deinit() (e.g. lua_runtime_deinit()'s lua_close()) must
    // never run while this app's own task might still be executing, or it
    // frees/invalidates state (the Lua interpreter, in lua_runtime's case)
    // out from under a task still actively using it. Confirmed live: this
    // ordering (deinit() first, wait/kill second) is what let a looping
    // .meow script's close button free a still-running lua_State, and —
    // under MiniWin specifically — corrupt state badly enough to eventually
    // trip an unrecoverable internal assert while nested inside the UI
    // lock, freezing the whole UI. Same underlying bug is the likely cause
    // of an earlier, previously-unexplained clock.meow crash under Cupcake
    // too. A successful semaphore take proves native_task()/meow_task()
    // already ran to completion and self-deleted (vTaskDelete(NULL)) —
    // ctx->task is then a stale handle whose TCB may already be
    // reclaimed/reused by an unrelated task, so it must not be touched.
    // Only a genuine timeout means the task is still alive and actually
    // needs force-deleting — and only once it's confirmed to be truly gone
    // (self-completed or force-deleted) is it ever safe to call deinit().
    bool hung = false;
    if (ctx->done) {
        purr_kernel_ui_breadcrumb("appstop:wait_done");
        if (xSemaphoreTake(ctx->done, pdMS_TO_TICKS(2000)) == pdTRUE) {
            ctx->task = NULL;
        } else if (ctx->task) {
            ESP_LOGW(TAG, "force-deleting task for '%s'", app->name);
            purr_kernel_ui_breadcrumb("appstop:force_delete");
            // Must match how this task's stack was created (see
            // native_task()'s matching comment) — static-stack apps
            // (settings/fileman) use plain vTaskDelete(); everything else
            // was created via xTaskCreateWithCaps() (PSRAM-backed stack)
            // and needs vTaskDeleteWithCaps().
            if (ctx->static_stack) vTaskDelete(ctx->task);
            else                   vTaskDeleteWithCaps(ctx->task);
            ctx->task = NULL;
            hung = true;
        }
        vSemaphoreDelete(ctx->done);
        ctx->done = NULL;
    }

    // Now safe: the task is guaranteed to no longer be running.
    if (ctx->mod && ctx->mod->deinit) {
        purr_kernel_ui_breadcrumb("appstop:deinit");
        ctx->mod->deinit();
    }

    // Personal (loaded .claw) app: deinit() above just ran the module's own
    // claw_personal_deinit() — now safe to unmap its flash mapping and free
    // its RAM copies (claw_loader_unload() must not run before deinit() has
    // had its chance to finish using them). Matches this file's own
    // s_personal_running_idx comment on launch_personal().
    if (real == s_personal_running_idx) {
        claw_loader_unload(&s_personal_loaded);
        s_personal_running_idx = -1;
    }

    // Fallback window cleanup: a native app's own deinit() (e.g.
    // taskmgr_deinit()) already destroys app->window itself, in which case
    // this is a harmless no-op (get_win() on an already-freed handle
    // returns NULL, so the backend's win_destroy() just no-ops). But
    // lua_runtime's deinit() is shared across every .meow/.hiss/.kitten app
    // and has no per-app window to destroy — without this, closing a Lua
    // app via Back left its window orphaned on screen forever. This is the
    // one place that's guaranteed to run after every app's task has fully
    // stopped, regardless of tier, so it's the right spot for the net.
    if (app->window) {
        purr_kernel_ui_breadcrumb("appstop:win_destroy");
        purr_win_destroy(app->window);
        app->window = 0;
    }

    if (hung) purr_crash_guard_mark_hang(app->name, "TASK UNRESPONSIVE");
    else      purr_crash_guard_mark_stop(app->name, /*ok=*/true, NULL);

    app->state = APP_STATE_STOPPED;
    ESP_LOGI(TAG, "stopped: %s", app->name);
    purr_kernel_ui_breadcrumb("appstop:done");
}

// The UI-facing entry point — idx is a COMPACTED index, exactly what
// app_manager_count()/get() report (translate_local_idx() undoes the
// same skip local_count()/get() apply). This is what app_manager_stop()
// itself dispatches to; app_manager_on_win_close() below deliberately
// bypasses both this AND that dispatcher, calling local_stop_real()
// directly instead — its own idx is already a real one.
static void local_stop_idx(int idx)
{
    local_stop_real(translate_local_idx(idx));
}

void app_manager_stop(int idx)
{
    if (s_remote_mode) remote_stop_idx(idx);
    else                local_stop_idx(idx);
}

// ── Remote mode ─────────────────────────────────────────────────────────────
// See app_manager.h's own doc comment for the design. local_count()/get()/
// launch_idx()/stop_idx() and their remote_*() counterparts (the public
// app_manager_count()/get()/launch_idx()/stop() above each just dispatch to
// one or the other on s_remote_mode) are what used to be four inline
// if (s_remote_mode) {...} else {...} branches duplicated across four
// function bodies — split into named helpers instead, so local and remote
// handling read as two genuinely separate paths rather than one hybrid one.

// One-shot task for a fire-and-forget remote launch/stop — see
// app_manager_launch_idx()/app_manager_stop()'s own comments for why this
// can't run synchronously on whichever task called them. Copies the app
// name onto its own heap block (not a pointer into s_remote_apps[], which
// remote_refresh_task() can overwrite the instant its next list() call
// lands) so the name is still valid however long the provider's call takes.
typedef struct {
    uint8_t mac[6];
    char    name[48];   // matches app_entry_t::name / remote_app_entry_t::name
    bool    launch;      // true = provider->launch(), false = provider->stop()
} remote_op_ctx_t;

static void remote_op_task(void *arg)
{
    remote_op_ctx_t *ctx = (remote_op_ctx_t *)arg;
    // KNOWN, ACCEPTED RACE: app_manager_clear_remote() waits for
    // remote_refresh_task() to exit before a later app_manager_set_remote()
    // reassigns s_remote_provider, but does NOT wait for an in-flight
    // remote_op_task() like this one — a launch/stop tapped immediately
    // before Disconnect could still be reading s_remote_provider here the
    // instant a fresh set_remote() (a different device, say) overwrites it.
    // Same class of narrow fire-and-forget race the old milkbar send_msg_
    // task already carried (unawaited proximity_rpc_call() from a one-shot
    // task) — not solved there either, and not worth a join here for what
    // is, worst case, a launch/stop landing on the wrong (still trusted)
    // paired device once in a great while.
    if (ctx->launch) s_remote_provider.launch(ctx->mac, ctx->name);
    else if (s_remote_provider.stop) s_remote_provider.stop(ctx->mac, ctx->name);
    heap_caps_free(ctx);
    vTaskDeleteWithCaps(NULL);
}

static bool remote_launch_stop_task(int idx, bool launch)
{
    if (launch && !s_remote_provider.launch) return false;
    if (!launch && !s_remote_provider.stop)  return false;

    remote_op_ctx_t *ctx = heap_caps_malloc(sizeof(*ctx), MALLOC_CAP_SPIRAM);
    if (!ctx) return false;
    memcpy(ctx->mac, s_remote_mac, 6);
    strncpy(ctx->name, s_remote_apps[idx].name, sizeof(ctx->name) - 1);
    ctx->name[sizeof(ctx->name) - 1] = '\0';
    ctx->launch = launch;

    TaskHandle_t task = NULL;
    BaseType_t ok = xTaskCreateWithCaps(remote_op_task, "app_mgr_rop", 4096, ctx, 3, &task, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) { heap_caps_free(ctx); return false; }
    return true;
}

static void remote_refresh_task(void *arg)
{
    (void)arg;
    while (s_remote_task_running) {
        int n = s_remote_provider.list(s_remote_mac, s_remote_apps, MAX_REMOTE_APPS);
        // -1 = this pass's fetch failed (offline/timed out) — leave
        // s_remote_app_count and s_remote_apps[] exactly as they were from
        // the last successful pass, rather than flashing the registry to
        // empty for every transient miss. Remote mode itself stays on
        // regardless; only app_manager_clear_remote() turns it off.
        if (n >= 0) s_remote_app_count = n;
        for (int waited_ms = 0; waited_ms < REMOTE_REFRESH_MS && s_remote_task_running; waited_ms += 200) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    if (s_remote_task_done) xSemaphoreGive(s_remote_task_done);
    vTaskDeleteWithCaps(NULL);
}

// Real local-to-remote "close before opening" hand-off — walks the local
// registry and stops every RUNNING app except "milkbar" (the one
// sanctioned exception: it's what stays reachable, windowless, to get
// back to Dashboard's Disconnect / the Connection screen — see
// milkbar_app.c's own file header comment on that design). Called
// unconditionally at the top of app_manager_set_remote(), every time — a
// straight remote-server-to-remote-server switch (A -> B, s_remote_mode
// already true on entry) finds nothing local running and is a harmless
// no-op loop either way. Calls local_stop_idx() directly rather than the
// public app_manager_stop() dispatcher: that dispatcher reads the CURRENT
// s_remote_mode, which during an A -> B switch is still true at this
// point — the local path is what's actually wanted here regardless.
static void close_local_apps_for_remote_handoff(void)
{
    for (int i = 0; i < s_app_count; i++) {
        if (s_apps[i].state != APP_STATE_RUNNING) continue;
        if (strcmp(s_apps[i].name, "milkbar") == 0) continue;
        ESP_LOGI(TAG, "remote hand-off: stopping local app '%s'", s_apps[i].name);
        // local_stop_real(), not local_stop_idx() — `i` here is a real
        // s_apps[] index from iterating s_apps[]/s_app_count directly,
        // same reasoning app_manager_on_win_close()/app_manager_deinit()/
        // app_manager_kill_worst_offender() all document.
        local_stop_real(i);
    }
}

bool app_manager_set_remote(const uint8_t mac[6], const app_manager_remote_provider_t *provider)
{
    if (!mac || !provider || !provider->list) return false;

    close_local_apps_for_remote_handoff();

    // Clears any previous remote session first — covers both "already
    // pointed at a different device" and "called twice in a row" the same
    // way, and guarantees remote_op_task() (above) never runs concurrently
    // with the provider struct it captured being replaced out from under it.
    app_manager_clear_remote();

    if (!s_remote_apps) {
        s_remote_apps = heap_caps_malloc(sizeof(app_entry_t) * MAX_REMOTE_APPS, MALLOC_CAP_SPIRAM);
        if (!s_remote_apps) {
            ESP_LOGE(TAG, "remote mode: PSRAM alloc failed (%u bytes)",
                     (unsigned)(sizeof(app_entry_t) * MAX_REMOTE_APPS));
            return false;
        }
    }
    memset(s_remote_apps, 0, sizeof(app_entry_t) * MAX_REMOTE_APPS);
    memcpy(s_remote_mac, mac, 6);
    s_remote_provider  = *provider;
    s_remote_app_count = 0;

    if (!s_remote_task_done) s_remote_task_done = xSemaphoreCreateBinary();
    s_remote_task_running = true;
    s_remote_mode = true;   // set before the task starts — remote_op_task() reads s_remote_provider, not a local

    BaseType_t ok = xTaskCreateWithCaps(remote_refresh_task, "app_mgr_remote", 4096, NULL, 3, &s_remote_task, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "remote mode: refresh task create failed");
        s_remote_task_running = false;
        s_remote_mode = false;
        return false;
    }

    ESP_LOGI(TAG, "remote mode ON — pointed at %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}

void app_manager_clear_remote(void)
{
    if (!s_remote_mode) return;
    s_remote_task_running = false;
    // REMOTE_REFRESH_MS's own step loop notices s_remote_task_running
    // within 200ms; the +3500 headroom covers one in-flight list() call
    // riding proximity_rpc_call()'s own timeout before this task can exit —
    // same margin RPC_TIMEOUT_MS-based waits elsewhere in this codebase
    // (milkbar_app_deinit(), historically) already budget for that call.
    if (s_remote_task_done) xSemaphoreTake(s_remote_task_done, pdMS_TO_TICKS(REMOTE_REFRESH_MS + 3500));
    s_remote_task = NULL;
    s_remote_mode = false;
    s_remote_app_count = 0;
    ESP_LOGI(TAG, "remote mode OFF — back to the local registry");
}

bool app_manager_is_remote(void) { return s_remote_mode; }

const char *app_manager_get_pending_meow_path(void) { return s_meow_pending_path; }

const char *app_manager_get_pending_meow_code(size_t *out_len) {
    if (out_len) *out_len = s_meow_pending_len;
    return s_meow_pending_code;
}

bool app_manager_get_pending_meow_privileged(void) { return s_meow_pending_privileged; }

// +1 for the synthetic "Server Manager" entry — see
// server_manager_entry_active()'s own doc comment.
static int remote_count(void)
{
    return s_remote_app_count + (server_manager_entry_active() ? 1 : 0);
}

// True for a local app that must never appear in the NORMAL (non-remote)
// registry. Nothing currently qualifies — "server_manager" used to
// (hiding it from the Start Menu, reachable only via the synthetic
// remote-mode entry) until that turned out to make the app entirely
// unreachable from its own local icon: launched with no remote target
// (app_manager_remote_mac() returns false, the expected case for a
// normal local launch, not the "shouldn't happen" case the app's own
// original comment assumed), it had no way to pick one either — no
// Control Panel entry, nothing to manage, a real reported dead end.
// server_manager_app.c now has its own device picker (pairing_device_
// count()/at()) for exactly that case, so hiding it here is no longer
// needed — the synthetic remote-mode entry still exists too, as a
// convenience that pre-selects the already-connected server instead of
// asking. This function (and translate_local_idx()'s own compacted-
// index handling) stays in place for whatever future app genuinely
// needs it — real, defensive scaffolding, not dead code just because
// nothing uses it today.
static bool is_hidden_local_app(const char *name)
{
    (void)name;
    return false;
}

// Translates a UI-facing (compacted, hidden-entries-skipped) LOCAL index
// into the real s_apps[] index it corresponds to, or -1 if out of range.
// local_get()/local_launch_idx()/local_stop_idx() all take an index from
// a caller that only ever saw app_manager_count()/get()'s FILTERED view —
// every one of them must agree on this exact mapping, or "the Nth icon
// shown" and "the Nth real s_apps[] entry" silently drift apart the
// moment any entry is hidden from one but not the other. See this file's
// forward declaration of this function (near s_local_unlocked) for the
// real, reported bug this fixes.
static int translate_local_idx(int idx)
{
    if (idx < 0) return -1;
    int seen = 0;
    for (int i = 0; i < s_app_count; i++) {
        if (is_hidden_local_app(s_apps[i].name)) continue;
        if (seen == idx) return i;
        seen++;
    }
    return -1;
}

// s_local_unlocked gate: report empty until app_manager_notify_unlocked()
// has run — see app_manager.h's own doc comment. Checked BEFORE the boot-
// order backstop below, so a caller querying before unlock never triggers
// that scan either; the deferred scan happens once, inside
// app_manager_notify_unlocked() itself.
static int local_count(void)
{
    if (!s_local_unlocked) return 0;

    // Boot-order backstop: init()'s scan (when it still ran unconditionally
    // at init() — now only for the pre-OOBE-completion case, see
    // app_manager_init()) ran while the pre-linked app modules were still
    // unregistered (app_manager loads P2, PURR_MOD_APP entries load after
    // it), so it always found zero of them. KittenUI's desktop happens to
    // re-scan when it opens; MiniWin's WinCE start menu only calls this —
    // leaving its Programs list permanently empty (confirmed live on tab5).
    // Re-scan on first query instead: by the time any UI asks, the module
    // table is complete. One-shot on empty only — entries hold live launch
    // state, so a routine re-scan would clobber a RUNNING app's slot, but an
    // empty list has nothing to lose.
    if (s_app_count == 0) app_manager_scan();

    int n = 0;
    for (int i = 0; i < s_app_count; i++) {
        if (!is_hidden_local_app(s_apps[i].name)) n++;
    }
    return n;
}

int app_manager_count(void)
{
    return s_remote_mode ? remote_count() : local_count();
}

void app_manager_notify_unlocked(void)
{
    if (s_local_unlocked) return;   // idempotent — a relock->unlock cycle must not double-fire
    s_local_unlocked = true;
    ESP_LOGI(TAG, "local unlock: registry now visible");
}

void app_manager_notify_locked(void)
{
    if (!s_local_unlocked) return;
    s_local_unlocked = false;
    ESP_LOGI(TAG, "local lock: registry hidden");
}

static const app_entry_t *remote_get(int idx)
{
    if (server_manager_entry_active() && idx == s_remote_app_count) {
        // Synthesized fresh each call, not cached — cheap (one static
        // struct, memset + a name copy), and avoids ever needing to
        // invalidate a stale cached copy. tier/state are cosmetic here
        // (cheetah_home.c/systemui_xp.c only ever read ->name/->tier/
        // ->state/->window for rendering — see app_manager_remote.c's
        // own provider_list() comment on that same shortlist); IDLE/
        // CLAW read fine as "just a normal app icon," same as anything
        // else in the grid.
        static app_entry_t s_server_manager_entry;
        memset(&s_server_manager_entry, 0, sizeof(s_server_manager_entry));
        snprintf(s_server_manager_entry.name, sizeof(s_server_manager_entry.name), "Server Manager");
        s_server_manager_entry.tier  = APP_TIER_CLAW;
        s_server_manager_entry.state = APP_STATE_IDLE;
        return &s_server_manager_entry;
    }
    if (idx < 0 || idx >= s_remote_app_count) return NULL;
    return &s_remote_apps[idx];
}

static const app_entry_t *local_get(int idx)
{
    if (!s_local_unlocked) return NULL;
    int real = translate_local_idx(idx);
    return real >= 0 ? &s_apps[real] : NULL;
}

const app_entry_t *app_manager_get(int idx)
{
    return s_remote_mode ? remote_get(idx) : local_get(idx);
}

bool app_manager_remote_mac(uint8_t out_mac[6])
{
    if (!s_remote_mode || !out_mac) return false;
    memcpy(out_mac, s_remote_mac, 6);
    return true;
}

app_placement_t app_manager_decide_placement(app_placement_t declared,
                                              bool my_strong_compute,
                                              bool peer_strong_compute,
                                              bool peer_has_display)
{
    if (declared != APP_PLACE_HYBRID) return declared;   // REMOTE/LOCAL: no comparison needed

    if (my_strong_compute != peer_strong_compute) {
        // Exactly one side has it — run there.
        return my_strong_compute ? APP_PLACE_LOCAL : APP_PLACE_REMOTE;
    }
    // Tied on compute (both or neither strong) — prefer REMOTE only if
    // the peer can actually show it; otherwise there's nothing gained by
    // sending it over there.
    return peer_has_display ? APP_PLACE_REMOTE : APP_PLACE_LOCAL;
}

// ── Launcher button callback ──────────────────────────────────────────────────

// user is a pointer INTO s_apps[]'s own path[] field — s_apps is
// allocated once in app_manager_init() and never freed/moved for the
// device's whole runtime (see its own allocation comment), so this stays
// valid for as long as the launcher window can possibly still be open.
// Launches by PATH rather than by index on purpose: this legacy "Cat
// Apps" browser always iterates s_apps[]/s_app_count directly (unlike
// cheetah_home.c/systemui_xp.c, it doesn't go through app_manager_count()/
// get() at all, so it isn't affected by remote mode) — a path match is
// unambiguous regardless of any index-compaction app_manager_launch_idx()
// applies for the FILTERED view (see translate_local_idx()'s own
// comment), so there's nothing here that needs to track that mapping.
static void launcher_btn_cb(purr_wid_t wid, purr_event_t event, void *user)
{
    if (event != PURR_EVENT_CLICKED) return;
    const char *path = (const char *)user;
    ESP_LOGI(TAG, "launcher: launching %s", path);
    app_manager_launch_path(path);
}

void app_manager_open_launcher(void)
{
    ESP_LOGI(TAG, "cat apps launcher: %d app(s)", s_app_count);

    if (!purr_kernel_ui()) {
        ESP_LOGW(TAG, "no UI registered — launcher running in serial-only mode");
        for (int i = 0; i < s_app_count; i++) {
            if (is_hidden_local_app(s_apps[i].name)) continue;
            ESP_LOGI(TAG, "  [%d] %s (%s)", i, s_apps[i].name, tier_name(s_apps[i].tier));
        }
        return;
    }

    purr_win_t win = purr_win_create("Cat Apps");
    if (!win) {
        ESP_LOGE(TAG, "launcher: failed to create window");
        return;
    }

    // is_hidden_local_app() — same "never a normal Start Menu/launcher
    // entry" rule cheetah_home.c/systemui_xp.c's own listing already
    // follows via app_manager_count()/get(); this legacy browser needed
    // its own copy of that check since it reads s_apps[] directly.
    bool any = false;
    for (int i = 0; i < s_app_count; i++) {
        if (is_hidden_local_app(s_apps[i].name)) continue;
        purr_win_button(win, s_apps[i].name, launcher_btn_cb, (void *)s_apps[i].path);
        any = true;
    }
    if (!any) {
        purr_win_label(win, "No apps installed.\nCopy .meow/.hiss/.paws files to /sdcard/apps");
    }

    purr_win_show(win);
    ESP_LOGI(TAG, "launcher window open");
}

// ── Window tracking ───────────────────────────────────────────────────────────
// Wired up via purr_kernel_set_window_created_cb() below — see the comment
// above s_ctxs' old s_launching_app field for why this doesn't need every
// app to report its own window, and why it's keyed by task handle now.

static void app_manager_on_win_close(purr_wid_t win, purr_event_t event, void *user) {
    (void)win; (void)event;
    app_entry_t *app = (app_entry_t *)user;
    int idx = (int)(app - s_apps);
    // local_stop_real() directly — NOT the public app_manager_stop()
    // dispatcher. idx here is a REAL s_apps[] index (pointer arithmetic
    // against the app_entry_t app_manager_on_window_created() already
    // resolved via find_app_by_current_task()), and a window this hook
    // fires for is always a LOCALLY launched app's own window regardless
    // of whether remote mode happens to be on elsewhere right now — both
    // routing through app_manager_stop() (which would send it through
    // remote_stop_idx() instead, wrongly, if s_remote_mode were true) and
    // through local_stop_idx() (which would re-translate an already-real
    // index as if it were a compacted one) are wrong here. This was a
    // real, reported regression: the close button stopped closing apps
    // the moment local_stop_idx() started expecting a compacted index.
    local_stop_real(idx);
}

// Finds the app_entry_t whose launch task is the one calling right now —
// race-free (xTaskGetCurrentTaskHandle() only ever returns the caller's own
// identity), unlike the single shared s_launching_app global this replaced.
// Returns NULL for calls from any other task (a widget callback invoked from
// the UI's own render task, a follow-up dialog/window an app opens later,
// etc.) — correctly leaving app->window as the app's *original* window from
// its init() call, not overwritten by a later one.
static app_entry_t *find_app_by_current_task(void) {
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < s_app_count; i++) {
        if (s_ctxs[i].task == me) return s_ctxs[i].app;
    }
    return NULL;
}

// Called by a native app that ends on its own terms, so the manager stops
// believing it is still running.
//
// Takes the app NAME rather than using the calling task's handle. That was the
// first attempt and it silently did nothing: app_manager launches a native app
// on a short-lived `native_task` that calls init() and exits, so s_ctxs[].task
// is THAT task's handle. An app like MagiDOS spawns its own long-lived task from
// init(), and that task's handle was never in s_ctxs at all, so the match never
// fired.
//
// What actually blocks a relaunch is app->state: app_manager_launch_path() bails
// with `if (app->state == APP_STATE_RUNNING) return 0;`. Nothing cleared it for
// an app that exited by itself, so the second tap took that early return — and
// an exclusive app that drives the panel directly has no window to re-show, so
// nothing happened at all and the launcher just sat there.
//
// Safe to call from the exiting task: it only touches bookkeeping.
void app_manager_notify_exited(const char *name) {
    if (!name) return;
    for (int i = 0; i < s_app_count; i++) {
        if (strcmp(s_apps[i].name, name) != 0) continue;
        bool was_speed_demon = s_apps[i].speed_demon;
        s_apps[i].state  = APP_STATE_STOPPED;
        s_apps[i].window = 0;
        // Release the launch context too, so a stale handle can never be
        // mistaken for a live one by find_app_by_current_task().
        for (int c = 0; c < s_app_count; c++) {
            if (s_ctxs[c].app == &s_apps[i]) { s_ctxs[c].task = NULL; s_ctxs[c].app = NULL; }
        }
        ESP_LOGI(TAG, "'%s' reported exit — relaunchable again", name);
        // Put the OS back. Paired with the enter in native_task, so an app that
        // asked for the machine can never leave it torn down.
        if (was_speed_demon && purr_speed_demon_active()) purr_speed_demon_exit();
        return;
    }
    ESP_LOGW(TAG, "notify_exited: no app named '%s'", name);
}

static void app_manager_on_window_created(purr_win_t win) {
    app_entry_t *app = find_app_by_current_task();
    if (!app) return;
    app->window = win;
    purr_win_on_close(win, app_manager_on_win_close, (void *)app);
}

// Launches the first .kitten app found on the initial boot scan, without
// the user manually tapping it. Called once from app_manager_init() only —
// deliberately NOT from app_manager_scan() itself, so a later manual
// rescan (e.g. Fileman's SD hot-reload) never re-triggers this. The Lua
// runtime on these boards is single-instance ("only one Lua VM runs at a
// time" — see app_manager_get_pending_meow_path()'s doc comment), so this
// launches the first .kitten found, not every one present — the same
// limitation manually launching multiple .meow/.hiss apps already has,
// not a new restriction.
static void autorun_kitten(void)
{
    for (int i = 0; i < s_app_count; i++) {
        if (s_apps[i].tier == APP_TIER_KITTEN) {
            ESP_LOGI(TAG, "autorun: launching .kitten app '%s'", s_apps[i].name);
            // app_manager_launch_path(), not app_manager_launch_idx(i) —
            // `i` here is a REAL s_apps[] index, but launch_idx() is the
            // UI-facing dispatcher and its local branch now expects a
            // COMPACTED one (translate_local_idx()). A path match sides
            // steps the whole question, same fix already applied to
            // app_manager_open_launcher()/app_manager_on_win_close().
            app_manager_launch_path(s_apps[i].path);
            return;
        }
    }
}

// Same shape as autorun_kitten() just above: called once from
// app_manager_init() only, never from app_manager_scan() — a later manual
// rescan must not re-launch OOBE just because it happened to run again.
// user_mgr_oobe_completed() is the sole gate (see its own doc comment in
// user_mgr.h for why this is an explicit completion marker rather than
// inferred from account state) — the "oobe" app itself is responsible for
// calling user_mgr_set_oobe_completed() when it finishes or is skipped, the
// same way every other app owns its own exit path.
//
// Silently does nothing if no app named "oobe" is registered — a device
// that doesn't ship it (or one built before this existed) must not log an
// error for a condition that isn't actually wrong.
static void autorun_oobe(void)
{
    if (user_mgr_oobe_completed()) return;
    for (int i = 0; i < s_app_count; i++) {
        if (strcmp(s_apps[i].name, "oobe") == 0) {
            ESP_LOGI(TAG, "autorun: first run — launching OOBE setup");
            // app_manager_launch_by_name(), not app_manager_launch_idx(i)
            // — same reasoning as autorun_kitten() just above (`i` is a
            // real index, launch_idx() now wants a compacted one); a name
            // match is even more direct here since one's already in hand.
            app_manager_launch_by_name("oobe");
            return;
        }
    }
}

// Approximates "which running app is costing the most memory" without real
// per-task heap accounting (nothing in ESP-IDF tracks allocations by owner
// out of the box, and wrapping every malloc/free to add that was judged not
// worth the risk/complexity here) — each app's mem_free_at_launch (stamped
// in launch_native()/launch_meow() above) is how much internal SRAM was
// free the moment it started; whichever running app's launch-time reading
// minus the CURRENT reading is largest has coincided with the biggest drop
// since it started, making it the prime suspect. Registered with
// purr_kernel_set_mem_pressure_cb() below — see that function's doc comment
// in purr_kernel.h for why this is a callback rather than purr_kernel.c
// calling app_manager directly (layering: kernel core doesn't know
// app_manager exists, same reasoning as the window-created-cb hook).
static bool app_manager_kill_worst_offender(void)
{
    uint32_t now_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int     worst_idx  = -1;
    int32_t worst_drop = 0;   // only ever kill an app that looks *worse* than at its own launch

    for (int i = 0; i < s_app_count; i++) {
        if (s_apps[i].state != APP_STATE_RUNNING) continue;
        int32_t drop = (int32_t)s_apps[i].mem_free_at_launch - (int32_t)now_free;
        if (drop > worst_drop) {
            worst_drop = drop;
            worst_idx  = i;
        }
    }

    // Nothing running, or every running app's launch-time reading is no
    // better than now (the pressure isn't attributable to any of them —
    // could be a kernel/driver-level leak instead) — correctly leave this
    // to the watchdog's next-tier escalation (a full restart) rather than
    // killing an app that's probably innocent.
    if (worst_idx < 0) return false;

    ESP_LOGW(TAG, "memory watchdog: stopping '%s' (~%d bytes consumed since launch, worst of %d running)",
             s_apps[worst_idx].name, (int)worst_drop, s_app_count);
    purr_kernel_notify("App stopped", s_apps[worst_idx].name, "memory watchdog");
    // local_stop_real(), not app_manager_stop() — worst_idx is a REAL
    // s_apps[] index (found by scanning s_apps[]/s_app_count directly,
    // entirely local state — a remote app was never a candidate here at
    // all), same reasoning app_manager_on_win_close() already documents.
    local_stop_real(worst_idx);
    return true;
}

int app_manager_init(void)
{
    // PSRAM-preferred, internal-RAM fallback — PSRAM-less devices (e.g.
    // Heltec V3, psram=false) have no MALLOC_CAP_SPIRAM pool at all, so a
    // hard SPIRAM-only allocation here failed unconditionally and disabled
    // this module via the crash guard after a few boots. Same fallback
    // pattern as proximity_rpc.c's reassembly buffers. Note this only
    // covers app_manager's own bookkeeping tables — app task stacks below
    // (xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)) still
    // require real PSRAM per-app (except settings/fileman's static
    // stacks), so launching most apps on a PSRAM-less device remains
    // unsupported; only enough to let app_manager itself come up and
    // answer queries like the app list.
    s_apps = heap_caps_malloc(sizeof(app_entry_t) * MAX_APPS, MALLOC_CAP_SPIRAM);
    if (!s_apps) s_apps = heap_caps_malloc(sizeof(app_entry_t) * MAX_APPS, MALLOC_CAP_DEFAULT);
    s_ctxs = heap_caps_malloc(sizeof(app_task_ctx_t) * MAX_APPS, MALLOC_CAP_SPIRAM);
    if (!s_ctxs) s_ctxs = heap_caps_malloc(sizeof(app_task_ctx_t) * MAX_APPS, MALLOC_CAP_DEFAULT);
    if (!s_apps || !s_ctxs) {
        ESP_LOGE(TAG, "alloc failed for s_apps/s_ctxs (%u + %u bytes)",
                 (unsigned)(sizeof(app_entry_t) * MAX_APPS), (unsigned)(sizeof(app_task_ctx_t) * MAX_APPS));
        return -1;
    }
    memset(s_apps, 0, sizeof(app_entry_t) * MAX_APPS);
    memset(s_ctxs, 0, sizeof(app_task_ctx_t) * MAX_APPS);
    purr_kernel_set_window_created_cb(app_manager_on_window_created);
    purr_kernel_set_mem_pressure_cb(app_manager_kill_worst_offender);
    // See app_manager_scan_ex()'s comment — this call runs under
    // load_one_static()'s own 5s bounded-init window on a recovering boot,
    // with no timeout of its own around the SD readdir() calls otherwise.
    bool recovering = purr_crash_guard_pending_recovery(NULL, 0, NULL, 0);
    app_manager_scan_ex(!recovering);
    autorun_kitten();
    autorun_oobe();
    ESP_LOGI(TAG, "init complete");
    return 0;
}

void app_manager_deinit(void)
{
    // local_stop_real(), not app_manager_stop() — same reasoning as
    // app_manager_kill_worst_offender()/app_manager_on_win_close(): `i`
    // here is a real s_apps[] index from iterating s_apps[]/s_app_count
    // directly, entirely local, regardless of whatever s_remote_mode
    // happens to be at teardown time.
    for (int i = 0; i < s_app_count; i++) {
        if (s_apps[i].state == APP_STATE_RUNNING) {
            local_stop_real(i);
        }
    }
    s_app_count = 0;
}

// ── .purr module header ───────────────────────────────────────────────────────

PURR_MODULE_REGISTER(app_manager) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    // Explicit — unset (0) sorted this ahead of P1 drivers (see miniwin's
    // matching comment for the failure that exposed it).
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "app_manager",
    .version           = "1.0.1",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,   // display/touch used at runtime via catcall, not hard-required
    .init              = app_manager_init,
    .deinit            = app_manager_deinit,
};
