// diagnostics.c — PURR OS Diagnostics (.claw), rewritten onto the uiconf
// declarative UI-config format (see the approved plan at
// /home/PastorCatto/.claude/plans/delightful-exploring-hearth.md).
//
// First-pass scope, deliberately smaller than the app this replaces: ONE
// read-only screen (loaded modules + free RAM/uptime), not the five
// merged tabs (Hardware I/O, Services, Mesh diag, Tasks) the previous
// diagnostics.c carried — see archive/apps_v1/diagnostics/ (once this
// rewrite is hardware-verified and archived there) for that version.
// Those come back gradually in a later pass, per this whole effort's own
// "gradually re-introducing features" direction — not a regression, a
// deliberate minimal-first-pass restart.
//
// ── Real-hardware history: fb backend crashed, MiniWin restored instead ──
// This app originally drew through purr_uiconf_render_fb.c (direct
// catcall_display_t access), matching tdeck_plus's own `ui="none"` at the
// time. That crashed launching on real hardware — most likely raw
// push_pixels()/fill_rect() calls racing purr_fbtty's OWN concurrent
// framebuffer console access with no shared lock between them, unlike
// purr_win_*()'s own purr_kernel_ui_lock()/unlock() wrapping every call.
// Rather than debug that blind, tdeck_plus's `ui=` was switched to
// "miniwin" (source/modules/miniwin/, restored from archive/ui_backends_v1/
// — a real, previously-working, still-current-kernel-version catcall_ui_t
// backend, not something rebuilt from scratch) and this app now draws
// through purr_uiconf_render_winapi.c instead. See tdeck_plus/device.pcat's
// own `ui=` comment for the fuller story, including the real
// apply_radio_companion_defaults() bug that surfaced (and got fixed)
// switching to a real UI backend for the first time since the console-only
// rewrite.
//
// ── Input, under a real UI backend ──────────────────────────────────────
// MiniWin owns its own input pump entirely (trackball/keyboard routed to
// the focused widget by MiniWin itself) — this app has NO polling task of
// its own, unlike the fb-backend version. Interaction arrives as widget
// callbacks: the module list's own purr_win_menu_on_select() fires
// PURR_EVENT_SELECTED (highlight moved) / PURR_EVENT_ACTIVATED (confirmed
// — no-op here, the list is read-only and has no "on activate" handler),
// and the on-screen "< Back" button fires PURR_EVENT_CLICKED, resolved
// through purr_uiconf_find_handler() same as every other backend. Per the
// user's own decision: a real on-screen Back button is the primary
// mechanism; a Backspace-double-tap keyboard shortcut (this format's
// original plan for a device with no dedicated hardware Back button) is
// NOT wired for this MiniWin path yet — nothing in purr_win.h exposes a
// window-level "any key" hook to build it from, and the on-screen button
// works today, so that shortcut is deferred rather than half-built.
//
// Because MiniWin manages the list's own selection/highlight and redraw
// entirely, this app never calls purr_uiconf_render_winapi_draw() again
// after the initial one — a second full draw() would purr_win_clear()
// the window, destroying whatever native focus/selection state MiniWin
// was tracking. Free RAM/uptime are therefore a snapshot taken at open,
// not a live-refreshing readout (an honest first-pass scope reduction,
// not an oversight) — reintroduced once per-widget live updates
// (purr_win_label_set() on a handle kept from open, not a full redraw)
// are worth the plumbing.
#include <string.h>
#include "esp_log.h"
#include "purr_module.h"
#include "purr_kernel.h"
#include "../../../modules/uiconf/purr_uiconf_core.h"
#include "../../../modules/uiconf/purr_uiconf_render_winapi.h"
#include "app_manager.h"

static const char *TAG = "diagnostics";

static purr_uiconf_render_winapi_t s_win;
static purr_uiconf_state_t         s_state;

static void do_exit(void)
{
    ESP_LOGI(TAG, "exiting");
    // No task of our own to delete — MiniWin's own pump task is what
    // called us, via the widget callback below. Deleting IT would take
    // the whole UI backend down with this one app; see this file's own
    // top comment.
    purr_uiconf_render_winapi_deinit(&s_win);
    purr_uiconf_close(&s_state);
    app_manager_notify_exited("diagnostics");
}

