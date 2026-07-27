#pragma once
// purr_kernel.h — PURR OS kernel spine API
//
// This is what modules call into the kernel for. The kernel owns these
// registries. Modules register their catcalls here at init time. Other
// modules call through here — never directly into another module.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "purr_module.h"
#include "../catcalls/catcalls.h"
#include "../catcalls/catcall_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── Version ───────────────────────────────────────────────────────────────────

// NOTE: this is a hand-maintained duplicate of purrstrap.py's PURROS_VERSION,
// which drives release/bake naming. The two drifted once already (this said
// dp7 for a whole dp8 cycle, so the About page reported the wrong version
// while every baked artifact said otherwise) — bump BOTH, or better, make
// purrstrap generate this header the way it already generates sdkconfig.
#define PURR_KERNEL_VERSION  "1.0.0-dp8"
#define KITT_VERSION         "1.0.0"

// ── Module loader ─────────────────────────────────────────────────────────────

// Called automatically by PURR_MODULE_REGISTER() constructors before app_main.
// Safe to call very early (just appends to a static list).
void purr_kernel_register_module_static(const purr_module_header_t *hdr);

// Load all modules pre-registered via purr_kernel_register_module_static().
// Sorts by priority and calls each module's init().
// P1 modules that fail to init trigger purr_kernel_panic().
// Call from boot.c instead of purr_kernel_scan_modules for built-in modules.
int  purr_kernel_load_static_modules(void);

// Scan a directory for .purr files sorted by priority, with optional SD
// fallback. Intended for SD card extras only — built-in modules use
// purr_kernel_load_static_modules() instead.
int  purr_kernel_scan_modules(const char *flash_dir, const char *sd_fallback_dir);

// Load a single .purr by path (SD card / file-based modules).
int  purr_kernel_load_module(const char *path);

// Unload by name.
void purr_kernel_unload_module(const char *name);

// Get loaded module header by name (NULL if not loaded).
const purr_module_header_t *purr_kernel_get_module(const char *name);

// Enumerate all registered modules (drivers, system, UI, and pre-linked apps).
int purr_kernel_module_count(void);
const purr_module_header_t *purr_kernel_module_at(int idx);

// ── Runtime module enable/disable ───────────────────────────────────────────
// Look up a compiled-in (static) module's header by name, whether or not
// it's currently loaded — the only way to recover a module's init() pointer
// after purr_kernel_unload_module() has removed it from the active
// registry. NULL if no static module with this name exists (e.g. it's a
// file-based/SD module, or the name is wrong).
const purr_module_header_t *purr_kernel_get_static_module(const char *name);

// Re-initializes a previously-unloaded static module and re-adds it to the
// active module registry (purr_kernel_get_module()/module_at() will see it
// again). Idempotent — a no-op returning 0 if already loaded. Returns -1 if
// no static module with this name exists, or if init() failed. No safety
// policy here — see purr_kernel_module_set_enabled() below for that.
int purr_kernel_enable_static_module(const char *name);

typedef enum {
    PURR_MODCTL_OK              = 0,
    PURR_MODCTL_ERR_NOT_FOUND   = -1,
    PURR_MODCTL_ERR_DENYLISTED  = -2,   // refused: app_manager/driver_manager/active UI/a REQUIRED driver
    PURR_MODCTL_ERR_ALREADY     = -3,   // already in the requested state
    PURR_MODCTL_ERR_INIT_FAILED = -4,
} purr_modctl_result_t;

// Enable/disable a module by name, with denylist safety policy applied —
// this, not purr_kernel_unload_module()/purr_kernel_enable_static_module()
// directly, is what the Services app and Terminal app call. Returns a
// purr_modctl_result_t (cast to int for C compatibility).
int purr_kernel_module_set_enabled(const char *name, bool enable);

// Unload-then-reload a single static module (e.g. after a config change).
// Same denylist policy as purr_kernel_module_set_enabled(). On re-enable
// failure, the module ends up unloaded rather than left in an ambiguous
// half-restarted state.
int purr_kernel_module_restart(const char *name);

// ── Catcall registry ──────────────────────────────────────────────────────────
// Modules call these at init to register what they provide.
// The kernel stores one pointer per catcall type. Last registered wins,
// which allows override (e.g. fallback display replaced by real driver).

