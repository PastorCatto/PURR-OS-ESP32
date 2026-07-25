// taskmgr.c — PURR OS Task Manager.
//
// The one deliberate place a running app actually gets torn down.
// MiniWin's title-bar X (miniwin.c, check_and_process_touch_on_title_bar())
// no longer calls mw_remove_window() at all — it just minimises the window,
// exactly like the minimise icon two branches below it always has. Real
// teardown (app_manager_stop(), which ends up calling the app's own
// deinit() -> purr_win_destroy()) only happens from this app's Kill button
// now, so it's rare and deliberate instead of reachable from every window's
// title bar by muscle memory. See app_manager.c/app_manager.h — nothing new
// was needed there, app_manager_get()/app_manager_count()/app_manager_stop()
// already existed.

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "purr_win.h"
#include "purr_kernel.h"
#include "purr_module.h"
#include "app_manager.h"

#define MAX_ROWS 32

static purr_win_t s_win        = 0;
static purr_wid_t s_status_lbl = 0;
static purr_wid_t s_list       = 0;

// Row i of s_list corresponds to app_manager index s_row_idx[i] — the list
// only shows RUNNING apps, so row indices and app_manager indices diverge
// as soon as anything is IDLE/STOPPED/ERROR. Same pattern as meshchat.c's
// s_buddy_ids[] mapping buddy-list row -> node id.
static int         s_row_idx[MAX_ROWS];
static char        s_row_labels[MAX_ROWS][64];
static const char  *s_row_label_ptrs[MAX_ROWS];
static int          s_row_count = 0;

static const char *tier_label(app_tier_t t) {
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
    s_row_count = 0;
    int n = app_manager_count();
    for (int i = 0; i < n && s_row_count < MAX_ROWS; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (!app || app->state != APP_STATE_RUNNING) continue;

        // Name is explicitly bounded at 38 chars rather than left to snprintf's
        // own truncation. 38 + " [kitten]" (9) + " (no window)" (12) + NUL fits
        // the 64-byte row exactly, which means the two suffixes always survive.
        // Letting the whole string truncate instead would drop them first — and
        // "(no window)" is the one piece of state this row exists to show, so
        // losing it to a long app name is precisely backwards.
        //
        // GCC only reports this at -O2 (-Werror=format-truncation needs the
        // analysis passes -Og skips), so it surfaced when this device moved off
        // the debug optimisation level, not from any change to this file.
        snprintf(s_row_labels[s_row_count], sizeof(s_row_labels[s_row_count]),
                 "%.38s [%s]%s", app->name, tier_label(app->tier),
                 app->window ? "" : " (no window)");
        s_row_label_ptrs[s_row_count] = s_row_labels[s_row_count];
        s_row_idx[s_row_count] = i;
        s_row_count++;
    }

    if (s_list) purr_win_list_set_items(s_list, s_row_label_ptrs, s_row_count);

    if (s_status_lbl) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%d app%s running", s_row_count, s_row_count == 1 ? "" : "s");
        purr_win_label_set(s_status_lbl, buf);
    }
}

static void on_refresh_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    refresh_task_list();
}

static void on_kill_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    int row = purr_win_list_get_selected(s_list);
    if (row < 0 || row >= s_row_count) return;

    int idx = s_row_idx[row];
    const app_entry_t *app = app_manager_get(idx);
    // Don't let Task Manager kill itself out from under the button handler
    // that's running inside it — same self-referential hazard app_manager
    // already guards against in its own stop-vs-running-task ordering.
    if (app && strcmp(app->name, "taskmgr") == 0) return;

    app_manager_stop(idx);
    refresh_task_list();
}

static void on_shutdown_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    if (s_status_lbl) purr_win_label_set(s_status_lbl, "Shutting down...");
    vTaskDelay(pdMS_TO_TICKS(500));
    purr_kernel_shutdown();
}

static int taskmgr_init(void) {
    s_win = purr_win_create("Task Manager");
    s_status_lbl = purr_win_label(s_win, "");

    purr_wid_t row = purr_win_row(s_win, 4);
    purr_win_button(s_win, "Refresh",  on_refresh_click, NULL);
    purr_win_button(s_win, "Kill",     on_kill_click, NULL);
    purr_win_button(s_win, "Shutdown", on_shutdown_click, NULL);
    purr_win_layout_end(row);

    s_list = purr_win_list(s_win, 100, 90);

    refresh_task_list();
    purr_win_show(s_win);
    return 0;
}

static void taskmgr_deinit(void) {
    purr_win_destroy(s_win);
    s_win = 0; s_status_lbl = 0; s_list = 0;
    s_row_count = 0;
}

// ── Module header ─────────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(taskmgr) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_APP,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "taskmgr",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = taskmgr_init,
    .deinit            = taskmgr_deinit,
};
