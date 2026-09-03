// diagnostics.c — PURR OS Diagnostics (.claw)
//
// Merges five formerly-standalone system apps that were all the same shape —
// a read-mostly screen over live system/hardware state, each paying its own
// app_manager slot/window/taskbar entry for what's really one tabbed info
// screen — into a single app with five sections. Follows the exact
// multi-screen pattern settings.c already established: one top-level
// category menu, each category a lazily-built, cached sub-window with its
// own "< Back" button. Behavior within each section is preserved verbatim
// from its original file; only the app-manager-facing shape changed.
//
//   drivermgr -> Drivers   (was driver_manager_app.c)
//   hwtest    -> Hardware  (was hwtest.c)
//   services  -> Services  (was services_app.c)
//   meshdiag  -> Mesh      (was meshdiag.c)
//   taskmgr   -> Tasks     (was taskmgr.c)

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR — see s_drv_row_bufs's own comment below
#include "purr_win.h"
#include "purr_kernel.h"
#include "purr_module.h"
#include "../../../kernel/catcalls/catcall_input.h"
#include "driver_manager.h"
#include "bt_mgr.h"
#include "app_manager.h"
#include "meshtastic.h"

// mesh_radio_lock()/unlock() only exist when CONFIG_PURR_FEATURE_MESHTASTIC is
// actually enabled — see meshdiag.c's original identical comment on this same
// guard: interleaving a radio call against mesh_task()'s own command sequence
// on the shared RadioLib object is a confirmed-live source of corruption, and
// the Mesh section's refresh task polls rssi()/snr() unpinned.
#ifdef CONFIG_PURR_FEATURE_MESHTASTIC
#include "../../modules/meshtastic/mesh_radio.h"
#define MESHDIAG_RADIO_LOCK()   mesh_radio_lock()
#define MESHDIAG_RADIO_UNLOCK() mesh_radio_unlock()
#else
#define MESHDIAG_RADIO_LOCK()   ((void)0)
#define MESHDIAG_RADIO_UNLOCK() ((void)0)
#endif

// ── Top-level: category picker ──────────────────────────────────────────────

static purr_win_t s_win      = 0;
static purr_wid_t s_cat_menu = 0;

// Shared by every section's sub-window, same helper settings.c uses:
// purr_win_t is a plain uint32_t handle (catcall_ui.h), so it round-trips
// through the callback's void* user pointer without needing per-window
// wrapper state.
static void on_subwin_back(purr_wid_t w, purr_event_t e, void *u) {
    (void)w; (void)e;
    purr_win_hide((purr_win_t)(uintptr_t)u);
}
static void add_back_button(purr_win_t win) {
    purr_win_button(win, "< Back", on_subwin_back, (void *)(uintptr_t)win);
}

// ── Drivers section (was driver_manager_app.c) ──────────────────────────────

#define DRV_MAX_ROWS 32

static purr_win_t  s_drv_win        = 0;
static purr_wid_t  s_drv_list       = 0;
static purr_wid_t  s_drv_status_lbl = 0;
// EXT_RAM_BSS_ATTR (PSRAM, not internal DRAM) on this file's row/log buffers
// — pure rebuilt-on-refresh display text, never touched before PSRAM is up
// (diagnostics is a normal launched app). Same class MiniWin's own control/
// list arrays and settings.c's/fileman.c's row buffers already use this
// for — real, confirmed via purr_os.map: a genuine link-time DRAM overflow
// once the current app set was all compiled in together (see msn.c's
// matching comment, 2026-09-01).
static EXT_RAM_BSS_ATTR char s_drv_row_bufs[DRV_MAX_ROWS][96];
static const char *s_drv_row_ptrs[DRV_MAX_ROWS];