void purr_kernel_register_display(const catcall_display_t *drv);
void purr_kernel_register_touch  (const catcall_touch_t   *drv);
void purr_kernel_register_input  (const catcall_input_t   *drv);
void purr_kernel_register_radio  (const catcall_radio_t   *drv);
void purr_kernel_register_gps    (const catcall_gps_t     *drv);
void purr_kernel_register_ui     (const catcall_ui_t      *ui);
// Clear a UI registration on unload, so the next backend to load can claim the
// screen. Matched — a module may only clear its OWN registration. Every UI
// backend starts init with "if (purr_kernel_ui()) skip"; without this, a module
// unloaded at runtime (game mode) comes back registered-but-uninitialised: no
// UI rebuilt, no render task, and a UI-unresponsive strike a few seconds later.
// Call it from the backend's deinit().
void purr_kernel_unregister_ui   (const catcall_ui_t      *ui);

// Retrieve registered catcall implementations (NULL if none registered).
const catcall_display_t *purr_kernel_display(void);
const catcall_touch_t   *purr_kernel_touch(void);
const catcall_input_t   *purr_kernel_input(void);       // first registered (legacy)
int                       purr_kernel_input_count(void); // total registered inputs
const catcall_input_t    *purr_kernel_input_at(int idx); // iterate all inputs
const catcall_radio_t   *purr_kernel_radio(void);
const catcall_gps_t     *purr_kernel_gps(void);
const catcall_ui_t      *purr_kernel_ui(void);

// Calls set_backlight() on whichever registered input driver implements it
// (e.g. bbq20) — keeps callers decoupled from which specific input driver
// actually has a backlight, mirroring how brightness goes through
// catcall_display rather than a specific display driver. No-op (returns
// ESP_ERR_NOT_SUPPORTED) if no registered input driver implements it.
esp_err_t purr_kernel_keyboard_set_backlight(uint8_t brightness);

// ── UI thread safety ──────────────────────────────────────────────────────────
// LVGL (and other catcall_ui_t backends) are not safe to call from more than
// one task at a time. The registered UI backend's own render/message-pump
// task must hold this lock for the duration of each pump call; purr_win.h's
// dispatch macros take it automatically around every call into the backend,
// so an app updating its UI from a background task (e.g. a periodic status
// refresh) is safe without doing anything extra. Recursive — safe to
// re-acquire from a widget callback invoked synchronously during the
// backend's own pump call (e.g. a button handler that itself calls
// purr_win_label_set() while the UI task is mid-render).
void purr_kernel_ui_lock(void);
void purr_kernel_ui_unlock(void);

// Call once per iteration from the active UI backend's own render/pump
// loop (right by its purr_kernel_ui_lock()/unlock() pair) — lets the
// kernel's health watchdog detect a fully hung UI task (one that's stopped
// pumping entirely, e.g. deadlocked on a lock it'll never release) and
// trigger purr_crash_guard's hang path. See purr_crash_guard.h.
void purr_kernel_ui_heartbeat(void);

// Optional: record which step of its own pump loop the active UI backend is
// currently inside (a string literal is fine — no copy is taken). Purely
// diagnostic: read back and logged by the health watchdog at the moment it
// detects a hang, so a stuck-forever iteration says *where* it's stuck
// instead of just "unresponsive" — the previous, first-time debugging pass
// on a live "miniwin hangs, cause unknown" report had nothing better to go
// on than that generic message. Cheap enough (one pointer store) to call at
// every step of a pump loop, not just once per iteration like the heartbeat
// above.
void purr_kernel_ui_breadcrumb(const char *step);

