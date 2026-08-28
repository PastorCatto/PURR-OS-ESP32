// purr_kernel.c — PURR OS kernel spine implementation
//
// Module loading order:
//   1. Collect all .purr headers from flash_dir (read-only scan, no init)
//   2. Sort by load_priority ascending (1=REQUIRED first)
//   3. For each entry attempt load from flash path
//      - If fails AND priority == REQUIRED: try SD fallback, panic if still missing
//      - If fails AND priority == IMPORTANT: log warning, continue
//      - If fails AND priority == OPTIONAL: silent continue
//   4. SD card (/sdcard/modules, /sdcard/drivers) is scanned afterwards for
//      any additional optional modules not present on flash

#include "purr_kernel.h"
#include "purr_crash_guard.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdarg.h>

static const char *TAG = "purr_kernel";

// ── Kernel log tail (ring buffer) ───────────────────────────────────────────
// Captures every ESP_LOG* call system-wide into a small scrollback buffer so
// a diagnostic screen can show recent kernel activity without a serial
// cable attached — built for the Meshtastic diagnostics screen
// (source/apps/system/meshdiag/meshdiag.c), general-purpose otherwise.
// Installed via esp_log_set_vprintf(), which every ESP-IDF log call already
// routes through — the original vprintf (normally the UART writer) is kept
// and still called every time, so serial logging is completely unaffected.

#define KLOG_BUF_SIZE 4096
static char              *s_klog_buf  = NULL;
static size_t              s_klog_len = 0;
static portMUX_TYPE        s_klog_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t      s_klog_orig_vprintf = NULL;

static int klog_vprintf(const char *fmt, va_list args) {
    char line[256];
    va_list args_copy;
    va_copy(args_copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, args_copy);
    va_end(args_copy);

    if (n > 0 && s_klog_buf) {
        size_t tlen = (size_t)n;
        if (tlen >= sizeof(line)) tlen = sizeof(line) - 1;   // vsnprintf truncated
        portENTER_CRITICAL(&s_klog_lock);
        if (s_klog_len + tlen >= KLOG_BUF_SIZE - 1) {
            // Scroll: drop the oldest half rather than the newest data —
            // same pattern as meshchat.c's chat-log scrollback.
            size_t half = KLOG_BUF_SIZE / 2;
            size_t keep = s_klog_len > half ? s_klog_len - half : 0;
            memmove(s_klog_buf, s_klog_buf + (s_klog_len - keep), keep);
            s_klog_len = keep;
        }
        memcpy(s_klog_buf + s_klog_len, line, tlen);
        s_klog_len += tlen;
        s_klog_buf[s_klog_len] = '\0';
        portEXIT_CRITICAL(&s_klog_lock);
    }

    return s_klog_orig_vprintf ? s_klog_orig_vprintf(fmt, args) : 0;
}

void purr_kernel_klog_init(void) {
    if (s_klog_buf) return;   // already installed
    s_klog_buf = heap_caps_malloc(KLOG_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_klog_buf) return;
    s_klog_buf[0] = '\0';
    s_klog_orig_vprintf = esp_log_set_vprintf(klog_vprintf);
}

size_t purr_kernel_klog_tail(char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    if (!s_klog_buf) { out[0] = '\0'; return 0; }
    portENTER_CRITICAL(&s_klog_lock);
    size_t n = (s_klog_len < out_size - 1) ? s_klog_len : (out_size - 1);
    size_t start = s_klog_len - n;
    memcpy(out, s_klog_buf + start, n);
    portEXIT_CRITICAL(&s_klog_lock);
    out[n] = '\0';
    return n;
}

// ── Catcall registry ──────────────────────────────────────────────────────────

static const catcall_display_t *s_display = NULL;
static const catcall_touch_t   *s_touch   = NULL;
#define MAX_INPUTS 4
static const catcall_input_t   *s_inputs[MAX_INPUTS];
static int                      s_input_count = 0;
static const catcall_radio_t   *s_radio   = NULL;
static const catcall_gps_t     *s_gps     = NULL;
static const catcall_ui_t      *s_ui      = NULL;

void purr_kernel_register_display(const catcall_display_t *drv) {
    s_display = drv;
    ESP_LOGI(TAG, "display registered: %s", drv ? drv->name : "null");
}
void purr_kernel_register_touch(const catcall_touch_t *drv) {
    s_touch = drv;
    ESP_LOGI(TAG, "touch registered: %s", drv ? drv->name : "null");
}
void purr_kernel_register_input(const catcall_input_t *drv) {
    if (!drv) return;
    if (s_input_count < MAX_INPUTS) {
        s_inputs[s_input_count++] = drv;
    }
    ESP_LOGI(TAG, "input registered: %s (%d total)", drv->name, s_input_count);
}
void purr_kernel_register_radio(const catcall_radio_t *drv) {
    s_radio = drv;
    ESP_LOGI(TAG, "radio registered: %s", drv ? drv->name : "null");
}
void purr_kernel_register_gps(const catcall_gps_t *drv) {
    s_gps = drv;
    ESP_LOGI(TAG, "gps registered: %s", drv ? drv->name : "null");
}
void purr_kernel_register_ui(const catcall_ui_t *ui) {
    s_ui = ui;
    ESP_LOGI(TAG, "ui registered: %s", ui ? ui->name : "null");
}

// Drop a UI registration on unload. MATCHED, not unconditional: only the module
// that actually owns the current registration may clear it, so a late or
// duplicated deinit cannot wipe a backend that has since taken over.
//
// This exists because unloading a UI module left s_ui pointing at it forever,
// and every UI backend begins init with "if (purr_kernel_ui()) skip — something
// else owns the screen". Speed demon therefore restored a backend that
// immediately short-circuited: the kernel marked the module loaded, but no UI
// was rebuilt and no render task was started. The crash guard caught it six
// seconds later as "UI TASK UNRESPONSIVE @ idle", which is a true statement
// about a task that had never been created.
//
// Both backends carry that identical guard, which is why the failure reproduced
// on Mochi and Cupcake alike — it was never a backend bug.
void purr_kernel_unregister_ui(const catcall_ui_t *ui) {
    if (!ui || s_ui != ui) return;
    s_ui = NULL;
    ESP_LOGI(TAG, "ui unregistered: %s", ui->name);
}

const catcall_display_t *purr_kernel_display(void) { return s_display; }
const catcall_touch_t   *purr_kernel_touch(void)   { return s_touch; }
const catcall_input_t   *purr_kernel_input(void)    { return s_input_count > 0 ? s_inputs[0] : NULL; }
int                       purr_kernel_input_count(void) { return s_input_count; }
const catcall_input_t    *purr_kernel_input_at(int i)   { return (i >= 0 && i < s_input_count) ? s_inputs[i] : NULL; }
const catcall_radio_t   *purr_kernel_radio(void)   { return s_radio; }
const catcall_gps_t     *purr_kernel_gps(void)     { return s_gps; }
const catcall_ui_t      *purr_kernel_ui(void)      { return s_ui; }

esp_err_t purr_kernel_keyboard_set_backlight(uint8_t brightness) {
    for (int i = 0; i < s_input_count; i++) {
        if (s_inputs[i] && s_inputs[i]->set_backlight) {
            esp_err_t ret = s_inputs[i]->set_backlight(brightness);
            ESP_LOGI("purr_kernel", "keyboard_set_backlight(%u) -> %s (%s)",
                     brightness, esp_err_to_name(ret), s_inputs[i]->name);
            return ret;
        }
    }
    ESP_LOGW("purr_kernel", "keyboard_set_backlight(%u): no input with set_backlight registered (count=%d)",
             brightness, s_input_count);
    return ESP_ERR_NOT_SUPPORTED;
}

// ── App window tracking ───────────────────────────────────────────────────────

static purr_window_created_cb_t s_window_created_cb = NULL;

void purr_kernel_set_window_created_cb(purr_window_created_cb_t cb) {
    s_window_created_cb = cb;
}
void purr_kernel_notify_window_created(purr_win_t win) {
    if (s_window_created_cb) s_window_created_cb(win);
}

static purr_mem_pressure_cb_t s_mem_pressure_cb = NULL;

void purr_kernel_set_mem_pressure_cb(purr_mem_pressure_cb_t cb) {
    s_mem_pressure_cb = cb;
}

static void (*s_panic_usb_share_cb)(void) = NULL;

void purr_kernel_set_panic_usb_share_cb(void (*cb)(void)) {
    s_panic_usb_share_cb = cb;
}

// ── UI thread safety ──────────────────────────────────────────────────────────
//
// LVGL (and any other catcall_ui_t backend) is not safe to call from two
// tasks at once. The UI backend's own task holds this for each render/
// message-pump call; purr_win.h's dispatch macros hold it around every call
// into the backend, so an app's background task (e.g. a periodic status
// updater) can't race the render loop. Created lazily on first use — the
// first call always happens single-threaded during a module's synchronous
// init() on the boot task, before any UI task is spun up, so there's no
// init race in practice.
//
// Recursive because a widget event callback (e.g. a button handler) runs
// synchronously from inside the backend's own pump call, on the same task
// that's already holding the lock — if that callback calls back into
// purr_win_* (settings.c's on_theme_wce does exactly this via
// purr_win_label_set()), a non-recursive mutex would deadlock against itself.
static SemaphoreHandle_t s_ui_mutex = NULL;

void purr_kernel_ui_lock(void) {
    if (!s_ui_mutex) s_ui_mutex = xSemaphoreCreateRecursiveMutex();
    if (s_ui_mutex) xSemaphoreTakeRecursive(s_ui_mutex, portMAX_DELAY);
}

void purr_kernel_ui_unlock(void) {
    if (s_ui_mutex) xSemaphoreGiveRecursive(s_ui_mutex);
}

// ── Module registry ───────────────────────────────────────────────────────────

#define MAX_MODULES 32

typedef struct {
    purr_module_header_t header;
    bool                 loaded;
} module_slot_t;

static module_slot_t s_modules[MAX_MODULES];
static int           s_module_count = 0;

// Recursive for the same reason s_ui_mutex above is: purr_kernel_module_set_
// enabled()/_restart() (further down) call the lower-level accessors below
// while already holding this lock, and a plain mutex would deadlock against
// itself. Lazy-created on first use — mirrors purr_kernel_ui_lock()'s exact
// pattern. Protects s_modules[]/s_module_count only; s_static_reg[] (below)
// is populated once, at boot, before app_main() via C constructors, and
// never mutated afterward, so it doesn't need locking.
static SemaphoreHandle_t s_module_registry_mutex = NULL;

static void module_registry_lock(void) {
    if (!s_module_registry_mutex) s_module_registry_mutex = xSemaphoreCreateRecursiveMutex();
    if (s_module_registry_mutex) xSemaphoreTakeRecursive(s_module_registry_mutex, portMAX_DELAY);
}
static void module_registry_unlock(void) {
    if (s_module_registry_mutex) xSemaphoreGiveRecursive(s_module_registry_mutex);
}

// ── Version comparison ────────────────────────────────────────────────────────
// Exported (was file-static) for ota_mgr's manifest-version check — same
// "MAJOR.MINOR.PATCH" comparison a module's kernel_min/kernel_max already
// gets checked against below, reused rather than re-implemented.

int purr_kernel_version_cmp(const char *a, const char *b) {
    unsigned ma=0,mi=0,pa=0, mb=0,mib=0,pb=0;
    sscanf(a, "%u.%u.%u", &ma, &mi, &pa);
    sscanf(b, "%u.%u.%u", &mb, &mib, &pb);
    if (ma != mb) return ma < mb ? -1 : 1;
    if (mi != mib) return mi < mib ? -1 : 1;
    if (pa != pb) return pa < pb ? -1 : 1;
    return 0;
}

// Returns the s_modules[] index already holding this name, or -1. Used to
// detect the same module present in both /flash and /sdcard (or an SD-card
// update of a module already loaded from flash) before appending a second
// copy — see purr_kernel_load_module().
static int find_module_slot_by_name(const char *name) {
    for (int i = 0; i < s_module_count; i++) {
        if (strncmp(s_modules[i].header.name, name, sizeof(s_modules[i].header.name)) == 0) {
            return i;
        }
    }
    return -1;
}