static void refresh_drivers(void) {
    int n = driver_manager_get_count();
    if (n > DRV_MAX_ROWS) n = DRV_MAX_ROWS;

    for (int i = 0; i < n; i++) {
        const drv_entry_t *d = driver_manager_get_entry(i);
        if (!d) { n = i; break; }
        if (d->status == DRV_STATUS_FAIL || d->status == DRV_STATUS_SKIP) {
            snprintf(s_drv_row_bufs[i], sizeof(s_drv_row_bufs[i]), "%s %-16s v%s (%s)",
                     drv_status_badge(d->status), d->name, d->version, d->fail_reason);
        } else {
            snprintf(s_drv_row_bufs[i], sizeof(s_drv_row_bufs[i]), "%s %-16s v%s",
                     drv_status_badge(d->status), d->name, d->version);
        }
        s_drv_row_ptrs[i] = s_drv_row_bufs[i];
    }
    purr_win_list_set_items(s_drv_list, s_drv_row_ptrs, n);

    char status[48];
    snprintf(status, sizeof(status), "%d driver(s) scanned", n);
    purr_win_label_set(s_drv_status_lbl, status);
}

static void on_drv_refresh(purr_wid_t w, purr_event_t e, void *u) {
    (void)w; (void)e; (void)u;
    refresh_drivers();
}

static void open_drivers(purr_wid_t w, purr_event_t e, void *u) {
    (void)w; (void)e; (void)u;
    if (s_drv_win) { purr_win_show(s_drv_win); refresh_drivers(); return; }

    s_drv_win = purr_win_create("Drivers");
    add_back_button(s_drv_win);
    purr_win_button(s_drv_win, "Refresh", on_drv_refresh, NULL);
    s_drv_list = purr_win_list(s_drv_win, 100, 80);
    s_drv_status_lbl = purr_win_label(s_drv_win, "Ready.");

    purr_win_show(s_drv_win);
    refresh_drivers();
}

// ── Hardware section (was hwtest.c) ─────────────────────────────────────────

#define HW_LOG_LINES 14
#define HW_LINE_LEN  40

static purr_win_t s_hw_win = 0;
static purr_wid_t s_hw_out = 0;
static TaskHandle_t s_hw_poller = NULL;
static bool s_hw_running = false;
// Given by hw_poller_task() right before it self-deletes, waited on by
// diagnostics_deinit() before it destroys s_hw_win — same use-after-free fix
// hwtest.c's original s_poller_done carried: Kill/close could otherwise tear
// down s_hw_win/s_hw_out while the poller was mid purr_win_textarea_set() on
// them under enough scheduling contention. Created once in diagnostics_init()
// regardless of whether the Hardware section is ever opened — cheap, and
// keeps diagnostics_deinit()'s wait unconditionally safe.
static SemaphoreHandle_t s_hw_poller_done = NULL;

static EXT_RAM_BSS_ATTR char s_hw_log[HW_LOG_LINES][HW_LINE_LEN];
static int  s_hw_log_head  = 0;
static int  s_hw_log_count = 0;

static void hw_log_line(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_hw_log[s_hw_log_head], HW_LINE_LEN, fmt, ap);
    va_end(ap);
    s_hw_log_head = (s_hw_log_head + 1) % HW_LOG_LINES;
    if (s_hw_log_count < HW_LOG_LINES) s_hw_log_count++;
}

static void hw_render_log(char *buf, size_t sz) {
    size_t pos = 0;
    pos += snprintf(buf + pos, sz - pos,
        "Trackball + keyboard tester\n"
        "Roll the ball / click it / type keys.\n"
        "----------------------------------------\n");
    int start = (s_hw_log_head - s_hw_log_count + HW_LOG_LINES) % HW_LOG_LINES;
    for (int i = 0; i < s_hw_log_count && pos < sz; i++) {
        int idx = (start + i) % HW_LOG_LINES;
        pos += snprintf(buf + pos, sz - pos, "%s\n", s_hw_log[idx]);
    }
}