// ── Generic liveness watch ──────────────────────────────────────────────────
//
// The UI-hang check above is gated on purr_kernel_ui() being non-NULL, which is
// correct for its purpose and leaves a hole: game mode deliberately UNLOADS the
// UI backend, so that check silently stops running at exactly the moment the
// device is most exposed — one app holding the display and input outright, with
// no UI left to recover to. A hang there is a black brick until someone pulls
// the power.
//
// This is the same mechanism without the UI dependency. An owner declares how
// often it will beat and how many beats may be missed; the existing health
// watchdog task enforces it and routes a failure into purr_crash_guard's hang
// path, exactly as the UI check does — so recovery, strike counting and the
// pending-recovery marker all behave identically.
//
//   purr_kernel_watch_begin("game_mode", 5000, 2);   // beat every 5s, react after 2 missed
//   ... purr_kernel_watch_beat() from the owner's loop ...
//   purr_kernel_watch_end();
//
// Only one watch at a time — this is for whole-device exclusive states, not
// general task supervision (FreeRTOS's own TWDT covers that, and modules
// subscribe to it individually). A second begin() replaces the first.
//
// watch_beat() before begin(), or after end(), is a harmless no-op — a game
// that beats on a frame boundary need not care whether the transition has
// finished yet.
void purr_kernel_watch_begin(const char *owner, uint32_t interval_ms, int missed_beats);
void purr_kernel_watch_beat(void);
void purr_kernel_watch_end(void);

// ── Kernel log tail ───────────────────────────────────────────────────────────
// Captures every ESP_LOG* call system-wide into a small in-RAM scrollback
// buffer — call purr_kernel_klog_init() once, early in boot, then any
// module can pull recent kernel activity for on-screen display (e.g. a
// diagnostics screen) with no serial cable attached. Doesn't replace normal
// serial logging — the original vprintf is still called every time.
void   purr_kernel_klog_init(void);
// Copies up to out_size-1 of the most recent captured log text into out
// (null-terminated). Returns the number of bytes copied.
size_t purr_kernel_klog_tail(char *out, size_t out_size);

// ── System info ───────────────────────────────────────────────────────────────

uint32_t purr_kernel_free_ram(void);
uint64_t purr_kernel_uptime_ms(void);
bool     purr_kernel_sd_available(void);
bool     purr_kernel_wifi_connected(void);
int      purr_kernel_battery_percent(void);  // -1 = unknown (no PMIC/fuel gauge)
int      purr_kernel_battery_voltage_mv(void);  // -1 = unknown
bool     purr_kernel_lora_available(void);
void     purr_kernel_reboot(void);

// Runs fn(arg) on a helper task and waits up to timeout_ms for it to
// finish; returns false on timeout instead of blocking forever. Built for
// bring-up calls that touch a shared SPI bus that may still be wedged
// right after a hang-triggered reboot (see purr_crash_guard.c's pending-
// recovery marker) — without this, a still-bad bus would just hang the
// recovery boot itself the same way. On timeout, fn()'s helper task is
// deliberately left running and its context deliberately leaked — a task
// truly parked inside a blocking SPI call with no software timeout isn't
// blocked on any FreeRTOS primitive and can't be safely reclaimed; the
// alternative (freeing while it might still resume) is worse. Not a
// general-purpose timeout utility — only use this where "the callee might
// never return" is a real, known possibility.
typedef void (*purr_bounded_fn_t)(void *arg);
bool     purr_kernel_run_bounded(const char *label, purr_bounded_fn_t fn,
                                  void *arg, uint32_t timeout_ms);
// True power-down, not a reboot — deep sleep with no wake source
// configured, so the device stays off (minimal draw) until the physical
// EN/reset button power-cycles it. No PMIC-level power cut on any current
// board (none of the detected PMICs — IP5306/AXP2101/MAX17048 — have a
// software power-off path wired up here), so this is the closest thing to
// "off" available across every device uniformly.
void     purr_kernel_shutdown(void);

// ── Mesh backend preference ─────────────────────────────────────────────────
// Which mesh protocol module (meshtastic vs meshcore — mutually exclusive,
// one physical radio) should activate at boot. NVS-backed directly
// (namespace "purr_settings", key "mesh_backend") rather than the RAM-
// cached-and-app-persisted pattern purr_kernel_screen_timeout_min() uses:
// mesh_manager_init()/mc_manager_init() need this value before MSN or
// Settings exist as running apps, so every call reads/writes NVS fresh
// rather than relying on an app having already loaded it into kernel RAM.
typedef enum {
    PURR_MESH_BACKEND_MESHTASTIC = 0,  // default when no preference is stored yet
    PURR_MESH_BACKEND_MESHCORE   = 1,
} purr_mesh_backend_t;