// ── Panic ─────────────────────────────────────────────────────────────────────
//
// Two variants, sharing one text-rendering core built ONLY on
// catcall_display_t::fill_rect() and catcall_touch_t (the only primitives
// those catcalls expose) — deliberately no LVGL/MiniWin/app_manager
// dependency, since the whole point of both screens is that they must
// still work when the thing that's broken is a P2/P3 module, including the
// active UI backend itself. See purr_crash_guard.h for the guard that
// decides when the recoverable (blue) variant fires.
//
// Font: a compact 3x5 dot-matrix, uppercase + digits + a minimal
// punctuation set only (not the full mixed-case ASCII range) — hand-
// authoring bitmap glyph data carries real risk of unnoticed bit errors
// with no way to visually proof each one before flashing, so the character
// set is deliberately kept small and every string is force-uppercased
// before rendering. Unsupported characters fall back to a blank glyph.

typedef struct { char c; uint8_t rows[5]; } panic_glyph_t;

// Each row's 3 bits = left,mid,right column (bit2=left). Every entry below
// was authored from an explicit 3x5 on/off grid, not transcribed from
// memory of an existing font table.
static const panic_glyph_t PANIC_FONT[] = {
    {' ', {0x0,0x0,0x0,0x0,0x0}},
    {'!', {0x2,0x2,0x2,0x0,0x2}},
    {'(', {0x3,0x4,0x4,0x4,0x3}},
    {')', {0x6,0x1,0x1,0x1,0x6}},
    {',', {0x0,0x0,0x0,0x2,0x4}},
    {'-', {0x0,0x0,0x7,0x0,0x0}},
    {'.', {0x0,0x0,0x0,0x0,0x2}},
    {'/', {0x1,0x1,0x2,0x4,0x4}},
    {':', {0x0,0x2,0x0,0x2,0x0}},
    {'_', {0x0,0x0,0x0,0x0,0x7}},
    {'0', {0x7,0x5,0x5,0x5,0x7}},
    {'1', {0x2,0x6,0x2,0x2,0x7}},
    {'2', {0x7,0x1,0x7,0x4,0x7}},
    {'3', {0x7,0x1,0x7,0x1,0x7}},
    {'4', {0x5,0x5,0x7,0x1,0x1}},
    {'5', {0x7,0x4,0x7,0x1,0x7}},
    {'6', {0x7,0x4,0x7,0x5,0x7}},
    {'7', {0x7,0x1,0x2,0x2,0x2}},
    {'8', {0x7,0x5,0x7,0x5,0x7}},
    {'9', {0x7,0x5,0x7,0x1,0x7}},
    {'>', {0x4,0x2,0x1,0x2,0x4}},
    {'A', {0x2,0x5,0x7,0x5,0x5}},
    {'B', {0x6,0x5,0x6,0x5,0x6}},
    {'C', {0x3,0x4,0x4,0x4,0x3}},
    {'D', {0x6,0x5,0x5,0x5,0x6}},
    {'E', {0x7,0x4,0x7,0x4,0x7}},
    {'F', {0x7,0x4,0x7,0x4,0x4}},
    {'G', {0x3,0x4,0x5,0x5,0x3}},
    {'H', {0x5,0x5,0x7,0x5,0x5}},
    {'I', {0x7,0x2,0x2,0x2,0x7}},
    {'J', {0x1,0x1,0x1,0x5,0x2}},
    {'K', {0x5,0x5,0x6,0x5,0x5}},
    {'L', {0x4,0x4,0x4,0x4,0x7}},
    {'M', {0x5,0x7,0x5,0x5,0x5}},
    {'N', {0x5,0x7,0x7,0x7,0x5}},
    {'O', {0x7,0x5,0x5,0x5,0x7}},
    {'P', {0x7,0x5,0x7,0x4,0x4}},
    {'Q', {0x7,0x5,0x5,0x7,0x1}},
    {'R', {0x7,0x5,0x6,0x5,0x5}},
    {'S', {0x3,0x4,0x7,0x1,0x6}},
    {'T', {0x7,0x2,0x2,0x2,0x2}},
    {'U', {0x5,0x5,0x5,0x5,0x7}},
    {'V', {0x5,0x5,0x5,0x5,0x2}},
    {'W', {0x5,0x5,0x7,0x7,0x5}},
    {'X', {0x5,0x5,0x2,0x5,0x5}},
    {'Y', {0x5,0x5,0x2,0x2,0x2}},
    {'Z', {0x7,0x1,0x2,0x4,0x7}},
};
#define PANIC_FONT_COUNT (sizeof(PANIC_FONT) / sizeof(PANIC_FONT[0]))

#define PANIC_SCALE   3
#define PANIC_CHAR_W  (3 * PANIC_SCALE + PANIC_SCALE)   // glyph + 1-cell gap
#define PANIC_CHAR_H  (5 * PANIC_SCALE + PANIC_SCALE)   // glyph + 1-line gap

static const panic_glyph_t *panic_glyph_for(char c)
{
    for (size_t i = 0; i < PANIC_FONT_COUNT; i++)
        if (PANIC_FONT[i].c == c) return &PANIC_FONT[i];
    return &PANIC_FONT[0];   // blank/unsupported
}

static void panic_draw_char(const catcall_display_t *disp, int x, int y, char c, uint16_t color)
{
    char up = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    const panic_glyph_t *g = panic_glyph_for(up);
    for (int row = 0; row < 5; row++) {
        uint8_t bits = g->rows[row];
        for (int col = 0; col < 3; col++) {
            if (bits & (0x4u >> col)) {
                disp->fill_rect(x + col * PANIC_SCALE, y + row * PANIC_SCALE,
                                 PANIC_SCALE, PANIC_SCALE, color);
            }
        }
    }
}

// Left-aligned at (x,y), hard-wraps every max_chars_per_line characters
// (character-count wrap, not word-aware — fine for short diagnostic
// strings) and on '\n'. Returns the y just below the last line, so callers
// can stack further text underneath.
static int panic_draw_string(const catcall_display_t *disp, int x, int y,
                              const char *s, uint16_t color, int max_chars_per_line)
{
    if (max_chars_per_line < 1) max_chars_per_line = 1;
    int col = 0, cx = x, cy = y;
    for (const char *p = s; s && *p; p++) {
        if (*p == '\n' || col >= max_chars_per_line) {
            cx = x; cy += PANIC_CHAR_H; col = 0;
            if (*p == '\n') continue;
        }
        panic_draw_char(disp, cx, cy, *p, color);
        cx += PANIC_CHAR_W;
        col++;
    }
    return cy + PANIC_CHAR_H;
}