// Drains every registered input device. Started lazily, once, the first time
// the Hardware section is opened (see open_hardware()) — stopped only in
// diagnostics_deinit(), so it runs for as long as the original standalone
// hwtest app's poller did (its entire open lifetime), just gated on this
// section having been visited at all rather than on app launch itself.
static void hw_poller_task(void *arg) {
    (void)arg;
    char buf[HW_LOG_LINES * HW_LINE_LEN + 128];
    bool dirty = true;

    while (s_hw_running) {
        int n = purr_kernel_input_count();
        for (int i = 0; i < n; i++) {
            const catcall_input_t *inp = purr_kernel_input_at(i);
            if (!inp || !inp->poll_event) continue;

            input_event_t ev;
            int drained = 0;
            while (drained++ < 16 && inp->poll_event(&ev)) {
                dirty = true;
                switch (ev.type) {
                    case INPUT_EVENT_POINTER:
                        hw_log_line("[%s] move dx=%d dy=%d", inp->name, ev.delta_x, ev.delta_y);
                        break;
                    case INPUT_EVENT_KEY_DOWN:
                        if (ev.keycode >= 0x20 && ev.keycode <= 0x7E)
                            hw_log_line("[%s] key DOWN '%c' (0x%02X)", inp->name, (char)ev.keycode, ev.keycode);
                        else
                            hw_log_line("[%s] key DOWN 0x%02X", inp->name, ev.keycode);
                        break;
                    case INPUT_EVENT_KEY_UP:
                        hw_log_line("[%s] key UP   0x%02X", inp->name, ev.keycode);
                        break;
                    default:
                        break;
                }
            }
        }

        if (dirty) {
            hw_render_log(buf, sizeof(buf));
            purr_win_textarea_set(s_hw_out, buf);
            dirty = false;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    if (s_hw_poller_done) xSemaphoreGive(s_hw_poller_done);
    // Must match the WithCaps variant used to create this task.
    vTaskDeleteWithCaps(NULL);
}

static void on_hw_clear(purr_wid_t w, purr_event_t e, void *u) {
    (void)w; (void)e; (void)u;
    s_hw_log_head = 0;
    s_hw_log_count = 0;
    char buf[256];
    hw_render_log(buf, sizeof(buf));
    purr_win_textarea_set(s_hw_out, buf);
}

static void open_hardware(purr_wid_t w, purr_event_t e, void *u) {
    (void)w; (void)e; (void)u;
    if (s_hw_win) { purr_win_show(s_hw_win); return; }

    s_hw_win = purr_win_create("Hardware");
    add_back_button(s_hw_win);
    s_hw_out = purr_win_textarea(s_hw_win, 100, 80);

    purr_wid_t row = purr_win_row(s_hw_win, 4);
    purr_win_button(s_hw_win, "Clear", on_hw_clear, NULL);
    purr_win_layout_end(row);

    char buf[256];
    hw_render_log(buf, sizeof(buf));
    purr_win_textarea_set(s_hw_out, buf);

    purr_win_show(s_hw_win);

    s_hw_running = true;
    // No NVS/flash/SD access in this task's body — safe on a PSRAM-backed
    // stack (see app_manager.c's launch_native()/launch_meow() pattern).
    xTaskCreateWithCaps(hw_poller_task, "diag_hw_poll", 3072, NULL, 4, &s_hw_poller, MALLOC_CAP_SPIRAM);
}

// ── Services section (was services_app.c) ───────────────────────────────────
//
// meshtastic/meshcore are deliberately filtered out of the health-list rows
// below — they're "extensions" managed through MSN (msn.c's chooser screen)
// and Settings' Connectivity category, not generic modules browsed here.

#define SVC_MAX_ROWS 24

static purr_win_t  s_svc_win        = 0;
static purr_wid_t  s_svc_list       = 0;
static purr_wid_t  s_svc_mem_lbl    = 0;
static purr_wid_t  s_svc_status_lbl = 0;
static EXT_RAM_BSS_ATTR char s_svc_row_bufs[SVC_MAX_ROWS][80];
static const char *s_svc_row_ptrs[SVC_MAX_ROWS];

static bool svc_is_filtered_health_name(const char *name) {
    return name && (strcmp(name, "meshtastic") == 0 || strcmp(name, "meshcore") == 0);
}

static void refresh_services(void) {
    int n = 0;

    snprintf(s_svc_row_bufs[n], sizeof(s_svc_row_bufs[n]), "WiFi: %s",
             purr_kernel_wifi_connected() ? "connected" : "disconnected");
    s_svc_row_ptrs[n] = s_svc_row_bufs[n]; n++;

    snprintf(s_svc_row_bufs[n], sizeof(s_svc_row_bufs[n]), "Bluetooth: %s",
             bt_mgr_is_enabled() ? "enabled" : "disabled");
    s_svc_row_ptrs[n] = s_svc_row_bufs[n]; n++;

    snprintf(s_svc_row_bufs[n], sizeof(s_svc_row_bufs[n]), "SD card: %s",
             purr_kernel_sd_available() ? "mounted" : "not mounted");
    s_svc_row_ptrs[n] = s_svc_row_bufs[n]; n++;

    snprintf(s_svc_row_bufs[n], sizeof(s_svc_row_bufs[n]), "LoRa radio: %s",
             purr_kernel_lora_available() ? "available" : "unavailable");
    s_svc_row_ptrs[n] = s_svc_row_bufs[n]; n++;

    // No device in the supported table has a battery-backed RTC — see
    // purr_kernel.h's "Wall-clock time" doc comment — so "unsynced" is the
    // expected state fresh off a cold boot, not a fault.
    if (purr_kernel_time_is_synced()) {
        time_t now = purr_kernel_time_now();
        struct tm tm_utc;
        gmtime_r(&now, &tm_utc);
        char ts[20];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", &tm_utc);
        const char *src = "?";
        switch (purr_kernel_time_source()) {
            case PURR_TIME_SOURCE_NVS:    src = "saved"; break;
            case PURR_TIME_SOURCE_GPS:    src = "gps";   break;
            case PURR_TIME_SOURCE_NTP:    src = "ntp";   break;
            case PURR_TIME_SOURCE_RTC_HW: src = "rtc";   break;
            default: break;
        }
        snprintf(s_svc_row_bufs[n], sizeof(s_svc_row_bufs[n]), "Clock: %s UTC (%s)", ts, src);
    } else {
        snprintf(s_svc_row_bufs[n], sizeof(s_svc_row_bufs[n]), "Clock: unsynced");
    }
    s_svc_row_ptrs[n] = s_svc_row_bufs[n]; n++;

    // Every service registered with purr_kernel_health_register() shows up
    // here automatically — except meshtastic/meshcore, see file header.
    int health_n = purr_kernel_health_count();
    for (int i = 0; i < health_n && n < SVC_MAX_ROWS; i++) {
        const char *name = NULL;
        bool alive = false;
        if (!purr_kernel_health_at(i, &name, &alive)) break;
        if (svc_is_filtered_health_name(name)) continue;
        snprintf(s_svc_row_bufs[n], sizeof(s_svc_row_bufs[n]), "%s: %s",
                 name ? name : "?", alive ? "alive" : "UNRESPONSIVE");
        s_svc_row_ptrs[n] = s_svc_row_bufs[n]; n++;
    }

    purr_win_list_set_items(s_svc_list, s_svc_row_ptrs, n);

    size_t int_free    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t psram_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    char mem_buf[80];
    snprintf(mem_buf, sizeof(mem_buf), "Internal: %u B free (largest %u B)  |  PSRAM: %u KB free",
             (unsigned)int_free, (unsigned)int_largest, (unsigned)(psram_free / 1024));
    purr_win_label_set(s_svc_mem_lbl, mem_buf);

    // Internal RAM is the scarce resource on this board (a few KB after
    // boot, see app_manager.c's launch-path comments) — flag it plainly
    // once it gets tight rather than making the reader do the math.
    if (int_largest < 4096) {
        purr_win_label_set(s_svc_status_lbl, "Memory pressure: HIGH — app launches may fail.");
    } else if (int_largest < 8192) {
        purr_win_label_set(s_svc_status_lbl, "Memory pressure: moderate.");
    } else {
        purr_win_label_set(s_svc_status_lbl, "Memory pressure: normal.");
    }
}

static void on_svc_refresh(purr_wid_t w, purr_event_t e, void *u) {
    (void)w; (void)e; (void)u;
    refresh_services();
}

static void open_services(purr_wid_t w, purr_event_t e, void *u) {
    (void)w; (void)e; (void)u;
    if (s_svc_win) { purr_win_show(s_svc_win); refresh_services(); return; }

    s_svc_win = purr_win_create("Services");
    add_back_button(s_svc_win);

    purr_win_label(s_svc_win, "Core Services");
    s_svc_list = purr_win_list(s_svc_win, 100, 55);

    purr_win_label(s_svc_win, "Memory");
    s_svc_mem_lbl    = purr_win_label(s_svc_win, "...");
    s_svc_status_lbl = purr_win_label(s_svc_win, "...");

    purr_win_button(s_svc_win, "Refresh", on_svc_refresh, NULL);

    purr_win_show(s_svc_win);
    refresh_services();
}

// ── Mesh section (was meshdiag.c) ────────────────────────────────────────────
//
// Not a chat client (see msn.c for that) — purely for bringing up and
// debugging the radio itself: raw RSSI/SNR, node count, kernel log tail (no
// serial cable needed), and a one-shot test broadcast.

#define MESH_KLOG_TAIL_SIZE 2048
#define MESH_REFRESH_MS     2000

static purr_win_t s_mesh_win       = 0;
static purr_wid_t s_mesh_stats_out = 0;
static purr_wid_t s_mesh_log_out   = 0;
static purr_wid_t s_mesh_send_in   = 0;

static TaskHandle_t s_mesh_refresh_task = NULL;
static bool         s_mesh_running      = false;
static char        *s_mesh_klog_buf     = NULL;   // heap/PSRAM
// Same synchronization role as s_hw_poller_done above — see that comment.
// Created once in diagnostics_init() regardless of whether the Mesh section
// is ever opened.
static SemaphoreHandle_t s_mesh_refresh_done = NULL;

static void refresh_mesh_stats(void) {
    if (!s_mesh_stats_out) return;

    const catcall_radio_t *radio = purr_kernel_radio();
    char buf[512];
    int n = 0;

    n += snprintf(buf + n, sizeof(buf) - n,
        "uptime: %llums   free ram: %u\n",
        (unsigned long long)purr_kernel_uptime_ms(), (unsigned)purr_kernel_free_ram());

    int batt = purr_kernel_battery_percent();
    n += snprintf(buf + n, sizeof(buf) - n,
        "wifi: %s   sd: %s   battery: %s\n",
        purr_kernel_wifi_connected() ? "connected" : "down",
        purr_kernel_sd_available() ? "ok" : "none",
        batt < 0 ? "unknown" : "");
    if (batt >= 0) {
        n += snprintf(buf + n, sizeof(buf) - n, "  (%d%%)\n", batt);
    }

    n += snprintf(buf + n, sizeof(buf) - n,
        "lora hw: %s\n", purr_kernel_lora_available() ? "present" : "not detected");

    if (radio) {
        MESHDIAG_RADIO_LOCK();
        int rssi = radio->rssi ? radio->rssi() : 0;
        float snr = radio->snr ? radio->snr() : 0.0f;
        MESHDIAG_RADIO_UNLOCK();
        n += snprintf(buf + n, sizeof(buf) - n,
            "radio rssi: %d dBm   snr: %.1f dB\n", rssi, snr);
    } else {
        n += snprintf(buf + n, sizeof(buf) - n, "radio: no catcall registered\n");
    }

    n += snprintf(buf + n, sizeof(buf) - n,
        "mesh: %s / %s   nodes: %d   my id: !%08lX\n",
        mesh_manager_ready() ? "ready" : "starting",
        mesh_manager_is_alive() ? "alive" : "not responding",
        mesh_manager_node_count(), (unsigned long)mesh_manager_node_id());

    purr_win_textarea_set(s_mesh_stats_out, buf);
}

static void refresh_mesh_klog(void) {
    if (!s_mesh_log_out || !s_mesh_klog_buf) return;
    purr_kernel_klog_tail(s_mesh_klog_buf, MESH_KLOG_TAIL_SIZE);
    purr_win_textarea_set(s_mesh_log_out, s_mesh_klog_buf);
}

static void mesh_refresh_task(void *arg) {
    (void)arg;
    while (s_mesh_running) {
        refresh_mesh_stats();
        refresh_mesh_klog();
        // Short steps, not one MESH_REFRESH_MS vTaskDelay — diagnostics_deinit()
        // blocks on this task actually exiting, so how quickly it notices
        // s_mesh_running == false directly bounds how long a Kill/close stalls.
        for (int waited_ms = 0; waited_ms < MESH_REFRESH_MS && s_mesh_running; waited_ms += 200) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    if (s_mesh_refresh_done) xSemaphoreGive(s_mesh_refresh_done);
    // Must match the WithCaps variant used to create this task.
    vTaskDeleteWithCaps(NULL);
}

static void on_mesh_send_test(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    const char *text = purr_win_textarea_get(s_mesh_send_in);
    if (!text || !*text) text = "PURR OS meshdiag test";
    bool ok = mesh_manager_send_text(MESH_BROADCAST, 0, text);   // primary channel test broadcast
    ESP_LOGI("diagnostics", "mesh test send %s: \"%s\"", ok ? "OK" : "FAILED", text);
    refresh_mesh_stats();
}

static void on_mesh_refresh_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    refresh_mesh_stats();
    refresh_mesh_klog();
}

static void open_mesh(purr_wid_t w, purr_event_t e, void *u) {
    (void)w; (void)e; (void)u;
    if (s_mesh_win) { purr_win_show(s_mesh_win); return; }

    if (!s_mesh_klog_buf) s_mesh_klog_buf = heap_caps_malloc(MESH_KLOG_TAIL_SIZE, MALLOC_CAP_SPIRAM);

    s_mesh_win = purr_win_create("Mesh");
    add_back_button(s_mesh_win);

    s_mesh_stats_out = purr_win_textarea(s_mesh_win, 100, 35);

    purr_wid_t row = purr_win_row(s_mesh_win, 4);
    purr_win_button(s_mesh_win, "Refresh", on_mesh_refresh_click, NULL);
    purr_win_layout_end(row);

    s_mesh_send_in = purr_win_textarea(s_mesh_win, 70, 12);
    purr_win_button(s_mesh_win, "Send Test", on_mesh_send_test, NULL);

    s_mesh_log_out = purr_win_textarea(s_mesh_win, 100, 40);

    refresh_mesh_stats();
    refresh_mesh_klog();
    purr_win_show(s_mesh_win);
    purr_win_keyboard_show(s_mesh_win, s_mesh_send_in);

    s_mesh_running = true;
    // No NVS/flash/SD access anywhere in this task's own body — safe on a
    // PSRAM-backed stack (see app_manager.c's launch_native()/launch_meow()).
    xTaskCreateWithCaps(mesh_refresh_task, "diag_mesh_ref", 4096, NULL, 3, &s_mesh_refresh_task, MALLOC_CAP_SPIRAM);
}

// ── Tasks section (was taskmgr.c) ────────────────────────────────────────────
//
// The one deliberate place a running app actually gets torn down — see the
// original taskmgr.c's file header for why MiniWin's title-bar X only
// minimizes now instead of destroying.

#define TASK_MAX_ROWS 32

static purr_win_t  s_task_win        = 0;
static purr_wid_t  s_task_status_lbl = 0;
static purr_wid_t  s_task_list       = 0;

// Row i of s_task_list corresponds to app_manager index s_task_row_idx[i] —
// the list only shows RUNNING apps, so row indices and app_manager indices
// diverge as soon as anything is IDLE/STOPPED/ERROR.
static EXT_RAM_BSS_ATTR int  s_task_row_idx[TASK_MAX_ROWS];
static EXT_RAM_BSS_ATTR char s_task_row_labels[TASK_MAX_ROWS][64];
static const char *s_task_row_label_ptrs[TASK_MAX_ROWS];
static int         s_task_row_count = 0;

static const char *task_tier_label(app_tier_t t) {
    switch (t) {
        case APP_TIER_MEOW:   return "meow";
        case APP_TIER_PAWS:   return "paws";
        case APP_TIER_CLAW:   return "claw";
        case APP_TIER_HISS:   return "hiss";
        case APP_TIER_KITTEN: return "kitten";
        default:              return "?";
    }
}

static void refresh_task_list(void) {
    s_task_row_count = 0;
    int n = app_manager_count();
    for (int i = 0; i < n && s_task_row_count < TASK_MAX_ROWS; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (!app || app->state != APP_STATE_RUNNING) continue;

        // Name explicitly bounded at 38 chars rather than left to snprintf's
        // own truncation — see taskmgr.c's original identical comment: this
        // guarantees the " [tier]"/" (no window)" suffixes always survive.
        snprintf(s_task_row_labels[s_task_row_count], sizeof(s_task_row_labels[s_task_row_count]),
                 "%.38s [%s]%s", app->name, task_tier_label(app->tier),
                 app->window ? "" : " (no window)");
        s_task_row_label_ptrs[s_task_row_count] = s_task_row_labels[s_task_row_count];
        s_task_row_idx[s_task_row_count] = i;
        s_task_row_count++;
    }

    if (s_task_list) purr_win_list_set_items(s_task_list, s_task_row_label_ptrs, s_task_row_count);

    if (s_task_status_lbl) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%d app%s running", s_task_row_count, s_task_row_count == 1 ? "" : "s");
        purr_win_label_set(s_task_status_lbl, buf);
    }
}

static void on_task_refresh_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    refresh_task_list();
}