purr_mesh_backend_t purr_kernel_mesh_backend_get(void);
void                 purr_kernel_mesh_backend_set(purr_mesh_backend_t backend);

// Switches the active mesh backend live — no reboot. Persists the
// preference (purr_kernel_mesh_backend_set()) and then stops whichever of
// meshtastic/meshcore is currently running and starts the target, via the
// same purr_kernel_module_set_enabled() choke point Terminal's stop/start
// commands use — see purr_kernel.c's own comment for why that's sufficient
// (the mutual-exclusion guard in each module's init() only needs the other
// to be fully unloaded from the registry, not a reboot). Returns a
// PURR_MODCTL_* result code for the "start target" step.
int purr_kernel_mesh_backend_switch(purr_mesh_backend_t backend);

// Called by SD / WiFi / PMIC / LoRa drivers when their state changes
void     purr_kernel_set_sd_available(bool v);
void     purr_kernel_set_wifi_connected(bool v);
void     purr_kernel_set_battery_percent(int v);
void     purr_kernel_set_battery_voltage_mv(int mv);
void     purr_kernel_set_lora_available(bool v);

// Developer Mode — off by default, toggled from Settings (persisted under
// the "purr_settings" NVS namespace, synced here on settings' own init()).
// app_manager.c's launch_meow() checks this before running an unsigned
// .hiss script (a "-- purr-sig: unsigned" tag, or no tag at all) — signed
// .hiss scripts (dev-signed/trusted-signed/dev-approved) always run
// regardless of this flag; only "unsigned" is gated by it.
bool     purr_kernel_dev_mode_enabled(void);
void     purr_kernel_set_dev_mode(bool v);

// Lollipop nav bar + status bar "always visible" override — off by default
// (auto-hide, the normal Lollipop behavior), toggled from Settings
// (persisted under "purr_settings", synced here on settings' own init(),
// same pattern as Developer Mode above). cupcake_ui.c's lp_hide_navbar()/
// lp_hide_status() both no-op while this is true, so the bars simply never
// auto-hide; no other UI backend reads this yet.
bool     purr_kernel_navbar_always_visible(void);
void     purr_kernel_set_navbar_always_visible(bool v);

// Lock screen notification privacy. When true (the default), the lock screen
// shows only a count — "3 Notifications" — instead of the notification
// contents, and the list is revealed by swiping up on the lock screen. When
// false, the cards are listed immediately.
//
// Defaults to hidden because a lock screen is, by definition, what someone who
// has not authenticated can see: message bodies and sender names should not be
// readable across a desk by default. Settings' Customization panel exposes the
// choice, and both system UI styles honour it.
//
// Set from settings.c on boot (from NVS "purr_settings"/"lock_hide_notifs")
// and on every toggle, same shape as the navbar flag above.
bool     purr_kernel_lock_hide_notifications(void);
void     purr_kernel_set_lock_hide_notifications(bool v);

// ── UI effects (translucency) and the accent colour that replaces them ───────
//
// purr_kernel_ui_effects_enabled() is ON by default — translucent chrome is the
// intended look. When it is turned OFF, every surface that would have been
// translucent becomes fully opaque and is filled with purr_kernel_accent_color()
// instead. Surfaces that were already opaque, and invisible hit-zones that are
// deliberately fully transparent, are unaffected either way.
//
// Two independent reasons this exists, and both matter:
//
//   Legibility. Translucent chrome over an arbitrary user wallpaper has no
//   guaranteed contrast — a busy or light wallpaper can make the status bar and
//   notification text genuinely hard to read.
//
//   Performance. Measured on T-Deck Plus (DP8_CHECKLIST.md): a translucent
//   surface forces whatever is beneath it to be redrawn and then alpha-blended
//   per pixel, which also defeats LVGL's occlusion culling — it can no longer
//   skip what is hidden. With the display driver down to ~13% of frame time,
//   that blending is the dominant remaining cost.
//
// accent_color is 0xRRGGBB. The setter masks to 24 bits because Settings parses
// this from user-entered hex. Backends should read it through
// purr_systemui_fx_bg_opa() in systemui.h rather than reimplementing the
// "translucent or accent?" decision at each site.
//
// Both are set from settings.c on boot (from NVS "purr_settings"/"ui_effects"
// and "accent_color") and on every change, same shape as the flags above.
bool     purr_kernel_ui_effects_enabled(void);
void     purr_kernel_set_ui_effects(bool v);
uint32_t purr_kernel_accent_color(void);
void     purr_kernel_set_accent_color(uint32_t rgb);