static void panic_dump_logs(const char *entity_name, const char *reason)
{
    char buf[512];
    snprintf(buf, sizeof(buf),
        "PURR OS crash dump\n"
        "entity: %s\n"
        "reason: %s\n"
        "uptime_ms: %llu\n"
        "free_internal: %u\n"
        "free_psram: %u\n"
        "reset_reason: %d\n",
        entity_name ? entity_name : "(unknown)",
        reason ? reason : "(unknown)",
        (unsigned long long)purr_kernel_uptime_ms(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (int)esp_reset_reason());

    ESP_LOGE(TAG, "%s", buf);

    // SD if present (matches "both, and serial only if no SD card"), always
    // also to serial above regardless.
    if (purr_kernel_sd_available()) {
        char path[64];
        snprintf(path, sizeof(path), "/sdcard/crashlog_%llu.txt",
                 (unsigned long long)purr_kernel_uptime_ms());
        FILE *f = fopen(path, "w");
        if (f) {
            fwrite(buf, 1, strlen(buf), f);
            fclose(f);
            ESP_LOGI(TAG, "crash dump written to %s", path);
        } else {
            ESP_LOGW(TAG, "crash dump: failed to open %s for write", path);
        }
    }
}

// Public wrapper — panic_dump_logs() itself stays static/unchanged (its
// only other call site is the manual "TAP:DUMP LOGS" panic-screen button
// below). This lets purr_crash_guard_check_reset_reason() trigger the same
// dump automatically once a recovery boot has confirmed SD is actually
// available again, without touching that existing tap-to-dump call site.
void purr_kernel_panic_dump_logs(const char *entity_name, const char *reason)
{
    panic_dump_logs(entity_name, reason);
}

// Three visual states share one rendering core (built ONLY on
// catcall_display_t/catcall_touch_t — no LVGL/MiniWin dependency, since
// this must still work when the broken thing IS the active UI backend):
//   PANIC_KIND_FATAL       — red, ":-(", 10s countdown then reboot. P1
//                            REQUIRED failures (purr_kernel_panic()).
//   PANIC_KIND_RECOVERABLE — blue, ":-(", parks with touch buttons
//                            (dump logs / hold-to-reset). A P2/P3 entity
//                            just hit its strike threshold or hung —
//                            purr_crash_guard.c's normal path.
//   PANIC_KIND_UI_DISABLED — red, ">:-(", same parked touch-button shell
//                            as RECOVERABLE (never auto-reboots — that
//                            would just loop straight back into the same
//                            disabled module every ~10s with no way out).
//                            Fires when the static module loader finds the
//                            UI module already disabled from a PAST
//                            session's strikes and skips it outright —
//                            previously silent (a log line only), leaving
//                            a totally dead screen with zero user-visible
//                            feedback. Angrier face than RECOVERABLE on
//                            purpose: this isn't a fresh strike, it's the
//                            fully-exhausted state.
typedef enum { PANIC_KIND_FATAL, PANIC_KIND_RECOVERABLE, PANIC_KIND_UI_DISABLED } panic_kind_t;

static void __attribute__((noreturn)) panic_render(panic_kind_t kind, const char *reason, const char *entity_name)
{
    bool fatal = (kind == PANIC_KIND_FATAL);
    bool red   = (kind != PANIC_KIND_RECOVERABLE);

    // Always log to serial first — display may not be up.
    const char *kind_tag = kind == PANIC_KIND_FATAL ? "" :
                           kind == PANIC_KIND_RECOVERABLE ? " (recoverable)" : " (UI disabled)";
    ESP_LOGE(TAG, "=== KERNEL PANIC%s ===", kind_tag);
    if (entity_name && entity_name[0]) ESP_LOGE(TAG, "entity: %s", entity_name);
    ESP_LOGE(TAG, "%s", reason ? reason : "(no reason given)");
    ESP_LOGE(TAG, "System halted.");

    const catcall_display_t *disp = s_display;
    uint16_t w = 320, h = 240;
    if (disp) {
        display_info_t info = {0};
        if (disp->get_info) disp->get_info(&info);
        w = info.width  ? info.width  : 320;
        h = info.height ? info.height : 240;

        uint16_t bg = red ? 0xD800u /* red */ : 0x001Fu /* blue */;
        disp->fill_rect(0, 0, w, h, bg);
        disp->fill_rect(0, 0, w, 24, 0xFFFFu);   // white header bar

        const char *header = kind == PANIC_KIND_FATAL       ? "PURR OS - KERNEL PANIC :-(" :
                             kind == PANIC_KIND_RECOVERABLE ? "SUBSYSTEM DISABLED :-("     :
                                                               "UI DISABLED >:-(";

        int max_cols = (int)((w - 8) / PANIC_CHAR_W);
        int y = 4;
        y = panic_draw_string(disp, 4, y, header, 0x0000u, max_cols);
        y += PANIC_SCALE;
        if (entity_name && entity_name[0]) {
            y = panic_draw_string(disp, 4, y, entity_name, 0xFFFFu, max_cols);
        }
        y = panic_draw_string(disp, 4, y, reason ? reason : "UNKNOWN REASON", 0xFFFFu, max_cols);
    }

    if (fatal) {
        // Fatal — same shape the historic KITT 0.6.0 panic screen used:
        // hold long enough to actually read, then reboot. purr_kernel_reboot()
        // (below) already exists and is reused, not reimplemented.
        for (int s = 10; s > 0; s--) {
            if (disp) {
                char line[24];
                snprintf(line, sizeof(line), "RESTARTING IN %d...", s);
                disp->fill_rect(4, h - 28, w - 8, 24, 0xD800u);
                panic_draw_string(disp, 4, h - 28, line, 0xFFFFu, (int)((w - 8) / PANIC_CHAR_W));
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        purr_kernel_reboot();
    }

    // Recoverable (blue): no auto-reboot, no dismiss — sits here until the
    // user forces a reset, per design. Raw touch polling only (no
    // LVGL/MiniWin dependency — must work even when the disabled entity IS
    // the active UI backend).

    // Share the SD card over USB automatically, the moment the panic screen
    // appears — no button tap required (see purr_kernel_set_panic_usb_share_
    // cb()'s doc comment). Everything else the OS would be doing with the
    // card is already halted by this point, so there's no "still in use"
    // race to worry about.
    bool usb_shared = false;
    if (s_panic_usb_share_cb) {
        s_panic_usb_share_cb();
        usb_shared = true;
    }

    int btn_h   = 48;
    int btn_y   = h - btn_h - 8;
    int dump_x  = 4;
    int dump_w  = (w - 12) / 2;
    int reset_x = dump_x + dump_w + 4;
    int reset_w = dump_w;

    if (disp) {
        if (usb_shared) {
            panic_draw_string(disp, 4, btn_y - PANIC_CHAR_H - 4, "SD SHARED VIA USB", 0xFFFFu,
                               (int)((w - 8) / PANIC_CHAR_W));
        }
        disp->fill_rect(dump_x, btn_y, dump_w, btn_h, 0x0000u);
        panic_draw_string(disp, dump_x + 4, btn_y + 6, "TAP:DUMP LOGS", 0xFFFFu,
                           (int)((dump_w - 8) / PANIC_CHAR_W));
        disp->fill_rect(reset_x, btn_y, reset_w, btn_h, 0x0000u);
        panic_draw_string(disp, reset_x + 4, btn_y + 6, "HOLD:RESET", 0xFFFFu,
                           (int)((reset_w - 8) / PANIC_CHAR_W));
    }

    const catcall_touch_t *touch = s_touch;
    uint32_t hold_start_ms = 0;
    bool holding_reset = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (!touch || !touch->is_pressed || !touch->is_pressed()) {
            holding_reset = false;
            continue;
        }
        uint16_t tx = 0, ty = 0;
        if (touch->read_point) touch->read_point(&tx, &ty);

        bool in_reset = (int)tx >= reset_x && (int)tx < reset_x + reset_w &&
                         (int)ty >= btn_y  && (int)ty < btn_y + btn_h;
        bool in_dump  = (int)tx >= dump_x  && (int)tx < dump_x + dump_w &&
                         (int)ty >= btn_y  && (int)ty < btn_y + btn_h;

        if (in_reset) {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (!holding_reset) { holding_reset = true; hold_start_ms = now_ms; }
            else if (now_ms - hold_start_ms >= 2000) {
                purr_kernel_reboot();
            }
        } else {
            holding_reset = false;
            if (in_dump) {
                panic_dump_logs(entity_name, reason);
                // Debounce: wait for release so one tap doesn't dump repeatedly.
                while (touch->is_pressed && touch->is_pressed()) vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
    }
}

void __attribute__((noreturn)) purr_kernel_panic_ex(const char *reason, bool recoverable, const char *entity_name)
{
    panic_render(recoverable ? PANIC_KIND_RECOVERABLE : PANIC_KIND_FATAL, reason, entity_name);
}

void __attribute__((noreturn)) purr_kernel_panic_ui_disabled(const char *entity_name, const char *reason)
{
    panic_render(PANIC_KIND_UI_DISABLED, reason, entity_name);
}

void __attribute__((noreturn)) purr_kernel_panic(const char *reason)
{
    purr_kernel_panic_ex(reason, /*recoverable=*/false, NULL);
}

// ── Module header peek (no init, no registration) ─────────────────────────────

static bool peek_module_header(const char *path, purr_module_header_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(out, 1, sizeof(*out), f);
    fclose(f);
    return n >= sizeof(*out) && out->magic == PURR_MODULE_MAGIC;
}

// ── Static (constructor) module registration ──────────────────────────────────
//
// PURR_MODULE_REGISTER() emits a __attribute__((constructor)) function for each
// module. These run before app_main, filling s_static_reg[]. When boot.c calls
// purr_kernel_load_static_modules(), we sort by priority and init each one.

#define MAX_STATIC_REG 64
static const purr_module_header_t *s_static_reg[MAX_STATIC_REG];
static int s_static_reg_count = 0;

void purr_kernel_register_module_static(const purr_module_header_t *hdr)
{
    if (s_static_reg_count < MAX_STATIC_REG) {
        s_static_reg[s_static_reg_count++] = hdr;
    } else {
        // Can't call ESP_LOG this early — just silently drop (shouldn't happen)
    }
}

// Generic bound for any P2/P3 module's init() during a post-hang recovery
// boot — covers the radio (sx1262_rl) without any radio-specific plumbing
// here, since it's just "whichever guarded module happens to touch the
// still-possibly-wedged SPI bus." Starting point, needs real-hardware
// tuning.
#define MODULE_INIT_TIMEOUT_MS 5000UL

typedef struct {
    const purr_module_header_t *hdr;
    int                          rc;
} module_init_ctx_t;

static void run_module_init(void *arg)
{
    module_init_ctx_t *c = (module_init_ctx_t *)arg;
    c->rc = c->hdr->init();
}

// Caller must hold module_registry_lock() — called both from boot
// (purr_kernel_load_static_modules(), before the mutex could possibly have
// any contention) and from purr_kernel_enable_static_module() (runtime,
// where contention is the whole reason the lock exists).
//
// recovering: true only when this boot is recovering from a hang-
// triggered reboot (see purr_crash_guard_pending_recovery()) — gates
// whether hdr->init() runs bounded (purr_kernel_run_bounded()) or exactly
// as it always has. purr_kernel_enable_static_module() (the runtime
// re-enable path) always passes false — this is boot-sequence-only by
// design. Never applied to P1/REQUIRED modules (guarded below is already
// false for those) — a required module's init hang is a different,
// unrelated scenario and stays fully blocking.
static int load_one_static(const purr_module_header_t *hdr, bool recovering)
{
    if (s_module_count >= MAX_MODULES) {
        ESP_LOGE(TAG, "module table full, cannot load %s", hdr->name);
        return -1;
    }
    if (hdr->magic != PURR_MODULE_MAGIC) {
        ESP_LOGE(TAG, "bad magic in static module '%s'", hdr->name);
        return -1;
    }
    if (hdr->abi_version != PURR_MODULE_ABI_VERSION) {
        ESP_LOGE(TAG, "ABI mismatch: '%s' (module=%d kernel=%d)",
                 hdr->name, hdr->abi_version, PURR_MODULE_ABI_VERSION);
        return -1;
    }

    // Apps are registered in the module table for the app_manager to discover,
    // but their init() is NOT called at boot — the app_manager launches them.
    bool call_init = (hdr->module_type != PURR_MOD_APP);

    // Crash-loop guard applies to P2/P3 only — P1 REQUIRED already panics
    // immediately on the very first failure (below, in the caller), so
    // there's no "loop" to catch there; a P1 module that keeps failing
    // stops the boot outright every time regardless of this guard.
    bool guarded = call_init && hdr->load_priority != PURR_PRIORITY_REQUIRED;
    if (guarded && purr_crash_guard_is_disabled(hdr->name)) {
        ESP_LOGW(TAG, "static module '%s' disabled after repeated failures — skipping", hdr->name);
        // The UI module specifically gets a red screen, not just a log
        // line — every other guarded module silently skips (a background
        // driver/service going quiet isn't user-visible the way losing
        // the entire UI is), but the UI is the one case where "just
        // skip it" leaves a totally dead, unexplained screen. Noreturn:
        // halts the boot right here rather than continuing to load other
        // P2/P3 modules into a device that will never show anything.
        if (hdr->module_type == PURR_MOD_UI) {
            purr_kernel_panic_ui_disabled(hdr->name, "disabled after repeated failures");
        }
        return -1;
    }

    if (call_init && hdr->init) {
        if (guarded) purr_crash_guard_mark_start(hdr->name);
        int rc;
        if (recovering && guarded) {
            module_init_ctx_t *ictx = (module_init_ctx_t *)calloc(1, sizeof(*ictx));
            if (ictx) {
                ictx->hdr = hdr;
                ictx->rc  = -1;
                bool ok = purr_kernel_run_bounded(hdr->name, run_module_init, ictx, MODULE_INIT_TIMEOUT_MS);
                rc = ok ? ictx->rc : -1;
                // Deliberately NOT freed on timeout — see purr_kernel_run_bounded()'s
                // doc comment; the (possibly still-running) helper task may write
                // into ictx->rc later, harmlessly, since nothing reads it again.
                if (ok) free(ictx);
            } else {
                rc = hdr->init();   // alloc failed — fall back to unbounded rather than skip
            }
        } else {
            rc = hdr->init();
        }
        bool declined = (rc == PURR_MODULE_INIT_DECLINED);
        // A decline isn't a crash-loop symptom — see PURR_MODULE_INIT_DECLINED's
        // doc comment. Report it to the guard as "ok" (clears the mark_start()
        // breadcrumb, records no strike) rather than as a failure — the module
        // returned control normally, it just chose not to fully start.
        if (guarded) purr_crash_guard_mark_stop(hdr->name, rc == 0 || declined, "init() failed");
        if (rc != 0) {
            if (declined) {
                ESP_LOGI(TAG, "static module '%s' declined to start", hdr->name);
            } else {
                ESP_LOGE(TAG, "static module '%s' init() returned %d", hdr->name, rc);
            }
            return -1;
        }
    }
    module_slot_t *slot = &s_modules[s_module_count++];
    memcpy(&slot->header, hdr, sizeof(*hdr));
    slot->loaded = call_init;
    const char *prio = hdr->load_priority == PURR_PRIORITY_REQUIRED  ? "P1" :
                       hdr->load_priority == PURR_PRIORITY_IMPORTANT ? "P2" : "P3";
    const char *badge = call_init ? "[static]" : "[app/deferred]";
    ESP_LOGI(TAG, "loaded %s: %s v%s %s", prio, hdr->name, hdr->version, badge);
    return 0;
}

// Look up a compiled-in (static) module's header by name, whether or not
// it's currently loaded — s_static_reg holds every PURR_MODULE_REGISTER()'d
// module regardless of enable/disable state. NULL if no static module with
// this name exists (e.g. it's a file-based/SD module, or the name is wrong).
// s_static_reg is populated once at boot (before app_main) and never
// mutated afterward, so this doesn't need module_registry_lock().
const purr_module_header_t *purr_kernel_get_static_module(const char *name)
{
    for (int i = 0; i < s_static_reg_count; i++)
        if (strcmp(s_static_reg[i]->name, name) == 0)
            return s_static_reg[i];
    return NULL;
}

// Re-run load_one_static() for a single static module by name — the
// re-enable counterpart to purr_kernel_unload_module(). No safety/denylist
// policy here (see purr_kernel_module_set_enabled() below for that) — this
// is the bare mechanism, reused by boot (indirectly, via
// purr_kernel_load_static_modules()) and by the policy wrapper alike.
int purr_kernel_enable_static_module(const char *name)
{
    module_registry_lock();
    if (purr_kernel_get_module(name)) {   // already loaded — idempotent, no double-init
        module_registry_unlock();
        return 0;
    }
    const purr_module_header_t *hdr = purr_kernel_get_static_module(name);
    if (!hdr) {
        module_registry_unlock();
        return -1;
    }
    int rc = load_one_static(hdr, /*recovering=*/false);
    module_registry_unlock();
    return rc;
}

static int cmp_reg_priority(const void *a, const void *b)
{
    const purr_module_header_t *ha = *(const purr_module_header_t **)a;
    const purr_module_header_t *hb = *(const purr_module_header_t **)b;
    // Primary: load_priority (1=REQUIRED first). Secondary: module_type (DRIVER < SYSTEM < UI < APP).
    // An UNSET load_priority (0) must sort LAST, not first — a module that
    // forgot to declare one used to jump ahead of the P1 display driver
    // (miniwin, before it declared P2) and fail against missing catcalls.
    int pa = ha->load_priority ? ha->load_priority : PURR_PRIORITY_OPTIONAL + 1;
    int pb = hb->load_priority ? hb->load_priority : PURR_PRIORITY_OPTIONAL + 1;
    int ka = pa * 10 + (int)ha->module_type;
    int kb = pb * 10 + (int)hb->module_type;
    return ka - kb;
}

// One-time pause between the P2 (IMPORTANT — display/UI backend/app_manager/
// wifi_mgr) and P3 (OPTIONAL — proximity/pairing/meshtastic/meshcore, and
// every deferred user app: msn, nearby, services, settings, ...) load
// tiers. UI is P2 and should come up as fast as possible; P3 is everything
// else racing to start radio tasks and spin up their own background
// pollers on top of a UI that's barely rendered its first frame yet.
// Confirmed live: opening the Nearby app moments after boot could freeze
// cupcake's own render task long enough to trip the "UI TASK UNRESPONSIVE"
// hang-watchdog (see purr_crash_guard.h) — giving the UI a couple of
// uncontested seconds first, before P3 modules/apps start piling on,
// avoids racing it against everything spinning up at once.
// 2500 -> 500 (2026-07-28). The reason above still stands; the amount it needs
// to buy does not.
//
// 2500ms was chosen when the UI was genuinely starved — internal DRAM sat at
// ~1-2KB within seconds of boot, and a full-screen redraw took ~150ms. The DP8
// performance pass changed both sides of that: -O2 (~3.3x on frame time),
// asynchronous flush with double buffering, off-screen compose, and the idle
// repaint fix (the system UI was rewriting the whole status row five times a
// second whether or not anything had changed).
//
// The two hangs this was papering over were also root-caused since, and neither
// was contention: the UI catcall was never released on unload, and the display
// driver's SPI bus was held across an async return. Both fixed.
//
// Reported as "the OS hangs for a few seconds on boot" — which it did, visibly,
// with the progress bar parked wherever P2 ended. Kept rather than removed
// because the original failure was timing-dependent and only appeared when
// someone opened an app in the first moment after boot; 500ms still gives the
// UI a clear head start. If that race ever returns, this is the first place to
// look, and raising it is a one-line change.
// Overridable per device via device.pcat's [device] boot_settle_ms — purrstrap
// passes it through to a -D on the main component. The right value depends on
// what a build actually contains: a minimal image with no Nearby/MSN/mesh has
// nothing racing the UI at P3, so it can drop to ~50ms, while a full image
// wants the head start. A single global constant cannot express that.
#ifndef BOOT_SETTLE_MS
#define BOOT_SETTLE_MS 500UL
#endif

// Defined further down, next to the state it writes. See its own comment for
// why the kernel loads these rather than the settings app.
static void kernel_load_persisted_settings(void);
// Defined next to purr_kernel_time_set() — seeds the coarse NVS fallback
// (PURR_TIME_SOURCE_NVS) before any module, including wifi_mgr/generic_nmea,
// gets a chance to sync a fresher one.
static void kernel_load_persisted_time(void);

int purr_kernel_load_static_modules(void)
{
    // Before the sort, and therefore before any module's init() runs. Hooked
    // here rather than in each kernel_*_boot.c so that every device — generic
    // core and specialised kernels alike — gets it without a per-board edit.
    kernel_load_persisted_settings();
    kernel_load_persisted_time();

    qsort(s_static_reg, s_static_reg_count,
          sizeof(s_static_reg[0]), cmp_reg_priority);

    // Computed once, not per-module — this boot is either recovering from
    // a hang or it isn't; re-reading NVS per module would be redundant and
    // could only ever flip mid-loop if purr_crash_guard_clear_pending_
    // recovery() were called concurrently, which it isn't (that only
    // happens after this whole function returns — see kernel_tdp_boot.c).
    bool recovering = purr_crash_guard_pending_recovery(NULL, 0, NULL, 0);
    if (recovering) {
        ESP_LOGW(TAG, "recovering from a hang-triggered reboot — bounding P2/P3 module init this boot");
    }

    int n = s_static_reg_count;
    uint8_t prev_priority = 0;
    for (int i = 0; i < n; i++) {
        const purr_module_header_t *hdr = s_static_reg[i];

        if (hdr->load_priority == PURR_PRIORITY_OPTIONAL && prev_priority != PURR_PRIORITY_OPTIONAL) {
            ESP_LOGI(TAG, "boot settle: pausing %lu ms before P3/OPTIONAL modules", BOOT_SETTLE_MS);
            vTaskDelay(pdMS_TO_TICKS(BOOT_SETTLE_MS));
        }
        prev_priority = hdr->load_priority;

        module_registry_lock();
        int rc = load_one_static(hdr, recovering);
        module_registry_unlock();
        if (rc != 0 && hdr->load_priority == PURR_PRIORITY_REQUIRED) {
            char reason[96];
            snprintf(reason, sizeof(reason),
                     "Required module '%.32s' failed to init.", hdr->name);
            purr_kernel_panic(reason);
        }
    }
    ESP_LOGI(TAG, "static module load: %d/%d initialised", s_module_count, n);
    return s_module_count;
}

// ── File-based loader (SD card extras) ───────────────────────────────────────

int purr_kernel_load_module(const char *path)
{
    purr_module_header_t hdr;
    if (!peek_module_header(path, &hdr)) {
        ESP_LOGE(TAG, "cannot read/validate header: %s", path);
        return -1;
    }

    if (hdr.abi_version != PURR_MODULE_ABI_VERSION) {
        ESP_LOGE(TAG, "ABI mismatch in %s (module=%d kernel=%d)",
                 path, hdr.abi_version, PURR_MODULE_ABI_VERSION);
        return -1;
    }

    if (hdr.kernel_min[0] && purr_kernel_version_cmp(KITT_VERSION, hdr.kernel_min) < 0) {
        ESP_LOGE(TAG, "module %s requires kernel >= %s (running %s)",
                 hdr.name, hdr.kernel_min, KITT_VERSION);
        return -1;
    }

    bool compat_mode = false;
    if (hdr.kernel_max[0] && purr_kernel_version_cmp(KITT_VERSION, hdr.kernel_max) > 0) {
        ESP_LOGW(TAG, "module %s: kernel %s > kernel_max %s [COMPAT]",
                 hdr.name, KITT_VERSION, hdr.kernel_max);
        compat_mode = true;
    }

    // Same name may already be loaded — e.g. present in both /flash and
    // /sdcard, or an SD-card update of a module loaded earlier from flash.
    // Highest version wins; loading the same module twice would double-
    // register its catcall and run init() twice.
    module_slot_t *slot;
    int existing = find_module_slot_by_name(hdr.name);
    if (existing >= 0) {
        if (purr_kernel_version_cmp(hdr.version, s_modules[existing].header.version) <= 0) {
            ESP_LOGI(TAG, "module '%s' v%s already loaded (have v%s) — skipping %s",
                     hdr.name, hdr.version, s_modules[existing].header.version, path);
            return 0;
        }
        ESP_LOGI(TAG, "module '%s': v%s supersedes loaded v%s — reloading from %s",
                 hdr.name, hdr.version, s_modules[existing].header.version, path);
        if (s_modules[existing].header.deinit) s_modules[existing].header.deinit();
        slot = &s_modules[existing];
    } else {
        if (s_module_count >= MAX_MODULES) {
            ESP_LOGE(TAG, "module table full, cannot load %s", path);
            return -1;
        }
        slot = &s_modules[s_module_count++];
    }

    if (hdr.init) {
        int rc = hdr.init();
        if (rc != 0) {
            ESP_LOGE(TAG, "module %s init() returned %d", hdr.name, rc);
            return -1;
        }
    }

    memcpy(&slot->header, &hdr, sizeof(hdr));
    slot->loaded = true;

    const char *badge = compat_mode ? " [COMPAT]" : " [OK]";
    const char *prio  = hdr.load_priority == PURR_PRIORITY_REQUIRED  ? "P1" :
                        hdr.load_priority == PURR_PRIORITY_IMPORTANT ? "P2" : "P3";
    ESP_LOGI(TAG, "loaded %s: %s v%s%s", prio, hdr.name, hdr.version, badge);
    return 0;
}

// ── Priority-sorted scan entry ────────────────────────────────────────────────

#define MAX_SCAN_ENTRIES 64

typedef struct {
    char                 path[512];
    uint8_t              priority;      // from peeked header
    uint8_t              module_type;   // PURR_MOD_* from peeked header
    char                 name[32];
} scan_entry_t;

static int scan_entry_cmp(const void *a, const void *b)
{
    const scan_entry_t *sa = (const scan_entry_t *)a;
    const scan_entry_t *sb = (const scan_entry_t *)b;
    int ka = (int)sa->priority * 10 + (int)sa->module_type;
    int kb = (int)sb->priority * 10 + (int)sb->module_type;
    return ka - kb;
}

// Collect all .purr files in dir (non-recursive for this level).
static int collect_dir(const char *dir, scan_entry_t *entries, int max_entries, int count)
{
    DIR *d = opendir(dir);
    if (!d) return count;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max_entries) {
        const char *ext = strrchr(ent->d_name, '.');
        if (!ext || strcmp(ext, ".purr") != 0) continue;
        scan_entry_t *e = &entries[count];
        snprintf(e->path, sizeof(e->path), "%s/%s", dir, ent->d_name);
        purr_module_header_t hdr = {0};
        if (peek_module_header(e->path, &hdr)) {
            e->priority    = hdr.load_priority ? hdr.load_priority : PURR_PRIORITY_OPTIONAL;
            e->module_type = hdr.module_type   ? hdr.module_type   : PURR_MOD_SYSTEM;
            strncpy(e->name, hdr.name, sizeof(e->name) - 1);
        } else {
            e->priority    = PURR_PRIORITY_OPTIONAL;
            e->module_type = PURR_MOD_SYSTEM;
            strncpy(e->name, ent->d_name, sizeof(e->name) - 1);
        }
        count++;
    }
    closedir(d);
    return count;
}

// Also recurse one level into subdirectories (for drivers/<type>/)
static int collect_dir_recursive(const char *dir, scan_entry_t *entries, int max, int count)
{
    count = collect_dir(dir, entries, max, count);
    DIR *d = opendir(dir);
    if (!d) return count;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max) {
        if (ent->d_type != DT_DIR || ent->d_name[0] == '.') continue;
        char sub[512];
        snprintf(sub, sizeof(sub), "%s/%s", dir, ent->d_name);
        count = collect_dir(sub, entries, max, count);
    }
    closedir(d);
    return count;
}

// ── Public scan + load ────────────────────────────────────────────────────────

int purr_kernel_scan_modules(const char *flash_dir, const char *sd_fallback_dir)
{
    // scan_entry_t's path[512] makes this 64-entry array ~34KB — by far the
    // single biggest static consumer of this board's already-scarce internal
    // SRAM (see the memory-pressure investigation this was found in), for a
    // function only ever called 2-3 times total, at boot, from boot.c/
    // kernel_*_boot.c. A PSRAM allocation scoped to this call recovers all
    // ~34KB for the rest of the device's runtime instead of reserving it
    // permanently as a `static` would.
    scan_entry_t *entries = heap_caps_malloc(sizeof(scan_entry_t) * MAX_SCAN_ENTRIES, MALLOC_CAP_SPIRAM);
    if (!entries) {
        ESP_LOGE(TAG, "scan %s: PSRAM alloc failed for %d scan_entry_t (%u bytes)",
                 flash_dir, MAX_SCAN_ENTRIES, (unsigned)(sizeof(scan_entry_t) * MAX_SCAN_ENTRIES));
        return 0;
    }
    int count = 0;

    count = collect_dir_recursive(flash_dir, entries, MAX_SCAN_ENTRIES, 0);

    if (count == 0) {
        ESP_LOGW(TAG, "scan: no .purr files found in %s", flash_dir);
    }

    // Sort by priority — REQUIRED (1) loaded before IMPORTANT (2) before OPTIONAL (3)
    qsort(entries, count, sizeof(scan_entry_t), scan_entry_cmp);

    int loaded = 0;
    for (int i = 0; i < count; i++) {
        scan_entry_t *e = &entries[i];
        const char *prio_str = e->priority == PURR_PRIORITY_REQUIRED  ? "P1:REQUIRED " :
                               e->priority == PURR_PRIORITY_IMPORTANT ? "P2:IMPORTANT" : "P3:OPTIONAL ";
        ESP_LOGI(TAG, "  %s  %s", prio_str, e->name[0] ? e->name : e->path);

        int rc = purr_kernel_load_module(e->path);
        if (rc == 0) {
            loaded++;
            continue;
        }

        // Load from flash failed
        if (e->priority == PURR_PRIORITY_REQUIRED) {
            // Try SD fallback
            bool recovered = false;
            if (sd_fallback_dir) {
                char fallback[512];
                // Try flat: /sdcard/modules/<name>.purr
                snprintf(fallback, sizeof(fallback), "%s/%s.purr", sd_fallback_dir, e->name);
                if (purr_kernel_load_module(fallback) == 0) {
                    ESP_LOGW(TAG, "P1 module '%s' recovered from SD card", e->name);
                    recovered = true;
                    loaded++;
                }
                // Try the same relative subpath as on flash
                if (!recovered) {
                    // Extract filename from path and try sd_fallback_dir/<filename>
                    const char *fname = strrchr(e->path, '/');
                    if (fname) fname++;
                    if (fname) {
                        snprintf(fallback, sizeof(fallback), "%s/%s", sd_fallback_dir, fname);
                        if (purr_kernel_load_module(fallback) == 0) {
                            ESP_LOGW(TAG, "P1 module '%s' recovered from SD card (by filename)", e->name);
                            recovered = true;
                            loaded++;
                        }
                    }
                }
            }

            if (!recovered) {
                char reason[96];
                snprintf(reason, sizeof(reason),
                         "Required module '%.32s' missing.",
                         e->name[0] ? e->name : "unknown");
                purr_kernel_panic(reason);
                // never returns
            }

        } else if (e->priority == PURR_PRIORITY_IMPORTANT) {
            ESP_LOGW(TAG, "P2 module '%s' failed to load — continuing without it",
                     e->name[0] ? e->name : e->path);
        }
        // OPTIONAL: silent
    }

    ESP_LOGI(TAG, "scan %s: %d/%d modules loaded", flash_dir, loaded, count);
    heap_caps_free(entries);
    return loaded;
}

void purr_kernel_unload_module(const char *name)
{
    module_registry_lock();
    for (int i = 0; i < s_module_count; i++) {
        if (strcmp(s_modules[i].header.name, name) == 0) {
            if (s_modules[i].header.deinit) s_modules[i].header.deinit();
            s_modules[i] = s_modules[--s_module_count];
            ESP_LOGI(TAG, "unloaded: %s", name);
            break;
        }
    }
    module_registry_unlock();
}

const purr_module_header_t *purr_kernel_get_module(const char *name)
{
    module_registry_lock();
    const purr_module_header_t *found = NULL;
    for (int i = 0; i < s_module_count; i++) {
        if (strcmp(s_modules[i].header.name, name) == 0) {
            found = &s_modules[i].header;
            break;
        }
    }
    module_registry_unlock();
    return found;
}

// ── Runtime module enable/disable policy ────────────────────────────────────
// Single choke point both the Services app and the Terminal app call for
// user-driven enable/disable/restart, so the safety policy (denylist) lives
// in exactly one place rather than being duplicated/drifting across callers.
//
// load_priority alone is NOT a reliable "safe to disable" signal — of every
// module registered via PURR_MODULE_REGISTER() in this tree, only
// display/touch drivers set PURR_PRIORITY_REQUIRED; app_manager,
// driver_manager, and the miniwin UI backend never set .load_priority at
// all (defaults to 0 — neither IMPORTANT nor OPTIONAL). A priority-only
// check would let a user disable app_manager or the active UI and hard-lock
// the device. Denylist by name instead, plus whichever UI backend is
// actually registered right now. wifi_mgr/bt_mgr are deliberately NOT
// denylisted (explicit product decision) despite Meshtastic's BLE companion
// (mesh_ble_init()) depending on bt_mgr's NimBLE stack — accepted tradeoff.
static bool module_is_denylisted(const purr_module_header_t *hdr)
{
    if (!hdr) return true;
    if (hdr->load_priority == PURR_PRIORITY_REQUIRED) return true;

    static const char *const s_denylist[] = { "app_manager", "driver_manager" };
    for (size_t i = 0; i < sizeof(s_denylist) / sizeof(s_denylist[0]); i++)
        if (strcmp(hdr->name, s_denylist[i]) == 0) return true;

    const catcall_ui_t *ui = purr_kernel_ui();
    if (ui && ui->name && strcmp(hdr->name, ui->name) == 0) return true;

    return false;
}

int purr_kernel_module_set_enabled(const char *name, bool enable)
{
    if (!name || !*name) return PURR_MODCTL_ERR_NOT_FOUND;

    module_registry_lock();
    const purr_module_header_t *static_hdr = purr_kernel_get_static_module(name);
    const purr_module_header_t *live_hdr   = purr_kernel_get_module(name);
    const purr_module_header_t *hdr = static_hdr ? static_hdr : live_hdr;
    if (!hdr) { module_registry_unlock(); return PURR_MODCTL_ERR_NOT_FOUND; }
    if (module_is_denylisted(hdr)) { module_registry_unlock(); return PURR_MODCTL_ERR_DENYLISTED; }

    int result;
    if (enable) {
        if (live_hdr) {
            result = PURR_MODCTL_ERR_ALREADY;
        } else if (!static_hdr) {
            // File-based (SD .purr) module with no static-registry entry —
            // the kernel doesn't track a loaded module's origin path, so
            // there's no way to re-enable it once unloaded. Out of scope
            // for this feature (static modules only), not a bug.
            result = PURR_MODCTL_ERR_NOT_FOUND;
        } else {
            result = (purr_kernel_enable_static_module(name) == 0)
                    ? PURR_MODCTL_OK : PURR_MODCTL_ERR_INIT_FAILED;
        }
    } else {
        if (!live_hdr) {
            result = PURR_MODCTL_ERR_ALREADY;
        } else {
            purr_kernel_unload_module(name);
            result = PURR_MODCTL_OK;
        }
    }
    module_registry_unlock();
    return result;
}

int purr_kernel_module_restart(const char *name)
{
    if (!name || !*name) return PURR_MODCTL_ERR_NOT_FOUND;

    module_registry_lock();
    const purr_module_header_t *static_hdr = purr_kernel_get_static_module(name);
    if (!static_hdr) {   // restart only meaningful for static modules
        module_registry_unlock();
        return PURR_MODCTL_ERR_NOT_FOUND;
    }
    if (module_is_denylisted(static_hdr)) {
        module_registry_unlock();
        return PURR_MODCTL_ERR_DENYLISTED;
    }

    if (purr_kernel_get_module(name)) purr_kernel_unload_module(name);
    // Fail-safe on re-enable failure: the module ends up unloaded (a known,
    // "off" state) rather than left in an ambiguous half-restarted state.
    int result = (purr_kernel_enable_static_module(name) == 0)
                ? PURR_MODCTL_OK : PURR_MODCTL_ERR_INIT_FAILED;
    module_registry_unlock();
    return result;
}

int purr_kernel_module_count(void)
{
    module_registry_lock();
    int n = s_module_count;
    module_registry_unlock();
    return n;
}

const purr_module_header_t *purr_kernel_module_at(int idx)
{
    module_registry_lock();
    const purr_module_header_t *hdr = (idx < 0 || idx >= s_module_count) ? NULL : &s_modules[idx].header;
    module_registry_unlock();
    return hdr;
}

// ── System info ───────────────────────────────────────────────────────────────

uint32_t purr_kernel_free_ram(void) {
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
}

uint64_t purr_kernel_uptime_ms(void) {
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

static bool s_sd_available    = false;
static bool s_wifi_connected  = false;
static int  s_battery_percent = -1;   // -1 = unknown (no PMIC/fuel gauge found)
static int  s_battery_voltage_mv = -1;   // -1 = unknown
static bool s_lora_available  = false;
static bool s_dev_mode        = false;   // off by default — see purr_kernel.h's doc comment
static bool s_navbar_always_visible = false;   // off by default — see purr_kernel.h's doc comment
// Privacy-by-default: a lock screen is what an unauthenticated onlooker sees,
// so contents stay hidden behind a count until deliberately revealed.
static bool s_lock_hide_notifications = true;
// Translucency/blur across the system UI. ON by default — it is the intended
// look. Turning it off is both an accessibility choice and a real performance
// one: measured on T-Deck Plus, per-pixel alpha blending of translucent chrome
// over the wallpaper dominates frame time (see DP8_CHECKLIST.md), because a
// translucent surface forces whatever is beneath it to be redrawn and blended
// rather than skipped.
static bool     s_ui_effects   = true;
// Colour every translucent surface collapses to when effects are off. 0xRRGGBB.
// Default is the iOS-ish blue-grey the chrome already reads as, so switching
// effects off changes the texture without changing the palette.
static uint32_t s_accent_color = 0x1C1C2E;
// Default 1 minute, not 0 — a 0 timeout would make cupcake_ui.c's
// "elapsed_ms >= timeout_min * 60000" idle check true on every tick,
// locking the screen in a permanent loop. Settings overwrites this from
// NVS the first time it's opened this session, same lazy-load pattern
// brightness already uses.
static uint8_t s_screen_timeout_min = 1;
// See purr_kernel.h's own doc comment — light (false) by default, matching
// real iOS 7's own light-first default.
static bool     s_dark_mode = false;

void purr_kernel_set_sd_available(bool v)    { s_sd_available    = v; }
void purr_kernel_set_wifi_connected(bool v)  { s_wifi_connected  = v; }
void purr_kernel_set_battery_percent(int v)  { s_battery_percent = v; }
void purr_kernel_set_battery_voltage_mv(int mv) { s_battery_voltage_mv = mv; }
void purr_kernel_set_lora_available(bool v)  { s_lora_available  = v; }
void purr_kernel_set_dev_mode(bool v)        { s_dev_mode        = v; }
void purr_kernel_set_navbar_always_visible(bool v) { s_navbar_always_visible = v; }
void purr_kernel_set_lock_hide_notifications(bool v) { s_lock_hide_notifications = v; }
void purr_kernel_set_ui_effects(bool v)      { s_ui_effects = v; }
// Masked to 24 bits: the setting is an RGB colour, and letting a stray high
// byte through would silently become garbage the moment anything treats it as
// ARGB. Settings parses user-entered hex, so this is a real input path.
void purr_kernel_set_accent_color(uint32_t rgb) { s_accent_color = rgb & 0x00FFFFFFu; }
void purr_kernel_set_screen_timeout_min(uint8_t v) { s_screen_timeout_min = v; }
void purr_kernel_set_dark_mode(bool v)             { s_dark_mode = v; }

bool purr_kernel_sd_available(void)    { return s_sd_available; }
bool purr_kernel_wifi_connected(void)  { return s_wifi_connected; }
int  purr_kernel_battery_percent(void) { return s_battery_percent; }
int  purr_kernel_battery_voltage_mv(void) { return s_battery_voltage_mv; }
bool purr_kernel_lora_available(void)  { return s_lora_available; }

// ── Wall-clock time ──────────────────────────────────────────────────────────
// See purr_kernel.h's doc comment for the source-authority model. State here
// is deliberately just "epoch at last accept + uptime_ms at that moment" —
// purr_kernel_time_now() extrapolates from it rather than a live libc clock,
// so there's nothing to keep ticking in the background and nothing that can
// drift out of sync with purr_kernel_uptime_ms()'s own esp_timer source.
#define TIME_NVS_NS  "purr_time"
#define TIME_NVS_KEY "epoch"

// How long a source's reading stays "fresh" enough to block a lower-
// authority source from overwriting it. 6h: long enough that a routine
// hourly NTP resync (CONFIG_LWIP_SNTP_UPDATE_DELAY) never lapses into
// "stale" between updates, short enough that a genuinely gone network lets
// GPS start carrying the clock well within the same day.
#define PURR_TIME_FRESH_WINDOW_MS (6ULL * 60 * 60 * 1000)

static time_t              s_wall_time_epoch    = 0;   // UTC seconds; 0 = unsynced
static purr_time_source_t  s_wall_time_source    = PURR_TIME_SOURCE_NONE;
static uint64_t            s_wall_time_set_at_ms = 0;   // uptime_ms() at last accept

bool purr_kernel_time_is_synced(void) { return s_wall_time_source != PURR_TIME_SOURCE_NONE; }

time_t purr_kernel_time_now(void) {
    if (s_wall_time_source == PURR_TIME_SOURCE_NONE) return 0;
    uint64_t elapsed_ms = purr_kernel_uptime_ms() - s_wall_time_set_at_ms;
    return s_wall_time_epoch + (time_t)(elapsed_ms / 1000);
}

purr_time_source_t purr_kernel_time_source(void) { return s_wall_time_source; }

void purr_kernel_time_set(purr_time_source_t source, time_t epoch_utc) {
    if (source == PURR_TIME_SOURCE_NONE || epoch_utc <= 0) return;

    uint64_t now_ms = purr_kernel_uptime_ms();
    bool current_fresh = s_wall_time_source != PURR_TIME_SOURCE_NONE &&
                          (now_ms - s_wall_time_set_at_ms) < PURR_TIME_FRESH_WINDOW_MS;
    if (source < s_wall_time_source && current_fresh) return;

    s_wall_time_epoch     = epoch_utc;
    s_wall_time_source    = source;
    s_wall_time_set_at_ms = now_ms;

    // NVS loading itself back in (purr_kernel_time_load_nvs(), at boot) would
    // otherwise be a pointless write-back of the value it just read.
    if (source != PURR_TIME_SOURCE_NVS) {
        nvs_handle_t h;
        if (nvs_open(TIME_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_i64(h, TIME_NVS_KEY, (int64_t)epoch_utc);
            nvs_commit(h);
            nvs_close(h);
        }
    }
}

// See the forward declaration up in purr_kernel_load_static_modules() for
// why this runs before any module — including wifi_mgr and generic_nmea —
// gets a chance to call purr_kernel_time_set() with something fresher.
static void kernel_load_persisted_time(void) {
    nvs_handle_t h;
    if (nvs_open(TIME_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;   // namespace absent (first boot after an erase, or never synced) — stays unset
    }
    int64_t v = 0;
    esp_err_t err = nvs_get_i64(h, TIME_NVS_KEY, &v);
    nvs_close(h);
    if (err == ESP_OK && v > 0) {
        purr_kernel_time_set(PURR_TIME_SOURCE_NVS, (time_t)v);
        ESP_LOGI(TAG, "time: seeded from NVS fallback (epoch=%lld)", (long long)v);
    }
}

time_t purr_kernel_time_from_utc_calendar(uint16_t year, uint8_t month, uint8_t day,
                                           uint8_t hour, uint8_t minute, uint8_t second) {
    // days_from_civil (Howard Hinnant — howardhinnant.github.io/date_algorithms.html),
    // chosen over struct tm + mktime() specifically to avoid depending on the
    // libc TZ globals: mktime() treats its input as LOCAL time, and this
    // kernel never sets a TZ, so relying on it would make every caller's
    // "UTC in" silently correct only as long as TZ stays unset. Pure integer
    // math instead — correct for any UTC calendar date, GPS epoch (1980) and
    // on.
    int y = (int)year - (month <= 2 ? 1 : 0);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);                                   // [0, 399]
    unsigned doy = (153u * (month + (month > 2 ? -3 : 9)) + 2u) / 5u + day - 1u; // [0, 365]
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;                     // [0, 146096]
    long days = (long)era * 146097L + (long)doe - 719468L;   // 719468 = 0000-03-01 -> 1970-01-01

    return (time_t)(days * 86400L + (long)hour * 3600L + (long)minute * 60L + (long)second);
}
bool purr_kernel_dev_mode_enabled(void) { return s_dev_mode; }
bool purr_kernel_navbar_always_visible(void) { return s_navbar_always_visible; }
bool purr_kernel_lock_hide_notifications(void) { return s_lock_hide_notifications; }
bool     purr_kernel_ui_effects_enabled(void) { return s_ui_effects; }
uint32_t purr_kernel_accent_color(void)       { return s_accent_color; }
bool     purr_kernel_dark_mode_enabled(void)  { return s_dark_mode; }

// Load the kernel's own persisted settings, BEFORE any module initialises.
// Forward-declared up at purr_kernel_load_static_modules(), which calls it.
//
// These values used to be pushed into the kernel by settings.c's nvs_load().
// That is the wrong layer, and it produced a real, visible bug: settings is a
// PURR_PRIORITY_OPTIONAL app, so it initialises AFTER the UI module
// (PURR_PRIORITY_IMPORTANT). System UI therefore built every surface against
// the compiled-in defaults, and a stored preference only took hold once
// something happened to rebuild them — which is exactly why the symptom was
// "wrong after a fresh flash, correct again once the device sleeps and wakes".
// Toggling the setting off and back on by hand papered over it the same way.
//
// ui_effects is what exposed it, because it is read at CONSTRUCTION time rather
// than per frame. Every value below shared the race; the others were just
// harder to see.
//
// settings.c still reads the same keys for its own widget state. That
// duplication is deliberate: this owns what the KERNEL needs before modules
// run, settings owns what its UI displays, and neither depends on the other's
// load order.
static void kernel_load_persisted_settings(void)
{
    nvs_handle_t h;
    if (nvs_open("purr_settings", NVS_READONLY, &h) != ESP_OK) {
        return;   // namespace absent (first boot after an erase) — defaults stand
    }
    uint8_t v;
    if (nvs_get_u8(h, "ui_effects",            &v) == ESP_OK) s_ui_effects              = (v != 0);
    if (nvs_get_u8(h, "lock_hide_notifs",      &v) == ESP_OK) s_lock_hide_notifications = (v != 0);
    if (nvs_get_u8(h, "navbar_always_visible", &v) == ESP_OK) s_navbar_always_visible   = (v != 0);
    if (nvs_get_u8(h, "dev_mode",              &v) == ESP_OK) s_dev_mode                = (v != 0);
    if (nvs_get_u8(h, "screen_timeout",        &v) == ESP_OK) s_screen_timeout_min      = v;
    if (nvs_get_u8(h, "dark_mode",              &v) == ESP_OK) s_dark_mode               = (v != 0);

    // Stored as the same "RRGGBB" text the user types into Settings. Trusted
    // only when the read succeeds AND returns the 6 characters we wrote —
    // strtoul() on an untouched buffer would parse whatever was on the stack.
    char   hex[8] = {0};
    size_t hlen   = sizeof(hex);
    if (nvs_get_str(h, "accent_color", hex, &hlen) == ESP_OK && hlen >= 7) {
        s_accent_color = (uint32_t)strtoul(hex, NULL, 16) & 0x00FFFFFFu;
    }
    nvs_close(h);

    ESP_LOGI(TAG, "settings: effects=%s accent=#%06lX notifs=%s timeout=%umin",
             s_ui_effects ? "on" : "off", (unsigned long)s_accent_color,
             s_lock_hide_notifications ? "hidden" : "shown",
             (unsigned)s_screen_timeout_min);
}
uint8_t purr_kernel_screen_timeout_min(void) { return s_screen_timeout_min; }

void purr_kernel_reboot(void) {
    ESP_LOGW(TAG, "kernel reboot requested");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

// ── Bounded-timeout execution — see purr_kernel.h's doc comment ────────────

typedef struct {
    purr_bounded_fn_t fn;
    void             *arg;
    SemaphoreHandle_t done;
} bounded_ctx_t;

static void bounded_trampoline(void *arg)
{
    bounded_ctx_t *ctx = (bounded_ctx_t *)arg;
    ctx->fn(ctx->arg);
    // Give and get out — do NOT touch ctx again after this. Confirmed
    // live this session as a real, hit-in-practice race: this task can
    // run on the OTHER core the instant xTaskCreateWithCaps() returns,
    // and if fn() finishes fast (the SD-mount case did), this task could
    // reach xSemaphoreGive()+delete+free BEFORE the waiter in
    // purr_kernel_run_bounded() ever executes its own xSemaphoreTake()
    // call — which would then be taking an already-deleted semaphore on
    // already-freed memory. That use-after-free is what corrupted a
    // spinlock and crashed with "assert failed: spinlock_acquire
    // (lock->count == 0)" inside xSemaphoreTake() itself. Cleanup now
    // belongs entirely to the waiter (below), and only after it has
    // actually taken the semaphore — never to this task.
    xSemaphoreGive(ctx->done);
    // vTaskDeleteWithCaps, NOT vTaskDelete — this task was created with
    // xTaskCreateWithCaps(), and ESP-IDF requires the matching deleter. Plain
    // vTaskDelete() does not release a caps-allocated stack and TCB, so every
    // bounded call leaked 8192 bytes of INTERNAL DRAM plus its TCB.
    //
    // Measured on hardware with per-module accounting during a game-mode round
    // trip. One unload consistently reported ALLOCATING ~8540 bytes rather than
    // freeing anything, and it attached to a different module on each run —
    // which is what gave it away: it was never the module, it was this task.
    //
    // This is the bulk of the ~9KB lost per game-mode cycle, the leak that made
    // the third consecutive run exhaust the device and panic.
    vTaskDeleteWithCaps(NULL);
}

bool purr_kernel_run_bounded(const char *label, purr_bounded_fn_t fn, void *arg, uint32_t timeout_ms)
{
    bounded_ctx_t *ctx = (bounded_ctx_t *)calloc(1, sizeof(*ctx));
    if (ctx) ctx->done = xSemaphoreCreateBinary();
    if (!ctx || !ctx->done) {
        // Can't set up the bounded path — fall back to running inline
        // rather than silently skipping fn() altogether.
        ESP_LOGW(TAG, "run_bounded: alloc failed for '%s', running inline (unbounded)", label ? label : "?");
        if (ctx) free(ctx);
        fn(arg);
        return true;
    }
    ctx->fn  = fn;
    ctx->arg = arg;

    // Internal RAM stack, not PSRAM — fn() may itself touch NVS (e.g. a
    // module init() calling purr_crash_guard_mark_start()/mark_stop()),
    // and a PSRAM-backed stack executing while flash cache is briefly
    // disabled is a confirmed hard crash on this codebase (see
    // purr_crash_guard.c's worker_task() doc comment for the same
    // constraint).
    //
    // 8192 bytes, not a smaller guess — confirmed live this session that
    // 4096 was NOT enough: wrapping st7789_drv_init() (GPIO+SPI init plus
    // ESP_LOG formatting calls, several stack frames deep) overflowed a
    // 4096-byte stack, crashing the helper task before it could ever
    // clear the pending-recovery flag — which meant every subsequent boot
    // re-entered the same bounded path and crashed again, forever (an
    // actual infinite reboot loop, only broken by erasing NVS by hand).
    // fn() here can be ANY P2/P3 module's init() too (load_one_static()
    // reuses this same helper), not just the display — 8192 matches this
    // codebase's own convention for tasks that do comparably heavy init
    // work (cupcake_task, mesh_task both use 8192-byte stacks).
    // xTaskCreateWithCaps()'s stack parameter is BYTES on ESP-IDF, not
    // words like vanilla FreeRTOS — worth stating explicitly since that
    // mismatch is exactly what caused the original bug.
    TaskHandle_t task = NULL;
    xTaskCreateWithCaps(bounded_trampoline, "bounded", 8192, ctx, 5, &task, MALLOC_CAP_INTERNAL);
    if (!task) {
        ESP_LOGW(TAG, "run_bounded: task create failed for '%s', running inline (unbounded)", label ? label : "?");
        vSemaphoreDelete(ctx->done);
        free(ctx);
        fn(arg);
        return true;
    }

    if (xSemaphoreTake(ctx->done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        // Only the waiter tears down, and only here — after the
        // semaphore is actually taken, so bounded_trampoline is
        // guaranteed to be past its xSemaphoreGive() call (it never
        // touches ctx again after that point, see its own comment).
        vSemaphoreDelete(ctx->done);
        free(ctx);
        return true;
    }

    // Timeout — fn()'s task may be genuinely, permanently stuck (this
    // session's confirmed failure mode: a blocking SPI call with no
    // software timeout). Deliberately leak ctx/done rather than free them
    // here — if the task ever does resume, bounded_trampoline still needs
    // them. See purr_kernel.h's doc comment.
    ESP_LOGE(TAG, "run_bounded: '%s' did not complete within %lu ms — leaving it running, continuing",
             label ? label : "?", (unsigned long)timeout_ms);
    return false;
}

void purr_kernel_shutdown(void) {
    ESP_LOGW(TAG, "kernel shutdown requested — entering deep sleep");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_deep_sleep_start();
}

#define MESH_BACKEND_NVS_NS  "purr_settings"
#define MESH_BACKEND_NVS_KEY "mesh_backend"

purr_mesh_backend_t purr_kernel_mesh_backend_get(void) {
    nvs_handle_t h;
    if (nvs_open(MESH_BACKEND_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return PURR_MESH_BACKEND_MESHTASTIC;
    }
    uint8_t v = PURR_MESH_BACKEND_MESHTASTIC;
    esp_err_t err = nvs_get_u8(h, MESH_BACKEND_NVS_KEY, &v);
    nvs_close(h);
    if (err != ESP_OK) return PURR_MESH_BACKEND_MESHTASTIC;
    return (v == PURR_MESH_BACKEND_MESHCORE) ? PURR_MESH_BACKEND_MESHCORE : PURR_MESH_BACKEND_MESHTASTIC;
}

void purr_kernel_mesh_backend_set(purr_mesh_backend_t backend) {
    nvs_handle_t h;
    if (nvs_open(MESH_BACKEND_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, MESH_BACKEND_NVS_KEY, (uint8_t)backend);
    nvs_commit(h);
    nvs_close(h);
}

// Persist first — if the target's start (below) fails for some reason, the
// preference still reflects intent, same as a failed switch should still
// "stick" for the next boot rather than silently staying on the old
// backend. purr_kernel_unload_module() (used by purr_kernel_module_set_
// enabled(name, false)) fully removes the stopped module from s_modules[]
// before this returns, so the target's own init() — re-run synchronously
// by purr_kernel_module_set_enabled(name, true) — sees a clean
// purr_kernel_get_module(other) == NULL when its mutual-exclusion guard
// checks (meshtastic_module.c's mesh_manager_init() / meshcore_module.cpp's
// mc_manager_init()), exactly as if this were done by hand via Terminal's
// `stop`/`start`. No reboot needed — the one-physical-radio constraint only
// requires the other module to be gone before the target starts, not a
// clean boot.
int purr_kernel_mesh_backend_switch(purr_mesh_backend_t backend) {
    purr_kernel_mesh_backend_set(backend);

    const char *target_name = (backend == PURR_MESH_BACKEND_MESHCORE) ? "meshcore" : "meshtastic";
    const char *other_name  = (backend == PURR_MESH_BACKEND_MESHCORE) ? "meshtastic" : "meshcore";

    if (purr_kernel_get_module(other_name)) {
        purr_kernel_module_set_enabled(other_name, false);
    }
    if (purr_kernel_get_module(target_name)) {
        return PURR_MODCTL_ERR_ALREADY;   // already running — nothing left to do
    }
    return purr_kernel_module_set_enabled(target_name, true);
}

// ── Notifications ─────────────────────────────────────────────────────────────
// Ring buffer indexed by insertion order; s_notify_head points at the slot
// the *next* notification will be written to, so the most recent entry is
// always at (s_notify_head - 1).

static purr_notification_t s_notify_buf[PURR_NOTIFY_MAX];
static int                  s_notify_head  = 0;
static int                  s_notify_count = 0;

void purr_kernel_notify(const char *title, const char *body, const char *source)
{
    purr_notification_t *n = &s_notify_buf[s_notify_head];
    memset(n, 0, sizeof(*n));
    if (title)  strncpy(n->title,  title,  sizeof(n->title)  - 1);
    if (body)   strncpy(n->body,   body,   sizeof(n->body)   - 1);
    if (source) strncpy(n->source, source, sizeof(n->source) - 1);
    n->timestamp_ms = purr_kernel_uptime_ms();

    s_notify_head = (s_notify_head + 1) % PURR_NOTIFY_MAX;
    if (s_notify_count < PURR_NOTIFY_MAX) s_notify_count++;

    ESP_LOGI(TAG, "notify [%s] %s: %s", source ? source : "?", title ? title : "", body ? body : "");
}

int purr_kernel_notify_count(void) { return s_notify_count; }

bool purr_kernel_notify_at(int idx, purr_notification_t *out)
{
    if (idx < 0 || idx >= s_notify_count || !out) return false;
    int slot = (s_notify_head - 1 - idx + PURR_NOTIFY_MAX) % PURR_NOTIFY_MAX;
    *out = s_notify_buf[slot];
    return true;
}

void purr_kernel_notify_clear(void)
{
    s_notify_head  = 0;
    s_notify_count = 0;
}

// Remove one entry, newest-first index, keeping the rest contiguous.
//
// The ring stores oldest..newest ending at (head-1), so "index k" lives at
// slot (head-1-k). Closing the gap means walking the entries NEWER than k
// (indices k-1 .. 0) one slot older each, which overwrites k's slot and frees
// the slot just below head — so head steps back by one and count drops.
// Walking newer-to-older rather than the reverse means each copy reads a slot
// that has not been written yet this pass.
bool purr_kernel_notify_remove(int idx)
{
    if (idx < 0 || idx >= s_notify_count) return false;

    for (int i = idx; i > 0; i--) {
        int dst = (s_notify_head - 1 - i     + PURR_NOTIFY_MAX) % PURR_NOTIFY_MAX;
        int src = (s_notify_head - 1 - (i-1) + PURR_NOTIFY_MAX) % PURR_NOTIFY_MAX;
        s_notify_buf[dst] = s_notify_buf[src];
    }

    s_notify_head = (s_notify_head - 1 + PURR_NOTIFY_MAX) % PURR_NOTIFY_MAX;
    s_notify_count--;
    return true;
}

// ── Service health registry ───────────────────────────────────────────────────
// See purr_kernel_health_register()'s comment in purr_kernel.h — a single
// shared watchdog task polls every registered check so individual modules
// (meshtastic, wifi_mgr, bt_mgr, ...) don't each need their own.

typedef struct {
    const char           *name;
    purr_health_check_fn  is_alive;
    bool                  was_alive;
} health_entry_t;

static health_entry_t   s_health[PURR_HEALTH_MAX];
static int               s_health_count = 0;
static TaskHandle_t      s_health_watchdog_task = NULL;

// Last uptime_ms() a UI backend's own pump loop called purr_kernel_ui_heartbeat()
// — 0 until the first call ever happens. See the staleness check in
// health_watchdog_task() below and purr_crash_guard.h for the full design.
static uint64_t s_ui_last_heartbeat_ms = 0;

// Memory-pressure watchdog state — see MEM_WARN_PCT's doc comment below.
static bool     s_mem_warn_active   = false;   // sticky: only notify on the rising edge
static uint64_t s_last_mem_kill_ms  = 0;       // 0 = never — MEM_KILL_COOLDOWN_MS gate

#define HEALTH_WATCHDOG_POLL_MS 2000UL
#define UI_HANG_THRESHOLD_MS    6000UL   // ~3 missed polls

// ── Memory-pressure thresholds ──────────────────────────────────────────────
// Percent of internal SRAM (MALLOC_CAP_INTERNAL) in use, checked every
// HEALTH_WATCHDOG_POLL_MS alongside the UI-hang check above. Escalating
// ladder — matches PURR OS's own pre-rewrite design (see "PURR OS Layout
// OLD.md"'s "Memory Management Thresholds" table), adapted to this kernel's
// actual capabilities: no real per-app heap accounting exists here (would
// need wrapping every malloc/free), so instead of enforcing a per-app
// declared budget, MEM_KILL_PCT delegates to whatever's registered via
// purr_kernel_set_mem_pressure_cb() (app_manager's
// app_manager_kill_worst_offender(), which approximates "worst offender" via
// each app's free-memory-at-launch snapshot) to pick a specific app to stop.
// Exact percentages are a starting point, not a tuned/measured target —
// adjust freely.
#define MEM_WARN_PCT      84   // log + notify only
#define MEM_KILL_PCT      90   // ask the registered callback to stop the worst offender
#define MEM_CRITICAL_PCT  97   // still critical after a kill attempt (or nothing to kill) — reboot
#define MEM_KILL_COOLDOWN_MS 10000UL   // don't attempt another kill within this long of the last one

static void health_watchdog_task(void *arg);

// Static INTERNAL-RAM stack. This was PSRAM-backed, under an exemption that was
// true when written and silently stopped being true:
//
//   "this task only calls is_alive()/heartbeat-staleness checks +
//    notify()/panic, never touches NVS/flash directly"
//
// It does now. health_watchdog_task() gained a memory-pressure path that calls
// app_manager_kill_worst_offender() -> app_manager_stop() ->
// purr_crash_guard_mark_stop() -> clear_breadcrumb() -> nvs_set_str(). Writing
// NVS disables the flash cache, and a PSRAM stack is unreachable while it is
// disabled, so the task faults on its own stack the moment it does the one thing
// it exists to do.
//
// Confirmed on hardware, decoded from the backtrace:
//   assert failed: esp_task_stack_is_sane_cache_disabled() (cache_utils.c:152)
//   health_watchdog_task -> app_manager_kill_worst_offender -> app_manager_stop
//     -> purr_crash_guard_mark_stop -> clear_breadcrumb -> nvs_set_str
//
// It stayed hidden because the kill path only runs under real memory pressure —
// it first fired after a game-mode round trip left internal DRAM at 26KB.
//
// Same protected category as cupcake_task and settings/fileman: any task that
// can reach flash needs its stack in internal RAM. 3072 bytes is a modest cost
// for removing a crash that only appears when the system is already struggling.
#define HEALTH_WD_STACK_SIZE 3072
static StackType_t  s_health_wd_stack[HEALTH_WD_STACK_SIZE];
static StaticTask_t s_health_wd_tcb;

static void ensure_health_watchdog_started(void)
{
    // Lazily started (by whichever of purr_kernel_health_register()/
    // purr_kernel_ui_heartbeat() runs first) rather than unconditionally at
    // kernel boot — some builds/configs never need either.
    if (!s_health_watchdog_task) {
        s_health_watchdog_task = xTaskCreateStatic(health_watchdog_task, "health_wd",
                                                   HEALTH_WD_STACK_SIZE, NULL, 2,
                                                   s_health_wd_stack, &s_health_wd_tcb);
    }
}

void purr_kernel_ui_heartbeat(void)
{
    s_ui_last_heartbeat_ms = purr_kernel_uptime_ms();
    ensure_health_watchdog_started();
}

static const char *s_ui_breadcrumb = "?";

void purr_kernel_ui_breadcrumb(const char *step)
{
    s_ui_breadcrumb = step ? step : "?";
}

// ── Generic liveness watch ──────────────────────────────────────────────────
// See purr_kernel.h for why this exists separately from the UI-hang check:
// that one is gated on a UI backend being registered, and speed demon's entire
// purpose is to unload it.
static const char *s_watch_owner      = NULL;
static uint64_t    s_watch_last_ms    = 0;
static uint32_t    s_watch_timeout_ms = 0;
// Whichever task most recently called purr_kernel_watch_beat() — updated on
// every beat, not captured once at watch_begin(). That matters for speed
// demon specifically: app_manager's declarative `.speed_demon = 1` path
// calls purr_speed_demon_enter() (and so watch_begin()) from a short-lived
// launch task, BEFORE the app's own long-running task even exists (doom_app.c
// spawns its game task from init(), which watch_begin() ran ahead of) — so
// there is no single "the game's task" to capture up front. Recording it on
// every beat instead means it is always whichever task most recently proved
// it was alive, with zero cooperation needed from the app beyond calling
// heartbeat the way it already has to.
static TaskHandle_t s_watch_task       = NULL;
// How many times the timeout has been extended for a task that is still
// alive but silent — see the health_watchdog_task check below. Reset on
// every real beat (a genuine heartbeat is full proof of liveness, not just
// "still scheduled") and at watch_begin()/watch_end().
static int          s_watch_extensions = 0;
// Caps the total silent-but-alive grace at 6 extensions of the base window
// (60s on speed demon's 10s window) — generous enough for PrBoom's own
// internal startup (texture/sprite table build — see doom_app.c's
// wad_progress() comment for the WAD-load half of this same problem, which
// heartbeat already covers; this covers the part after the WAD is loaded,
// which nothing calls heartbeat during, because it runs inside PrBoom's own
// vendored code) without turning into "a wedged task just runs forever
// unsupervised" — a task that is still silent after 60s is being treated as
// hung regardless of whether it is technically still scheduled.
#define SPEED_DEMON_MAX_EXTENSIONS 6

void purr_kernel_watch_begin(const char *owner, uint32_t interval_ms, int missed_beats)
{
    if (!owner || interval_ms == 0) return;
    if (missed_beats < 1) missed_beats = 1;

    s_watch_owner      = owner;
    s_watch_timeout_ms = interval_ms * (uint32_t)missed_beats;
    // Seeded to "now", not 0: the owner has not missed anything yet, and a 0
    // here would read as one full timeout's worth of silence the instant the
    // watchdog next polls.
    s_watch_last_ms    = purr_kernel_uptime_ms();
    s_watch_task       = NULL;
    s_watch_extensions = 0;

    ensure_health_watchdog_started();
    ESP_LOGI(TAG, "watch: %s every %ums, react after %d missed (%ums)",
             owner, (unsigned)interval_ms, missed_beats, (unsigned)s_watch_timeout_ms);
}

void purr_kernel_watch_beat(void)
{
    if (!s_watch_owner) return;   // no-op outside a watch — see header
    s_watch_last_ms    = purr_kernel_uptime_ms();
    s_watch_task       = xTaskGetCurrentTaskHandle();
    s_watch_extensions = 0;
}

void purr_kernel_watch_end(void)
{
    if (!s_watch_owner) return;
    ESP_LOGI(TAG, "watch: %s ended", s_watch_owner);
    s_watch_owner      = NULL;
    s_watch_timeout_ms = 0;
    s_watch_last_ms    = 0;
    s_watch_task       = NULL;
    s_watch_extensions = 0;

    // Hand the UI check a fresh grace period rather than a stale timestamp.
    //
    // The UI heartbeat stopped the moment speed demon unloaded the backend, so
    // when the watch lifts, s_ui_last_heartbeat_ms is however long the whole
    // session lasted — instantly past the threshold. The restored backend's
    // first beat is several hundred ms away (task start, LVGL re-init, first
    // frame), so without this the UI check would fire the moment it re-enables
    // and blame the backend for a hang that is really just a restart.
    s_ui_last_heartbeat_ms = purr_kernel_uptime_ms();
}

static void health_watchdog_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(HEALTH_WATCHDOG_POLL_MS));

        // UI-hang detection — feeds purr_crash_guard's hang path. The
        // render/pump loops (cupcake_task, MiniWin's pump task) have no
        // natural per-iteration timeout wrapper the way app tasks get
        // from app_manager_stop()'s semaphore-wait — they're designed to
        // loop forever, so a genuine deadlock (confirmed live: MiniWin's
        // own close-icon handling could freeze the whole UI this way
        // before Part A's fix) has nothing else to catch it. Only checked
        // once a UI backend is registered AND has heartbeated at least
        // once — a headless/serial-only build, or the brief window before
        // the UI task's very first loop iteration, must not trip this.
        // A generic watch SUPERSEDES the UI check — see s_watch_owner below.
        //
        // Unloading a UI backend does not unregister its catcall: nothing in
        // purr_kernel_unload_module() clears the registry, and no backend's
        // deinit does it either, so purr_kernel_ui() keeps returning the
        // module's static catcall_ui_t after its task is gone. The check below
        // then measures a heartbeat that can never arrive again and declares a
        // hang ~6s later.
        //
        // That is exactly what happened on the first working game-mode session:
        // MagiDOS ran, then died ~9s in (6s threshold + up to 2s poll
        // granularity + the transition), and the panic was attributed to the UI
        // backend that speed demon had deliberately removed.
        //
        // Speed demon arms its own watch precisely because the UI one cannot
        // cover it, so while that is in force this check has nothing useful to
        // say and must stand down.
        const catcall_ui_t *ui = purr_kernel_ui();
        if (!s_watch_owner && ui && s_ui_last_heartbeat_ms != 0) {
            uint64_t now = purr_kernel_uptime_ms();
            if (now - s_ui_last_heartbeat_ms > UI_HANG_THRESHOLD_MS) {
                // Include the last breadcrumb (purr_kernel_ui_breadcrumb())
                // so the panic screen says which step of the pump loop it
                // never got past, not just that it's unresponsive.
                char reason[64];
                snprintf(reason, sizeof(reason), "UI TASK UNRESPONSIVE @ %s", s_ui_breadcrumb);
                purr_crash_guard_mark_hang(ui->name, reason);
                // Loops forever inside purr_kernel_panic_ex() (blue,
                // recoverable) — nothing after this point runs again
                // this boot.
            }
        }

        // Generic watch — same hang path, no UI dependency.
        //
        // This is what covers speed demon. The check above cannot: it requires
        // purr_kernel_ui() to be non-NULL, and speed demon unloads the UI backend
        // outright, so an unsupervised window would open at precisely the point
        // where one app owns the display and input and nothing else is left
        // running to notice it stopped.
        //
        // Routed through purr_crash_guard_mark_hang() rather than a bespoke
        // reboot so a game hang is recorded, strike-counted and recovered
        // identically to a UI hang — including the pending-recovery marker the
        // next boot reads to show "Recovering from Game Mode" instead of the
        // usual splash.
        //
        // A missed heartbeat is NOT on its own treated as proof of a hang —
        // only as proof that whatever last called purr_kernel_watch_beat()
        // (s_watch_task) needs checking. Doom's WAD load calls heartbeat
        // throughout (see doom_app.c's wad_progress()), but PrBoom's own
        // internal startup after that — texture/sprite table construction,
        // R_InitData, entirely inside vendored code this OS does not
        // instrument — calls it not at all, and measured well past 10s on
        // this hardware. Reported live: the watchdog was striking and
        // rebooting a game that was still genuinely loading, not hung.
        //
        // eTaskGetState() on a handle whose task has since self-deleted
        // returns eDeleted rather than being undefined — FreeRTOS keeps a
        // deleted task's TCB around until the idle task reclaims it, and
        // querying a still-held handle in that window is the documented,
        // safe way to ask "did this task actually finish" without a second
        // synchronization primitive. That is the real distinction this
        // makes: still SCHEDULED (however silent) gets more time, up to
        // SPEED_DEMON_MAX_EXTENSIONS; actually GONE — self-deleted without
        // going through app_manager_notify_exited()'s own restore path (that
        // path calls purr_speed_demon_exit(), which ends this watch cleanly
        // before the task even gets here — see app_manager.c), or genuinely
        // crashed — gets no grace at all, straight to the existing
        // strike-and-reboot failsafe below, same as today.
        if (s_watch_owner && s_watch_timeout_ms) {
            uint64_t now = purr_kernel_uptime_ms();
            if (now - s_watch_last_ms > (uint64_t)s_watch_timeout_ms) {
                bool task_alive = s_watch_task != NULL &&
                                   eTaskGetState(s_watch_task) != eDeleted;
                if (task_alive && s_watch_extensions < SPEED_DEMON_MAX_EXTENSIONS) {
                    s_watch_extensions++;
                    ESP_LOGW(TAG, "watch: %s silent %ums but task still scheduled — "
                             "extending (%d/%d)", s_watch_owner,
                             (unsigned)(now - s_watch_last_ms),
                             s_watch_extensions, SPEED_DEMON_MAX_EXTENSIONS);
                    s_watch_last_ms = now;
                } else {
                    char reason[64];
                    unsigned silent_ms = (unsigned)(now - s_watch_last_ms);
                    if (task_alive) {
                        snprintf(reason, sizeof(reason),
                                 "NO HEARTBEAT FOR %ums (extensions exhausted)", silent_ms);
                    } else {
                        snprintf(reason, sizeof(reason),
                                 "NO HEARTBEAT FOR %ums (task gone, no clean exit)", silent_ms);
                    }
                    const char *owner = s_watch_owner;
                    // Cleared before marking: mark_hang() does not return, and a
                    // stale watch surviving into the recovery path would be a
                    // second, spurious trip.
                    s_watch_owner      = NULL;
                    s_watch_timeout_ms = 0;
                    s_watch_task       = NULL;
                    s_watch_extensions = 0;
                    purr_crash_guard_mark_hang(owner, reason);
                    // Loops forever inside purr_kernel_panic_ex() — nothing
                    // after this point runs again this boot.
                }
            }
        }

        // TEMPORARY diagnostic — tracking a reported fast internal-DRAM
        // drain ("23 bytes free... high memory pressure out of the gate").
        // Logs every 2s regardless of app activity so a continuous background
        // leak (vs. a one-time boot-time consumption) shows up as a steady
        // downward slope even with nothing else touched. Remove once found.
        //
        // dma_free/largest_dma added while chasing a follow-on symptom:
        // live boot captures show "dma_utils: esp_dma_capable_malloc(172):
        // Not enough heap memory" -> "diskio_sdmmc: sdmmc_read_blocks
        // failed" during phase-2 SD scanning, right after bt_mgr/wifi_mgr/
        // meshtastic have all loaded — and the same SD-read path
        // (fopen()/fread()) is what launch_meow() and lua_sd_read()/
        // lua_sd_write() use, so this is the likely cause of "loaded
        // scripts are broken". esp_dma_capable_malloc() requests
        // MALLOC_CAP_DMA specifically, which is internal-RAM-only on this
        // chip (esp_dma_utils.c explicitly can't combine MALLOC_CAP_DMA
        // with MALLOC_CAP_SPIRAM) — so unlike the BLE/LVGL fixes, this
        // can't just be routed to PSRAM. CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL
        // already carves out a dedicated 32KB DMA-capable pool at boot
        // (esp_psram_extram_reserve_dma_pool(), confirmed live: "esp_psram:
        // Reserving pool of 32K of internal memory for DMA/internal
        // allocations"). MALLOC_CAP_INTERNAL alone (already logged above)
        // doesn't tell us whether that specific 32KB reserve is what's
        // collapsing, or whether it's healthy and something else is going
        // on — these two extra fields answer that question directly.
        ESP_LOGW(TAG, "heapwatch: internal_free=%u largest_internal=%u dma_free=%u largest_dma=%u psram_free=%u uptime_ms=%llu",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned long long)purr_kernel_uptime_ms());

        // ── Memory-pressure watchdog ─────────────────────────────────────
        // heap_caps_get_info() (not just get_free_size()) so percent-used is
        // computed against this heap's own actual total capacity rather
        // than a hardcoded constant that could drift across boards/builds.
        {
            uint64_t now_wd = purr_kernel_uptime_ms();
            multi_heap_info_t info;
            heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);
            size_t total = info.total_free_bytes + info.total_allocated_bytes;
            uint8_t pct_used = total ? (uint8_t)((100ULL * info.total_allocated_bytes) / total) : 0;

            if (pct_used >= MEM_CRITICAL_PCT) {
                // Give a registered kill-worst-offender callback one shot
                // first (same cooldown-gated call as the KILL_PCT tier
                // below) — only reboot outright if that either isn't
                // registered, has nothing left to kill, or didn't help.
                bool killed = false;
                if (s_mem_pressure_cb && (now_wd - s_last_mem_kill_ms) >= MEM_KILL_COOLDOWN_MS) {
                    s_last_mem_kill_ms = now_wd;
                    killed = s_mem_pressure_cb();
                }
                if (killed) {
                    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);
                    total = info.total_free_bytes + info.total_allocated_bytes;
                    pct_used = total ? (uint8_t)((100ULL * info.total_allocated_bytes) / total) : 0;
                }
                if (pct_used >= MEM_CRITICAL_PCT) {
                    ESP_LOGE(TAG, "memory watchdog: %u%% internal SRAM used (critical) — restarting", pct_used);
                    purr_kernel_reboot();
                    // never returns
                }
            } else if (pct_used >= MEM_KILL_PCT) {
                if (s_mem_pressure_cb && (now_wd - s_last_mem_kill_ms) >= MEM_KILL_COOLDOWN_MS) {
                    s_last_mem_kill_ms = now_wd;
                    ESP_LOGW(TAG, "memory watchdog: %u%% internal SRAM used — stopping worst offender", pct_used);
                    s_mem_pressure_cb();
                }
            } else if (pct_used >= MEM_WARN_PCT) {
                if (!s_mem_warn_active) {
                    s_mem_warn_active = true;
                    ESP_LOGW(TAG, "memory watchdog: %u%% internal SRAM used", pct_used);
                    purr_kernel_notify("Low memory", "System memory running low", "kernel");
                }
            } else {
                s_mem_warn_active = false;   // dropped back below MEM_WARN_PCT — re-arm the notify
            }
        }

        for (int i = 0; i < s_health_count; i++) {
            health_entry_t *h = &s_health[i];
            bool alive = h->is_alive();
            if (h->was_alive && !alive) {
                char body[PURR_NOTIFY_BODY_LEN];
                snprintf(body, sizeof(body), "%s appears unresponsive", h->name);
                purr_kernel_notify("Service down", body, h->name);
            } else if (!h->was_alive && alive) {
                char body[PURR_NOTIFY_BODY_LEN];
                snprintf(body, sizeof(body), "%s recovered", h->name);
                purr_kernel_notify("Service recovered", body, h->name);
            }
            h->was_alive = alive;
        }
    }
}

void purr_kernel_health_register(const char *name, purr_health_check_fn is_alive)
{
    if (!name || !is_alive) return;

    // A restarted module (Meshtastic today, anything else stoppable/
    // restartable later — see purr_kernel_module_restart()) calls this
    // again every time it re-inits. Without this dedup check, each restart
    // used to append a brand-new entry forever, until PURR_HEALTH_MAX
    // silently stopped accepting more and the Services app's health list
    // showed the same module several times over. Re-registering just
    // refreshes the existing entry in place instead.
    for (int i = 0; i < s_health_count; i++) {
        if (strcmp(s_health[i].name, name) == 0) {
            s_health[i].is_alive  = is_alive;
            s_health[i].was_alive = is_alive();
            return;
        }
    }

    if (s_health_count >= PURR_HEALTH_MAX) return;
    health_entry_t *h = &s_health[s_health_count++];
    h->name      = name;
    h->is_alive  = is_alive;
    h->was_alive = is_alive();

    ensure_health_watchdog_started();
}

int purr_kernel_health_count(void) { return s_health_count; }

bool purr_kernel_health_at(int idx, const char **name, bool *alive)
{
    if (idx < 0 || idx >= s_health_count) return false;
    if (name)  *name  = s_health[idx].name;
    if (alive) *alive = s_health[idx].is_alive();
    return true;
}

// ── Boot readiness ───────────────────────────────────────────────────────────

static volatile bool s_boot_ready = false;

bool purr_kernel_boot_ready(void)        { return s_boot_ready; }
void purr_kernel_set_boot_ready(bool v)  { s_boot_ready = v; }