static void on_task_kill_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    int row = purr_win_list_get_selected(s_task_list);
    if (row < 0 || row >= s_task_row_count) return;

    int idx = s_task_row_idx[row];
    const app_entry_t *app = app_manager_get(idx);
    // Don't let this app kill itself out from under the button handler
    // that's running inside it — same self-referential hazard app_manager
    // already guards against in its own stop-vs-running-task ordering.
    // Checks "diagnostics" now, not "taskmgr" — this app's own registered name.
    if (app && strcmp(app->name, "diagnostics") == 0) return;

    app_manager_stop(idx);
    refresh_task_list();
}

static void on_task_shutdown_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    if (s_task_status_lbl) purr_win_label_set(s_task_status_lbl, "Shutting down...");
    vTaskDelay(pdMS_TO_TICKS(500));
    purr_kernel_shutdown();
}

static void open_tasks(purr_wid_t w, purr_event_t e, void *u) {
    (void)w; (void)e; (void)u;
    if (s_task_win) { purr_win_show(s_task_win); refresh_task_list(); return; }

    s_task_win = purr_win_create("Tasks");
    add_back_button(s_task_win);
    s_task_status_lbl = purr_win_label(s_task_win, "");

    purr_wid_t row = purr_win_row(s_task_win, 4);
    purr_win_button(s_task_win, "Refresh",  on_task_refresh_click, NULL);
    purr_win_button(s_task_win, "Kill",     on_task_kill_click, NULL);
    purr_win_button(s_task_win, "Shutdown", on_task_shutdown_click, NULL);
    purr_win_layout_end(row);

    s_task_list = purr_win_list(s_task_win, 100, 90);

    refresh_task_list();
    purr_win_show(s_task_win);
}