// Screen idle timeout, minutes — off by default until Settings loads the
// persisted value (same "purr_settings" NVS namespace, synced on
// settings' own init()), defaulting to 1 minute in the meantime. Only
// Cupcake currently acts on this (cupcake_ui.c's idle check); other UI
// backends don't implement a lock screen yet, same as how
// keyboard_set_backlight only means something on drivers that implement it.
uint8_t  purr_kernel_screen_timeout_min(void);
void     purr_kernel_set_screen_timeout_min(uint8_t v);

// ── Notifications ─────────────────────────────────────────────────────────────
// In-memory ring buffer, open to any module/driver/app. Not persisted across
// reboot — cleared on boot. Newest entry is index 0.

#define PURR_NOTIFY_MAX        32
#define PURR_NOTIFY_TITLE_LEN  32
#define PURR_NOTIFY_BODY_LEN   64
#define PURR_NOTIFY_SOURCE_LEN 16

typedef struct {
    char     title[PURR_NOTIFY_TITLE_LEN];
    char     body[PURR_NOTIFY_BODY_LEN];
    char     source[PURR_NOTIFY_SOURCE_LEN];
    uint64_t timestamp_ms;
} purr_notification_t;

// Post a notification. Safe to call from any module, driver, or app.
void purr_kernel_notify(const char *title, const char *body, const char *source);

// Number of notifications currently held (<= PURR_NOTIFY_MAX).
int  purr_kernel_notify_count(void);

// Fetch notification at idx, newest-first (0 = most recent). Returns false
// if idx is out of range.
bool purr_kernel_notify_at(int idx, purr_notification_t *out);

void purr_kernel_notify_clear(void);

// Remove a single notification by the same newest-first index
// purr_kernel_notify_at() uses, closing the gap so indices stay contiguous.
// Returns false if idx is out of range.
//
// Exists because a UI that shows notifications as individually dismissible
// items (the iOS system UI's swipe-to-clear) otherwise has no way to drop one
// without clearing the lot. Callers iterating while removing should re-read
// purr_kernel_notify_count() after each removal — every index above the one
// removed shifts down by one.
bool purr_kernel_notify_remove(int idx);

// ── Service health registry ───────────────────────────────────────────────────
// Lets any module register a cheap "am I alive" check (e.g. meshtastic's
// heartbeat-staleness test) instead of each one spinning up its own watchdog
// task. A single shared kernel watchdog polls every registered check and
// calls purr_kernel_notify() itself the moment one transitions alive<->dead,
// so a hung/crashed service is surfaced without anything else having to be
// open or watching. The Services app enumerates this same registry to show
// live per-service status.

#define PURR_HEALTH_MAX  16

typedef bool (*purr_health_check_fn)(void);

// `name` is stored by pointer (not copied) and used verbatim as the
// notification source and the Services app's row label — pass a string
// literal or other static/permanent storage, not a stack buffer.
void purr_kernel_health_register(const char *name, purr_health_check_fn is_alive);

int  purr_kernel_health_count(void);

// Fetch registered check idx's name + current live status (calls its
// is_alive() right now — not a cached value). Returns false if idx is out
// of range.
bool purr_kernel_health_at(int idx, const char **name, bool *alive);

// ── App window tracking ───────────────────────────────────────────────────────
// Generic hook so purr_win.h can announce every window it creates without
// knowing app_manager exists — app_manager is the (single) consumer, opting
// in via purr_kernel_set_window_created_cb() at its own init, same pattern as
// the catcall registries above. Lets app_manager associate a running app with
// its purr_win_t so a "Running Apps" UI can show/restore it later, without
// every individual app needing to report its own window handle.

typedef void (*purr_window_created_cb_t)(purr_win_t win);

void purr_kernel_set_window_created_cb(purr_window_created_cb_t cb);

// Called by purr_win_create() — not meant to be called directly by apps.
void purr_kernel_notify_window_created(purr_win_t win);

