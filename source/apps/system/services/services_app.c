// services_app.c — PURR OS Services (.claw)
//
// Read-only dashboard: live status of core background services (WiFi, BLE,
// SD, LoRa radio, the wall clock, plus anything registered via
// purr_kernel_health_register()) and current memory pressure. Exists so a
// hung/crashed background service is visible somewhere even if it never
// pushed its own notification, and so "is the device under memory pressure
// right now" (or "does the device even know what time it is") is answerable
// without a serial log.
//
// meshtastic/meshcore are deliberately filtered out of the health-list rows
// below — they're "extensions" managed through MSN (msn.c's chooser screen)
// and Settings' Connectivity category, not generic modules browsed here.
// The Meshtastic-specific Enable/Disable/Restart control this app used to
// have lived here only until MSN's chooser existed; removed now that MSN
// owns that responsibility. purr_kernel_health_register()'s underlying
// watchdog/notify mechanism is untouched — a crash/hang in either backend
// still produces a purr_kernel_notify() banner regardless of this filter.

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "purr_win.h"
#include "purr_module.h"
#include "purr_kernel.h"
#include "esp_heap_caps.h"
#include "bt_mgr.h"

#define MAX_ROWS 24

static purr_win_t s_win        = 0;
static purr_wid_t s_list       = 0;
static purr_wid_t s_mem_lbl    = 0;
static purr_wid_t s_status_lbl = 0;

static char        s_row_bufs[MAX_ROWS][80];
static const char *s_row_ptrs[MAX_ROWS];

static bool is_filtered_health_name(const char *name) {
    return name && (strcmp(name, "meshtastic") == 0 || strcmp(name, "meshcore") == 0);
}

static void refresh(void) {
    int n = 0;

    snprintf(s_row_bufs[n], sizeof(s_row_bufs[n]), "WiFi: %s",
             purr_kernel_wifi_connected() ? "connected" : "disconnected");
    s_row_ptrs[n] = s_row_bufs[n]; n++;

    snprintf(s_row_bufs[n], sizeof(s_row_bufs[n]), "Bluetooth: %s",
             bt_mgr_is_enabled() ? "enabled" : "disabled");
    s_row_ptrs[n] = s_row_bufs[n]; n++;

    snprintf(s_row_bufs[n], sizeof(s_row_bufs[n]), "SD card: %s",
             purr_kernel_sd_available() ? "mounted" : "not mounted");
    s_row_ptrs[n] = s_row_bufs[n]; n++;

    snprintf(s_row_bufs[n], sizeof(s_row_bufs[n]), "LoRa radio: %s",
             purr_kernel_lora_available() ? "available" : "unavailable");
    s_row_ptrs[n] = s_row_bufs[n]; n++;

    // No device in the supported table has a battery-backed RTC — see
    // purr_kernel.h's "Wall-clock time" doc comment — so "unsynced" is the
    // expected state fresh off a cold boot, not a fault. Source is shown
    // alongside the time itself because which one is currently authoritative
    // is exactly the kind of thing this app exists to surface.
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
        snprintf(s_row_bufs[n], sizeof(s_row_bufs[n]), "Clock: %s UTC (%s)", ts, src);
    } else {
        snprintf(s_row_bufs[n], sizeof(s_row_bufs[n]), "Clock: unsynced");
    }
    s_row_ptrs[n] = s_row_bufs[n]; n++;

    // Every service registered with purr_kernel_health_register() shows up
    // here automatically — except meshtastic/meshcore, see file header.
    int health_n = purr_kernel_health_count();
    for (int i = 0; i < health_n && n < MAX_ROWS; i++) {
        const char *name = NULL;
        bool alive = false;
        if (!purr_kernel_health_at(i, &name, &alive)) break;
        if (is_filtered_health_name(name)) continue;
        snprintf(s_row_bufs[n], sizeof(s_row_bufs[n]), "%s: %s",
                 name ? name : "?", alive ? "alive" : "UNRESPONSIVE");
        s_row_ptrs[n] = s_row_bufs[n]; n++;
    }

    purr_win_list_set_items(s_list, s_row_ptrs, n);

    size_t int_free    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t psram_free   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    char mem_buf[80];
    snprintf(mem_buf, sizeof(mem_buf), "Internal: %u B free (largest %u B)  |  PSRAM: %u KB free",
             (unsigned)int_free, (unsigned)int_largest, (unsigned)(psram_free / 1024));
    purr_win_label_set(s_mem_lbl, mem_buf);

    // Internal RAM is the scarce resource on this board (a few KB after
    // boot, see app_manager.c's launch-path comments) — flag it plainly
    // once it gets tight rather than making the reader do the math.
    if (int_largest < 4096) {
        purr_win_label_set(s_status_lbl, "Memory pressure: HIGH — app launches may fail.");
    } else if (int_largest < 8192) {
        purr_win_label_set(s_status_lbl, "Memory pressure: moderate.");
    } else {
        purr_win_label_set(s_status_lbl, "Memory pressure: normal.");
    }
}

static void on_refresh(purr_wid_t w, purr_event_t e, void *u) {
    (void)w;(void)e;(void)u;
    refresh();
}

static int services_app_init(void) {
    s_win = purr_win_create("Services");

    purr_win_label(s_win, "Core Services");
    s_list = purr_win_list(s_win, 100, 55);

    purr_win_label(s_win, "Memory");
    s_mem_lbl    = purr_win_label(s_win, "...");
    s_status_lbl = purr_win_label(s_win, "...");

    purr_win_button(s_win, "Refresh", on_refresh, NULL);

    purr_win_show(s_win);
    refresh();
    return 0;
}

static void services_app_deinit(void) {
    purr_win_destroy(s_win);
    s_win = 0; s_list = 0; s_mem_lbl = 0; s_status_lbl = 0;
}

// ── Module header ─────────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(services) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_APP,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "services",
    .version           = "1.1.0",
    .kernel_min        = "0.11.1",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = services_app_init,
    .deinit            = services_app_deinit,
};