static void dispatch_action(const char *action, const char *target)
{
    (void)target;   // diagnostics' own first-pass scope has no open_screen/launch_app/run_command use yet
    if (strcmp(action, "exit_app") == 0) {
        do_exit();
    }
}

// The one callback wired to every interactive widget this screen has
// (the module list's own on-select, and the "< Back" button) — see
// purr_uiconf_render_winapi_draw()'s own comment on why one shared
// callback, resolved by `wid`, is the right shape for a native widget
// manager's own event model.
static void on_widget_event(purr_wid_t wid, purr_event_t event, void *user)
{
    (void)user;
    int node = purr_uiconf_render_winapi_node_for_wid(&s_win, wid);
    if (node == PUI_NONE) return;

    if (node == s_win.menu_node) {
        // The module list itself. PURR_EVENT_SELECTED (highlight moved,
        // no confirm) just needs s_state.selected_index kept in sync so a
        // future redraw (if this app ever needs one again) doesn't reset
        // the highlight — cheap and harmless to do unconditionally.
        if (event == PURR_EVENT_SELECTED || event == PURR_EVENT_ACTIVATED) {
            int sel = purr_win_menu_get_selected(wid);
            if (sel >= 0) s_state.selected_index = sel;
        }
        if (event != PURR_EVENT_ACTIVATED) return;
        // Read-only list — no "on activate" handler exists in ui/main.pui,
        // so this is correctly a no-op; purr_uiconf_find_handler() returns
        // false and nothing happens.
        char action[24], target[40];
        if (purr_uiconf_find_handler(&s_state.screen, node, "activate", action, sizeof(action), target, sizeof(target)))
            dispatch_action(action, target);
        return;
    }

    // A standalone button (only "< Back" today).
    if (event != PURR_EVENT_CLICKED) return;
    char action[24], target[40];
    if (purr_uiconf_find_handler(&s_state.screen, node, "select", action, sizeof(action), target, sizeof(target)))
        dispatch_action(action, target);
}

static int diagnostics_init(void)
{
    if (!purr_uiconf_render_winapi_init(&s_win, "Diagnostics")) {
        ESP_LOGE(TAG, "purr_win_create() failed — no UI backend and no fallback available");
        return -1;
    }
    if (!purr_uiconf_open(&s_state, "diagnostics", "main")) {
        ESP_LOGE(TAG, "failed to load ui/main.pui");
        purr_uiconf_render_winapi_deinit(&s_win);
        return -1;
    }
    purr_uiconf_render_winapi_draw(&s_win, &s_state, on_widget_event, NULL);
    return 0;
}

static void diagnostics_deinit(void)
{
    // Never actually called today (same "every module keeps a deinit
    // either way" contract this whole tree follows) — the running
    // instance tears itself down via do_exit() instead, from the widget
    // callback that requested it.
}

// Exposed (non-static, no header — matches the previous version's own
// ad-hoc `extern` style) for kernel_tdeck_plus_pounce's boot code
// (kernel_tdp_boot.c), which calls this to jump straight into a
// Mesh-focused view instead of leaving the user on the general
// diagnostics screen — that build is a Meshtastic hardware/debugging
// image. The dedicated Mesh section is one of this rewrite's
// deliberately DEFERRED tabs (see this file's own top comment) — this is
// a real, temporary no-op until it's rebuilt on the new format, not a
// silent regression: it logs clearly instead of pretending to still work.
void diagnostics_open_mesh(void)
{
    ESP_LOGW(TAG, "diagnostics_open_mesh(): Mesh section not yet rebuilt on the new "
                  "uiconf format (first-pass diagnostics is read-only modules+RAM+uptime "
                  "only) -- showing the general screen instead");
}

PURR_MODULE_REGISTER(diagnostics) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_APP,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "diagnostics",
    .version           = "2.1.0",
    .kernel_min        = "0.11.1",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = diagnostics_init,
    .deinit            = diagnostics_deinit,
};
