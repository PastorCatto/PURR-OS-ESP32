// pounce_status.c — persistent Meshtastic status strip + compose hotkey for
// the Pounce UI backend (pounce_plan.md A6). Owns the top PW_STATUS_H rows
// of the screen exclusively — pw_layout_compute() (pounce_render.c) never
// lets window content occupy that band, so there's no compositing to do.
#include "sdkconfig.h"

#ifdef CONFIG_PURR_UI_BACKEND_POUNCE

#include "pounce.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/catcall_radio.h"
#include "../meshtastic/meshtastic.h"
#include "../app_manager/app_manager.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

// ── Swappable status-item providers ─────────────────────────────────────────
// Each slot is a name+render pair; changing what the strip shows is a
// one-line table edit here, not a rewrite of any drawing code.

typedef struct {
    const char *name;
    void (*render)(char *buf, size_t buflen);
} pw_status_item_t;

static void status_render_mesh_rssi(char *buf, size_t n) {
    const catcall_radio_t *radio = purr_kernel_radio();
    if (!radio || !radio->rssi) { snprintf(buf, n, "RF:--"); return; }
    snprintf(buf, n, "RF:%ddBm", radio->rssi());
}

static void status_render_mesh_nodes(char *buf, size_t n) {
    snprintf(buf, n, "NODES:%d", mesh_manager_node_count());
}

static void status_render_mesh_unread(char *buf, size_t n) {
    snprintf(buf, n, "MSG:%d", purr_kernel_notify_count());
}

static const pw_status_item_t s_status_items[] = {
    { "mesh_rssi",   status_render_mesh_rssi   },
    { "mesh_nodes",  status_render_mesh_nodes  },
    { "mesh_unread", status_render_mesh_unread },
};
#define STATUS_ITEM_COUNT (int)(sizeof(s_status_items) / sizeof(s_status_items[0]))

// Right-hand side of the strip — separate from the left-aligned mesh items
// above since it's anchored to the opposite edge, not appended in sequence.
static void status_render_battery(char *buf, size_t n) {
    int batt = purr_kernel_battery_percent();
    if (batt < 0) snprintf(buf, n, "BAT:--");
    else          snprintf(buf, n, "BAT:%d%%", batt);
}

// ── Draw ─────────────────────────────────────────────────────────────────────
// Redrawn on a timer, not per-tick — this data changes slowly (matches the
// WinCE taskbar's own RAM-readout refresh precedent from earlier this
// session's miniwin_wince_desktop.c work).

#define STATUS_REDRAW_INTERVAL_US (1000LL * 1000LL)
static int64_t s_last_draw_us = 0;

void pw_status_init(void) {
    s_last_draw_us = 0;
}

static void draw_status_strip(void) {
    const catcall_display_t *disp = purr_kernel_display();
    if (!disp) return;
    display_info_t info;
    disp->get_info(&info);

    pw_fill_rect(0, 0, (int16_t)info.width, PW_STATUS_H, PW_COL_BG);
    pw_fill_rect(0, (int16_t)(PW_STATUS_H - 1), (int16_t)info.width, 1, PW_COL_DIM);

    char seg[24];
    int16_t x = 2;
    for (int i = 0; i < STATUS_ITEM_COUNT; i++) {
        s_status_items[i].render(seg, sizeof(seg));
        pw_draw_string(x, 1, seg, PW_COL_FG, PW_COL_BG);
        x = (int16_t)(x + (int16_t)strlen(seg) * PW_CHAR_W + PW_CHAR_W);
    }

    // Battery, right-aligned on the opposite edge from the mesh items.
    char batt_seg[16];
    status_render_battery(batt_seg, sizeof(batt_seg));
    int16_t batt_x = (int16_t)(info.width - (int16_t)strlen(batt_seg) * PW_CHAR_W - 2);
    if (batt_x > x) pw_draw_string(batt_x, 1, batt_seg, PW_COL_FG, PW_COL_BG);
}

void pw_status_tick(void) {
    int64_t now = esp_timer_get_time();
    if (s_last_draw_us != 0 && (now - s_last_draw_us) < STATUS_REDRAW_INTERVAL_US) return;
    s_last_draw_us = now;
    draw_status_strip();
}

// ── Compose hotkey ───────────────────────────────────────────────────────────
// pw_focus_input_tick() (pounce_focus.c) only ever calls this from nav mode
// (never while a textarea has edit_target) — see this function's header
// declaration in pounce.h for why a bare letter is safe here.

#define PW_COMPOSE_HOTKEY 'M'

bool pw_status_hotkey_check(uint16_t keycode) {
    if (keycode != PW_COMPOSE_HOTKEY) return false;
    int n = app_manager_count();
    for (int i = 0; i < n; i++) {
        const app_entry_t *app = app_manager_get(i);
        if (app && strcmp(app->name, "meshchat") == 0) {
            app_manager_launch_idx(i);
            return true;
        }
    }
    return false;
}

#endif  // CONFIG_PURR_UI_BACKEND_POUNCE
