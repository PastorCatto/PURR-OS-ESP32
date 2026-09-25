// home.c — MiniWin's own "startx": a small window listing every real
// app_manager entry, pick one to launch. This is the failsafe UI the
// user asked for after diagnostics' own MiniWin restoration: reachable
// from the console with a dedicated command (`startx`, purr_console.c)
// the same way `startx` brings up X11 from a bare tty — whether or not
// the full LVGL graphical session (systemui/launcher sysclaw packages,
// kernel_tdp_boot.c's own run_graphical_session()) is working at all.
//
// Deliberately minimal for this first pass, per direct instruction:
// "Start with just a basic load minwin and a small window with apps
// opens (like windows 3.11)" — a flat Program-Manager-style app list,
// not a full desktop/taskbar/icon-grid. MiniWin is a real windowing
// system, so launching another app that also uses purr_win_*() (like
// diagnostics) opens a SECOND window alongside this one rather than
// replacing it — no window-switching/management logic added here yet,
// that's MiniWin's own job and untested territory beyond this first
// pass.
//
// Uses the SAME uiconf format + purr_uiconf_render_winapi.c backend
// diagnostics.c does (see that file's own top comment for the fuller
// "why MiniWin, not raw framebuffer" story) — this app is the first real
// exercise of the `launch_app` action / "$item" resolution against a
// live `app_list` source, both written but unused until now.
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "purr_module.h"
#include "../../../modules/uiconf/purr_uiconf_core.h"
#include "../../../modules/uiconf/purr_uiconf_render_winapi.h"
#include "app_manager.h"

static const char *TAG = "home";

static purr_uiconf_render_winapi_t s_win;
static purr_uiconf_state_t         s_state;

static void dispatch_action(const char *action, const char *target)
{
    if (strcmp(action, "launch_app") != 0) return;

    char resolved[40];
    const char *real_target = target;
    if (strcmp(target, "$item") == 0) {
        if (!purr_uiconf_render_winapi_selected_target(&s_win, &s_state, resolved, sizeof(resolved))) {
            ESP_LOGW(TAG, "launch_app: no current selection to resolve \"$item\" against");
            return;
        }
        real_target = resolved;
    }
    // source="app_list" resolves "$item" to the app's own index (see
    // purr_uiconf_sources.c's own source_target() case 0) — exactly what
    // app_manager_launch_idx(int) takes.
    int idx = atoi(real_target);
    int rc = app_manager_launch_idx(idx);
    ESP_LOGI(TAG, "launch_app idx=%d rc=%d", idx, rc);
}

static void on_widget_event(purr_wid_t wid, purr_event_t event, void *user)
{
    (void)user;
    int node = purr_uiconf_render_winapi_node_for_wid(&s_win, wid);
    if (node == PUI_NONE || node != s_win.menu_node) return;

    if (event == PURR_EVENT_SELECTED || event == PURR_EVENT_ACTIVATED) {
        int sel = purr_win_menu_get_selected(wid);
        if (sel >= 0) s_state.selected_index = sel;
    }
    if (event != PURR_EVENT_ACTIVATED) return;

    char action[24], target[40];
    if (purr_uiconf_find_handler(&s_state.screen, node, "activate", action, sizeof(action), target, sizeof(target)))
        dispatch_action(action, target);
}

static int home_init(void)
{
    if (!purr_uiconf_render_winapi_init(&s_win, "PURR OS")) {
        ESP_LOGE(TAG, "purr_win_create() failed — no UI backend and no fallback available");
        return -1;
    }
    if (!purr_uiconf_open(&s_state, "home", "main")) {
        ESP_LOGE(TAG, "failed to load ui/main.pui");
        purr_uiconf_render_winapi_deinit(&s_win);
        return -1;
    }
    purr_uiconf_render_winapi_draw(&s_win, &s_state, on_widget_event, NULL);
    return 0;
}

static void home_deinit(void)
{
    // Never actually called today (same "every module keeps a deinit
    // either way" contract this whole tree follows) — nothing currently
    // closes this window itself; it's meant to stay up as the failsafe
    // shell for the whole session.
}

PURR_MODULE_REGISTER(home) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_APP,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "home",
    .version           = "0.1.0",
    .kernel_min        = "0.11.1",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = home_init,
    .deinit            = home_deinit,
};