// ── Memory-pressure watchdog ────────────────────────────────────────────────
// Same registration pattern as purr_kernel_set_window_created_cb() above —
// purr_kernel.c's health_watchdog_task() (already polling every
// HEALTH_WATCHDOG_POLL_MS for UI-hang detection) tracks internal-RAM percent
// used and calls this at the "kill the worst offender" threshold, without
// needing to know app_manager exists or how it picks which app to blame.
// app_manager is the (single) consumer, registering during its own init.
// Returns true if it actually stopped an app (so the watchdog knows whether
// to expect memory back, vs. escalating straight to a full restart because
// there was nothing left to kill).
typedef bool (*purr_mem_pressure_cb_t)(void);

void purr_kernel_set_mem_pressure_cb(purr_mem_pressure_cb_t cb);

// ── Boot readiness ────────────────────────────────────────────────────────────
// Set once by the specialized kernel boot, after every static module/app has
// loaded and app_manager's registry is final. UI modules that build their UI
// from the app registry (e.g. cardstack) must poll this from their own task
// before reading it — their task can run before boot.c finishes, since
// FreeRTOS may preempt into a freshly created higher-priority task.

bool purr_kernel_boot_ready(void);
void purr_kernel_set_boot_ready(bool v);

// ── Panic ─────────────────────────────────────────────────────────────────────

// Kernel panic — logs reason to serial, draws a red panic screen if display
// is up, holds 10s, then reboots. Thin wrapper over purr_kernel_panic_ex()
// with recoverable=false — this is the fatal path, used for Layer-0/P1
// REQUIRED failures. Never returns.
void __attribute__((noreturn)) purr_kernel_panic(const char *reason);

// Full panic screen. recoverable=false is identical to purr_kernel_panic()
// above (entity_name ignored). recoverable=true draws a blue "SUBSYSTEM
// DISABLED" screen instead — no auto-reboot, no dismiss, stays up until the
// user holds the on-screen reset target — used by purr_crash_guard.c once a
// P2/P3 entity (module or app) hits its strike threshold, or hangs outright.
// entity_name is shown on screen and included in the log-dump; may be NULL.
// Never returns.
void __attribute__((noreturn)) purr_kernel_panic_ex(const char *reason, bool recoverable, const char *entity_name);

// Writes the same crash-dump text (entity/reason/uptime/free-mem/reset-
// reason) the panic screen's manual "TAP:DUMP LOGS" button produces, to
// serial always and to /sdcard/crashlog_<uptime>.txt if SD is available —
// but callable directly, without a panic screen or a tap. Used by
// purr_crash_guard_check_reset_reason() to auto-dump once a recovery boot
// has confirmed SD is actually up again. Returns immediately either way.
void purr_kernel_panic_dump_logs(const char *entity_name, const char *reason);

// Red "UI DISABLED >:-(" screen — distinct from both purr_kernel_panic_ex()
// variants above. Fires when the static module loader finds the UI module
// ALREADY disabled (purr_crash_guard_is_disabled(), from a past session's
// strikes) and would otherwise just skip it with a log line — leaving a
// totally dead screen with zero user-visible feedback, since no other
// panic path ever fires in that case (nothing crashed THIS boot, the
// module just never started). Same parked touch-button shell as the
// recoverable variant (dump logs / hold-to-reset) — never auto-reboots,
// since that would loop straight back into the same disabled module every
// ~10s with no way out. entity_name is shown on screen; reason may be
// NULL. Never returns.
void __attribute__((noreturn)) purr_kernel_panic_ui_disabled(const char *entity_name, const char *reason);

// Optional hook a specialized kernel boot registers (e.g.
// kernel_tdeck_plus/kernel_tdp_boot.c, calling usb_msc_share_sd() —
// source/modules/usb_msc/usb_msc.h) so purr_kernel_panic_ex()'s recoverable
// branch can automatically expose the SD card over USB the moment the panic
// screen appears, without purr_kernel.c needing any direct dependency on
// USB/TinyUSB/SD specifics — same registration pattern as
// purr_kernel_set_window_created_cb() above. NULL (never registered) is a
// silent no-op — devices without USB MSC support just don't get this.
void purr_kernel_set_panic_usb_share_cb(void (*cb)(void));

#ifdef __cplusplus
}
#endif