// ── Category nav ─────────────────────────────────────────────────────────────
// Same shape as settings.c's build_category_nav()/on_cat_menu() — one menu,
// one code path, every backend renders it the way that backend should look.

#define CAT_COUNT 5
static const char *s_category_labels[CAT_COUNT] = { "Drivers", "Hardware", "Services", "Mesh", "Tasks" };

static void on_cat_menu(purr_wid_t w, purr_event_t e, void *user) {
    (void)user;
    if (e != PURR_EVENT_ACTIVATED) return;
    switch (purr_win_menu_get_selected(w)) {
        case 0: open_drivers(0, PURR_EVENT_CLICKED, NULL);  break;
        case 1: open_hardware(0, PURR_EVENT_CLICKED, NULL); break;
        case 2: open_services(0, PURR_EVENT_CLICKED, NULL); break;
        case 3: open_mesh(0, PURR_EVENT_CLICKED, NULL);     break;
        case 4: open_tasks(0, PURR_EVENT_CLICKED, NULL);    break;
        default: break;
    }
}

static void build_category_nav(void) {
    static const purr_menu_section_t sec = {
        .header = NULL, .items = s_category_labels, .values = NULL, .count = CAT_COUNT,
    };
    s_cat_menu = purr_win_menu(s_win);
    purr_win_menu_set_sections(s_cat_menu, &sec, 1);
    purr_win_menu_on_select(s_cat_menu, on_cat_menu, NULL);
}

// ── App lifecycle ─────────────────────────────────────────────────────────────

static int diagnostics_init(void) {
    // Reused across relaunches — starts "empty" (taken), which is exactly the
    // state diagnostics_deinit()'s xSemaphoreTake() calls need at the start
    // of every run. Created unconditionally regardless of whether the
    // Hardware/Mesh sections are ever opened — cheap, and keeps deinit's
    // wait unconditionally safe to call.
    if (!s_hw_poller_done)   s_hw_poller_done   = xSemaphoreCreateBinary();
    if (!s_mesh_refresh_done) s_mesh_refresh_done = xSemaphoreCreateBinary();

    // Top-level window is just the category picker — each row opens its own
    // lazily-built, cached sub-window (see open_*() above). Keeps this
    // window's own widget count tiny, same rationale settings.c's identical
    // split already used.
    s_win = purr_win_create("Diagnostics");

    purr_win_label(s_win, "Diagnostics");
    build_category_nav();

    purr_win_show(s_win);
    return 0;
}

static void diagnostics_deinit(void) {
    if (s_drv_win)  { purr_win_destroy(s_drv_win);  s_drv_win  = 0; s_drv_list = 0; s_drv_status_lbl = 0; }

    if (s_hw_win) {
        s_hw_running = false;
        // Wait for hw_poller_task() to actually exit before touching s_hw_win
        // below — see s_hw_poller_done's declaration comment.
        if (s_hw_poller_done) xSemaphoreTake(s_hw_poller_done, pdMS_TO_TICKS(2000));
        s_hw_poller = NULL;
        purr_win_destroy(s_hw_win);
        s_hw_win = 0; s_hw_out = 0;
    }

    if (s_svc_win) { purr_win_destroy(s_svc_win); s_svc_win = 0; s_svc_list = 0; s_svc_mem_lbl = 0; s_svc_status_lbl = 0; }

    if (s_mesh_win) {
        s_mesh_running = false;
        // Wait for mesh_refresh_task() to actually exit before touching
        // s_mesh_win below — see s_mesh_refresh_done's declaration comment.
        if (s_mesh_refresh_done) xSemaphoreTake(s_mesh_refresh_done, pdMS_TO_TICKS(2000));
        s_mesh_refresh_task = NULL;
        purr_win_destroy(s_mesh_win);
        s_mesh_win = 0; s_mesh_stats_out = 0; s_mesh_log_out = 0; s_mesh_send_in = 0;
    }

    if (s_task_win) { purr_win_destroy(s_task_win); s_task_win = 0; s_task_status_lbl = 0; s_task_list = 0; s_task_row_count = 0; }

    purr_win_destroy(s_win);
    s_win = 0; s_cat_menu = 0;
}

// Exposed (non-static, no header — matches the ad-hoc `extern` style its one
// caller already uses for app_manager_launch_by_name() itself) for
// kernel_tdeck_plus_pounce's boot code, which launches straight into the Mesh
// section instead of leaving the user on the category picker — that build is
// a Meshtastic hardware/debugging image, and the point (per the original
// standalone meshdiag.c this was merged from) is RSSI/node-count/kernel-log
// visibility without navigating anywhere first. Safe to call only once
// app_manager has finished launching this app — app_manager_launch_by_name()
// blocks until diagnostics_init() (and so s_win) exists before it returns.
void diagnostics_open_mesh(void) {
    open_mesh(0, PURR_EVENT_CLICKED, NULL);
}

// ── Module header ─────────────────────────────────────────────────────────────
// Registered name is "diagnostics" — app_manager looks apps up by this .name
// field, and it's what task_tier_label()'s self-kill guard in the Tasks
// section above checks against.

PURR_MODULE_REGISTER(diagnostics) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_APP,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "diagnostics",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = diagnostics_init,
    .deinit            = diagnostics_deinit,
};
