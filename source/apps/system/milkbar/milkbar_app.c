// milkbar_app.c — PURR OS Milkbar (Remote Apps manager).
//
// Login itself does NOT live here — it's a systemui-level screen now
// (systemui_login.c's "Log in to a server" button and the near-identical
// screen it opens), which runs the actual pairing.h Phase A/B/C exchange
// and, on success, launches this app as the post-login hand-off
// (app_manager_launch_by_name("milkbar")). By the time this app's own
// init() runs in that case, user_mgr_current_user() is already the
// authenticated remote identity — see milkbar_app_init()'s own routing
// logic for how that's detected. This file used to own a whole login UI
// of its own (a Connection-screen login form, briefly a Windows-7-style
// tile/"Not you?" dialog); both were removed once login moved to
// systemui — no login widgets live in this file at all any more.
//
// Two screens, in order:
//   1. Connection (shown when this app is opened WITHOUT an active remote
//      session — e.g. tapped from the desktop during a normal local
//      session): the paired-device trust list (pairing.h) plus Nearby and
//      Disconnect — browsing/pairing management, nothing more. No way to
//      log in from here; that's systemui's job now.
//   2. Dashboard (admin accounts only, user_mgr_is_admin() — learned from
//      the server during login) — shown directly, skipping Connection
//      entirely, when milkbar_app_init() detects an active remote session.
//      Its own "Desktop" button (and, for a non-admin account, this app's
//      own init() directly) hands off to enter_remote_desktop(): NOT a
//      window this app draws — it points app_manager.h's remote mode at
//      the selected server (app_manager_remote_connect(),
//      app_manager_remote.h) and gets out of the way, hiding this app's own
//      window(s) and calling purr_systemui_return_home(). The launcher UI
//      already running on this device (cheetah_home.c's icon grid,
//      systemui_xp.c's taskbar/Start Menu) drives itself purely off
//      app_manager_count()/get()/launch_idx() — the instant remote mode is
//      on, it's showing the server's apps with no code of its own aware
//      anything changed. This is what the whole app used to be, before the
//      Connection/Dashboard split and before remote mode existed: its own
//      bespoke remote app list with Launch/Stop buttons, replaced once
//      there was a real launcher UI worth handing off to instead.
//
// "Nearby" section (was the standalone nearby_app.c): who's beaconing right
// now (proximity.h's live ESP-NOW table, not the trust list above) plus
// Pair/Unpair/Set Home actions. Folded in here — not kept as its own app —
// because discovering a nearby device and connecting to it are the same
// concern this app already owns; milkbar is meant to become the general
// server/client connection surface, and "who's around to connect to" is
// part of that, not a separate destination. Lazily built like Settings'/
// Diagnostics' category sub-windows (see add_back_button() below), and its
// refresh piggybacks on this file's own refresh_task() rather than running
// a second background task — proximity_device_count()/at() is a cheap local
// beacon-table read, unlike the remote-mode RPCs app_manager_remote.c now
// owns entirely on its own background task (see app_manager.h's remote-mode
// doc comment) — this file no longer makes a single proximity_rpc_call() of
// its own at all.

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "purr_win.h"
#include "purr_kernel.h"
#include "purr_module.h"
#include "pairing.h"
#include "proximity.h"
#include "app_manager.h"
#include "app_manager_remote.h"
#include "systemui.h"
#include "user_mgr.h"

#define REFRESH_MS        2000
#define MAX_DEVICE_ROWS   PAIRING_MAX_DEVICES

// Shared by the Nearby sub-window below, same helper settings.c/diagnostics.c
// use: purr_win_t is a plain uint32_t handle (catcall_ui.h), so it round-trips
// through the callback's void* user pointer without needing per-window
// wrapper state.
static void on_subwin_back(purr_wid_t w, purr_event_t e, void *u) {
    (void)w; (void)e;
    purr_win_hide((purr_win_t)(uintptr_t)u);
}
static void add_back_button(purr_win_t win) {
    purr_win_button(win, "< Back", on_subwin_back, (void *)(uintptr_t)win);
}

// ── Connection screen (opened first) ────────────────────────────────────
static purr_win_t s_win           = 0;
static purr_wid_t s_device_list   = 0;

// ── Dashboard screen (admin accounts only) ──────────────────────────────
static purr_win_t s_dashboard_win  = 0;
static purr_wid_t s_dashboard_info_lbl = 0;

static TaskHandle_t s_refresh_task = NULL;
static bool         s_running      = false;
// Same use-after-free fix as nearby_app.c/meshdiag.c/msn.c's own
// s_refresh_done — deinit() waits for the background task to actually
// exit before destroying widgets it might be mid-refresh on.
static SemaphoreHandle_t s_refresh_done = NULL;

static char        s_device_row_bufs[MAX_DEVICE_ROWS][32];
static const char *s_device_row_ptrs[MAX_DEVICE_ROWS];
static int          s_device_count = 0;

// Set from on_device_list_event() (cupcake_task, UI click) — read by
// refresh_task() (its own background task) to know which device's app
// list to fetch next poll. all-zero mac means "no device selected yet".
static uint8_t       s_selected_mac[6];
static volatile bool s_have_selection = false;

// ── App config (LittleFS, /config/milkbar.cfg) ──────────────────────────────
// Remembers which paired device was last selected, across reboots — not just
// relaunches (s_selected_mac/s_have_selection already survive those, being
// plain statics; only a real reboot resets them). Worth persisting
// specifically because milkbar is meant to become the general server/client
// connection surface: reopening it and having to reselect the same device
// every single time is exactly the friction that goal is supposed to remove.
// Bumped to version 3 — dropped the remembered-username field version 2
// added (a Windows-7-style login tile that briefly lived in this app; login
// lives in systemui now, see this file's top comment, so there's no local
// "identity last logged in with" for this app to remember any more). An
// old version-1/2 file fails the magic check below and is just treated as
// absent — same "no saved state yet" cold-start path a first-ever run
// already takes, not a real migration.
#define MILKBAR_CFG_MAGIC 0x4D4C4B03u   // "MLK" + struct version 3

typedef struct {
    uint32_t magic;
    uint8_t  mac[6];
    uint8_t  _pad[2];   // explicit, not relied-on compiler padding — this is written raw to a file
} milkbar_app_cfg_t;

static void milkbar_cfg_save(void) {
    milkbar_app_cfg_t cfg = { .magic = MILKBAR_CFG_MAGIC };
    memcpy(cfg.mac, s_selected_mac, 6);
    purr_app_config_write("milkbar", &cfg, sizeof(cfg));
}

// Loads the saved mac, but only actually selects it if that device is STILL
// in the trust list — pairing_forget() may have happened since the config
// was written, and re-selecting a device this device no longer trusts would
// have remote mode reconnecting to it regardless.
static void milkbar_cfg_load(void) {
    milkbar_app_cfg_t cfg;
    int got = purr_app_config_read("milkbar", &cfg, sizeof(cfg));
    if (got != (int)sizeof(cfg) || cfg.magic != MILKBAR_CFG_MAGIC) return;

    int n = pairing_device_count();
    for (int i = 0; i < n; i++) {
        paired_device_t pd;
        if (pairing_device_at(i, &pd) && memcmp(pd.mac, cfg.mac, 6) == 0) {
            memcpy(s_selected_mac, cfg.mac, 6);
            s_have_selection = true;
            return;
        }
    }
}

// Live presence via proximity's own beacon table — same check homebase.c
// uses for its own home-base connect/disconnect detection, applied here to
// whichever device is currently selected rather than a fixed home base.
static bool device_lookup(const uint8_t mac[6], proximity_device_t *out) {
    int n = proximity_device_count();
    for (int i = 0; i < n; i++) {
        if (proximity_device_at(i, out) && memcmp(out->mac, mac, 6) == 0) return true;
    }
    return false;
}

static bool device_is_connected(const uint8_t mac[6]) {
    proximity_device_t d;
    return device_lookup(mac, &d);
}

// 0 (no caps known) if the device isn't currently beaconing nearby — same
// "nearby right now" limitation device_is_connected() already has, since
// caps only ever travels in a live beacon (proximity.h), never persisted
// in pairing's own paired_device_t.
static uint8_t device_caps(const uint8_t mac[6]) {
    proximity_device_t d;
    return device_lookup(mac, &d) ? d.caps : 0;
}

static void refresh_device_list(void) {
    int n = pairing_device_count();
    if (n > MAX_DEVICE_ROWS) n = MAX_DEVICE_ROWS;
    for (int i = 0; i < n; i++) {
        paired_device_t pd;
        if (!pairing_device_at(i, &pd)) { n = i; break; }
        bool connected = device_is_connected(pd.mac);
        bool companion = connected && (device_caps(pd.mac) & PROXIMITY_CAP_RADIO_COMPANION) != 0;
        snprintf(s_device_row_bufs[i], sizeof(s_device_row_bufs[i]), "%s%s%s",
                 pd.name, connected ? "  [connected]" : "", companion ? "  [mesh]" : "");
        s_device_row_ptrs[i] = s_device_row_bufs[i];
    }
    s_device_count = n;
    if (s_device_list) purr_win_list_set_items(s_device_list, s_device_row_ptrs, s_device_count);
}

static void on_device_list_event(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)user;
    if (e != PURR_EVENT_ACTIVATED) return;
    int idx = purr_win_list_get_selected(s_device_list);
    if (idx < 0) return;
    paired_device_t pd;
    if (!pairing_device_at(idx, &pd)) return;
    memcpy(s_selected_mac, pd.mac, 6);
    s_have_selection = true;
    milkbar_cfg_save();
}

static void on_refresh_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    refresh_device_list();
}

// Turns app_manager's remote mode on for the selected device (app_manager.h,
// app_manager_remote_connect()) unless it's already pointed there. Both
// open_dashboard() and enter_remote_desktop() call this — Dashboard's own
// "Remote apps: N" line needs it live too, not just the eventual Desktop
// hand-off (see refresh_dashboard()) — and checking app_manager_is_remote()
// first means going Dashboard -> Desktop doesn't tear down and restart the
// same connection for no reason.
static void ensure_remote_connected(void) {
    if (!s_have_selection) return;
    if (!app_manager_is_remote()) app_manager_remote_connect(s_selected_mac);
}

// Ends the session and drops back to the Connection screen — from
// Dashboard, the remote desktop, or Connection's own Disconnect button
// (a harmless no-op re-show there). Turns remote mode off and logs out of
// user_mgr — this device is no longer meaningfully "logged in" once it's
// no longer connected to the server that identity came from — but leaves
// s_selected_mac/s_have_selection alone: the device stays selected in the
// Connection screen's list, so logging back in (via the systemui login
// screen, then relaunching this app) is just re-entering credentials
// there, not reselecting a device here too.
static void disconnect_to_connection_screen(void) {
    app_manager_clear_remote();
    if (s_dashboard_win) purr_win_hide(s_dashboard_win);
    user_mgr_logout();
    purr_win_show(s_win);
}

static void on_connection_disconnect_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    disconnect_to_connection_screen();
}

static void on_dashboard_disconnect_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    disconnect_to_connection_screen();
}

// ── "Desktop" — hand off to the real launcher, remote-mode ─────────────────
// Not a window this app draws any more (see this file's top comment) —
// ensure_remote_connected() above points app_manager at the selected
// server, then this just gets out of the way. cheetah_home.c's icon grid
// and systemui_xp.c's taskbar/Start Menu both already drive themselves
// purely off app_manager_count()/get()/launch_idx() — the instant remote
// mode is on, they show the server's apps with no code of their own aware
// anything changed.
static void enter_remote_desktop(void) {
    if (!s_have_selection) return;
    ensure_remote_connected();
    if (s_win)           purr_win_hide(s_win);
    if (s_dashboard_win) purr_win_hide(s_dashboard_win);
    // Reveal the desktop/taskbar underneath — same call mochi_springboard.c
    // already uses after showing ITS OWN home screen (see systemui.h's own
    // doc comment on purr_systemui_return_home(): "callers decide whether
    // leaving means hide (Home) or stop (Back)" — hide is exactly what the
    // two purr_win_hide() calls above just did). This app keeps running
    // with no window of its own visible for as long as remote mode stays
    // on this way — the same accepted shape app_manager.c's own header
    // comment already documents for "an exclusive app that drives the
    // panel directly has no window to re-show" (see
    // app_manager_notify_exited()'s doc comment): tapping Milkbar's
    // taskbar button again in that state just re-shows the (login-free)
    // Connection screen, which still has its own Disconnect button.
    purr_systemui_return_home();
}

static void on_dashboard_desktop_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    enter_remote_desktop();
}

// ── Dashboard (admin accounts only) ─────────────────────────────────────

// Called once Dashboard is first opened and again on every refresh_task()
// pass while s_dashboard_win is open — same "gated on the window existing"
// shape refresh_nearby() already uses, so the info line stays live if the
// user comes back to Dashboard after visiting the remote desktop.
static void refresh_dashboard(void) {
    if (!s_dashboard_win || !s_have_selection) return;

    // Copied out of pd (loop-scoped) rather than kept as a pointer into
    // it — GCC's -Werror=dangling-pointer correctly flags a pointer into a
    // block-scoped local used after that block ends, even though the
    // stack slot happens to still hold the right bytes in practice.
    char server_name[sizeof(((paired_device_t *)0)->name)] = "?";
    for (int i = 0; i < pairing_device_count(); i++) {
        paired_device_t pd;
        if (pairing_device_at(i, &pd) && memcmp(pd.mac, s_selected_mac, 6) == 0) {
            snprintf(server_name, sizeof(server_name), "%s", pd.name);
            break;
        }
    }

    // Live from app_manager's own remote-mode registry — open_dashboard()
    // already turned it on via ensure_remote_connected(). Reads 0 until its
    // background task's first successful list() fetch lands, same as any
    // other app_manager_count() caller.
    int remote_app_count = app_manager_is_remote() ? app_manager_count() : 0;

    char buf[192];
    snprintf(buf, sizeof(buf),
             "Server: %s\nLogged in as: %s\nStatus: %s\nRemote apps: %d",
             server_name, user_mgr_current_user(),
             device_is_connected(s_selected_mac) ? "online" : "not currently in range",
             remote_app_count);
    purr_win_label_set(s_dashboard_info_lbl, buf);
}

static void open_dashboard(void) {
    ensure_remote_connected();
    if (s_dashboard_win) { purr_win_show(s_dashboard_win); refresh_dashboard(); return; }

    s_dashboard_win = purr_win_create("Dashboard");
    s_dashboard_info_lbl = purr_win_label(s_dashboard_win, "");
    purr_wid_t row = purr_win_row(s_dashboard_win, 2);
    purr_win_button(s_dashboard_win, "Desktop", on_dashboard_desktop_click, NULL);
    purr_win_button(s_dashboard_win, "Disconnect", on_dashboard_disconnect_click, NULL);
    purr_win_layout_end(row);

    purr_win_show(s_dashboard_win);
    refresh_dashboard();
}

// ── Nearby section (was nearby_app.c) ───────────────────────────────────────

#define NEARBY_MAX_ROWS        PROXIMITY_MAX_DEVICES
#define NEARBY_MAX_PAIRED_ROWS PAIRING_MAX_DEVICES

static purr_win_t s_nearby_win        = 0;
static purr_wid_t s_nearby_list       = 0;
static purr_wid_t s_nearby_status_lbl = 0;
// Trust-list display — a proper list (see pairing.h's multi-device trust
// list) so "Unpair"/"Set Home" can act on whichever row is selected instead
// of always a single fixed pairing.
static purr_wid_t s_nearby_paired_list       = 0;
static purr_wid_t s_nearby_paired_status_lbl = 0;

// Pairing confirm dialog (initiator side) — open while pairing_get_state()
// == PAIRING_STATE_PENDING_OUTGOING, mirrors msn.c's own backend-switch
// confirm-dialog shape (small window, a label, Cancel). refresh_nearby()'s
// own poll (driven by this file's shared refresh_task() below) also drives
// closing this automatically once the peer accepts/rejects/times out.
static purr_win_t s_pair_win     = 0;
static purr_wid_t s_pair_win_lbl = 0;

static char        s_nearby_row_bufs[NEARBY_MAX_ROWS][64];
static const char *s_nearby_row_ptrs[NEARBY_MAX_ROWS];

static char        s_nearby_paired_row_bufs[NEARBY_MAX_PAIRED_ROWS][32];
static const char *s_nearby_paired_row_ptrs[NEARBY_MAX_PAIRED_ROWS];

static void close_pair_dialog(void) {
    if (s_pair_win) {
        purr_win_destroy(s_pair_win);
        s_pair_win = 0; s_pair_win_lbl = 0;
    }
}

// ── Remote setup dialog (pairing.h's "Remote OOBE") ─────────────────────
// For a paired device with no screen/keyboard of its own to complete its
// own first-run setup (e.g. Heltec's oled_ui) — pushes an admin account
// (or "keep the default") to whichever paired row is selected. One
// request/response, no polling/approval wait (unlike the server-login
// flow systemui_login.c now owns), so this stays a small self-contained
// dialog here rather than needing its own background poll loop.
static purr_win_t s_setup_dlg        = 0;
static purr_wid_t s_setup_user_input = 0;
static purr_wid_t s_setup_pass_input = 0;
static purr_wid_t s_setup_status_lbl = 0;
static uint8_t        s_setup_target_mac[6];
static volatile bool  s_setup_in_flight = false;

// Gated on s_nearby_win: skips all of this (formatting rows nobody would
// see, pushing them to widgets that don't exist yet) until the Nearby
// section has actually been opened once — same "don't do the work for a
// section nobody visited" reasoning diagnostics.c's lazy poller start uses,
// just as a cheap early-return here since this rides the shared task rather
// than owning one.
static void refresh_nearby(void) {
    if (!s_nearby_win) return;

    int n = proximity_device_count();
    if (n > NEARBY_MAX_ROWS) n = NEARBY_MAX_ROWS;

    for (int i = 0; i < n; i++) {
        proximity_device_t dev;
        if (!proximity_device_at(i, &dev)) { n = i; break; }
        uint32_t age_s = ((uint32_t)purr_kernel_uptime_ms() - dev.last_seen_ms) / 1000UL;
        // "[radio]" flags PROXIMITY_CAP_RADIO_COMPANION devices — the ones
        // on_nearby_pair_click() below will actually accept a pairing request.
        snprintf(s_nearby_row_bufs[i], sizeof(s_nearby_row_bufs[i]), "%s%s  (%d dBm, %lus ago)",
                 dev.name, (dev.caps & PROXIMITY_CAP_RADIO_COMPANION) ? " [radio]" : "",
                 (int)dev.rssi, (unsigned long)age_s);
        s_nearby_row_ptrs[i] = s_nearby_row_bufs[i];
    }
    purr_win_list_set_items(s_nearby_list, s_nearby_row_ptrs, n);

    if (!proximity_ready()) {
        purr_win_label_set(s_nearby_status_lbl, "Proximity: starting...");
    } else if (!proximity_is_alive()) {
        purr_win_label_set(s_nearby_status_lbl, "Proximity: not responding");
    } else {
        char buf[48];
        snprintf(buf, sizeof(buf), "Proximity: ready (%d nearby)", n);
        purr_win_label_set(s_nearby_status_lbl, buf);
    }

    // Once the peer accepts (PAIRED), close the dialog — the paired list
    // just below already reflects the new pairing, nothing more to show.
    // Any OTHER way the request ends (rejected or timed out — both
    // surface as a reset back to NONE, see pairing_module.c) used to
    // close this dialog exactly the same way: silently, with the human
    // never told which happened, or that anything happened at all. Under
    // real conditions (weak signal, a busy radio, the memory pressure
    // this session's own boot-log heapwatch already measured on this
    // board — dma_free/largest_dma down near ~2.7KB, right where ESP-NOW's
    // own TX/RX buffers have to come from) a dropped REQUEST/ACCEPT frame
    // reads as "nothing happened when I tapped Pair" with zero way to
    // tell that apart from "it's still working, wait." Leave the dialog
    // open with a real reason instead — on_pair_cancel_click() (its
    // existing Cancel button) still closes it either way, and
    // pairing_cancel() is already a safe no-op once the state has already
    // reset off PENDING_OUTGOING.
    if (s_pair_win) {
        pairing_state_t st = pairing_get_state();
        if (st == PAIRING_STATE_PAIRED) {
            close_pair_dialog();
        } else if (st != PAIRING_STATE_PENDING_OUTGOING) {
            purr_win_label_set(s_pair_win_lbl, "Pairing failed \xE2\x80\x94 timed out or rejected.\nCheck the other device is nearby and awake, then try again.");
        }
    }

    int paired_n = pairing_device_count();
    if (paired_n > NEARBY_MAX_PAIRED_ROWS) paired_n = NEARBY_MAX_PAIRED_ROWS;
    for (int i = 0; i < paired_n; i++) {
        paired_device_t pd;
        if (!pairing_device_at(i, &pd)) { paired_n = i; break; }
        snprintf(s_nearby_paired_row_bufs[i], sizeof(s_nearby_paired_row_bufs[i]), "%s%s",
                 pd.name, pairing_is_home_base(pd.mac) ? "  [home]" : "");
        s_nearby_paired_row_ptrs[i] = s_nearby_paired_row_bufs[i];
    }
    purr_win_list_set_items(s_nearby_paired_list, s_nearby_paired_row_ptrs, paired_n);

    char buf[32];
    snprintf(buf, sizeof(buf), "Paired devices: %d", paired_n);
    purr_win_label_set(s_nearby_paired_status_lbl, buf);
}

static void on_nearby_refresh_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    refresh_nearby();
}

static void on_pair_cancel_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    pairing_cancel();
    close_pair_dialog();
}

// No pairing_get_pending_code() call on this (initiator) side — as of the
// ECDH handshake upgrade (pairing_module.c), the real code isn't derivable
// until the responder's public key arrives in PAIRING_MSG_ACCEPT, which is
// also the exact moment pairing_on_frame() flips state straight to PAIRED.
// There's no meaningful window left to show it here before this dialog
// auto-closes on refresh_nearby()'s very next poll. The code the human
// actually needs to check is on the RESPONDER's confirm screen (real today
// on oled_ui_module.c's Heltec-companion flow) — that's where the genuine
// integrity check happens.
static void open_pair_dialog(const char *peer_name) {
    // A previous attempt's dialog can still be showing its own failure
    // message here now (refresh_nearby()'s own doc comment on why that no
    // longer auto-closes) — close it first rather than leaking/orphaning
    // that window handle. on_nearby_pair_click()'s own gate only checks
    // pairing_get_state(), which has already reset by the time a failed
    // dialog is still visible, so it alone doesn't stop this from being
    // reached while one is still up.
    close_pair_dialog();

    char msg[80];
    snprintf(msg, sizeof(msg), "Pairing with %s\nWaiting for confirmation...", peer_name);

    s_pair_win = purr_win_create("Pairing");
    s_pair_win_lbl = purr_win_label(s_pair_win, msg);
    purr_win_button(s_pair_win, "Cancel", on_pair_cancel_click, NULL);
    purr_win_show(s_pair_win);
}

// Acts on whichever row is currently selected in the list — purr_win's list
// widget only offers click/select, not a press-and-hold gesture, so pairing
// is a select-then-click-a-button flow.
static void on_nearby_pair_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    // Only mid-negotiation blocks starting a new one — already having other
    // paired devices doesn't (pairing_start() enforces the same rule; this
    // is just an early UI-side bail so this doesn't even try).
    pairing_state_t st = pairing_get_state();
    if (st != PAIRING_STATE_NONE && st != PAIRING_STATE_PAIRED) return;

    int idx = purr_win_list_get_selected(s_nearby_list);
    if (idx < 0) return;

    proximity_device_t dev;
    if (!proximity_device_at(idx, &dev)) return;
    if (!(dev.caps & PROXIMITY_CAP_RADIO_COMPANION)) return;   // not a pairable device

    if (pairing_start(dev.mac, dev.name)) {
        open_pair_dialog(dev.name);
    }
}

static void on_nearby_unpair_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    int idx = purr_win_list_get_selected(s_nearby_paired_list);
    if (idx < 0) return;
    paired_device_t pd;
    if (!pairing_device_at(idx, &pd)) return;
    pairing_forget(pd.mac);
    refresh_nearby();
}

// Toggles: selecting the current home base clears it, selecting any other
// paired row makes it the new one (pairing_set_home_base() only allows one
// at a time — see pairing.h).
static void on_nearby_set_home_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    int idx = purr_win_list_get_selected(s_nearby_paired_list);
    if (idx < 0) return;
    paired_device_t pd;
    if (!pairing_device_at(idx, &pd)) return;
    if (pairing_is_home_base(pd.mac)) {
        pairing_clear_home_base();
    } else {
        pairing_set_home_base(pd.mac);
    }
    refresh_nearby();
}

typedef struct {
    uint8_t mac[6];
    char    username[USER_MGR_USERNAME_MAX];
    char    password[64];
} setup_push_ctx_t;

// Fire-and-forget, same one-shot-task shape as app_manager.c's own
// remote_op_task() from this session — never call pairing_remote_oobe_
// push() (a blocking proximity_rpc_call()) from cupcake_task. Safe to
// touch s_setup_status_lbl directly from here despite that: purr_win_
// label_set()'s own backend already defers internally (see
// disconnect_to_connection_screen() and everywhere else in this file that
// touches a purr_win_* widget from refresh_task() without lv_async_call()-
// style marshaling) — this is the SAME portable API, not raw LVGL.
static void setup_push_task(void *arg) {
    setup_push_ctx_t *ctx = (setup_push_ctx_t *)arg;
    bool ok = pairing_remote_oobe_push(ctx->mac, ctx->username, ctx->password);
    if (s_setup_status_lbl) {
        purr_win_label_set(s_setup_status_lbl,
            ok ? "Setup complete!" : "Setup failed - already configured, or unreachable");
    }
    memset(ctx->password, 0, sizeof(ctx->password));
    free(ctx);
    s_setup_in_flight = false;
    vTaskDeleteWithCaps(NULL);
}

static void on_setup_push_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    if (s_setup_in_flight) return;   // already running — ignore a double-tap

    setup_push_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) return;
    memcpy(ctx->mac, s_setup_target_mac, 6);
    const char *u = s_setup_user_input ? purr_win_textarea_get(s_setup_user_input) : NULL;
    const char *p = s_setup_pass_input ? purr_win_textarea_get(s_setup_pass_input) : NULL;
    snprintf(ctx->username, sizeof(ctx->username), "%s", u ? u : "");
    snprintf(ctx->password, sizeof(ctx->password), "%s", p ? p : "");

    if (s_setup_status_lbl) purr_win_label_set(s_setup_status_lbl, "Pushing setup...");
    s_setup_in_flight = true;
    TaskHandle_t task = NULL;
    BaseType_t ok = xTaskCreateWithCaps(setup_push_task, "milkbar_setup", 4096, ctx, 3, &task, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        s_setup_in_flight = false;
        free(ctx);
        if (s_setup_status_lbl) purr_win_label_set(s_setup_status_lbl, "Could not start - try again");
    }
}

// Same push, but with both fields forced blank first — the exact
// zero-friction "keep the peer's own bootstrap default, no password" path
// pairing_remote_oobe_push()'s own doc comment describes, one tap instead
// of leaving the fields empty and remembering not to type anything.
static void on_setup_keep_default_click(purr_wid_t w, purr_event_t e, void *user) {
    if (s_setup_user_input) purr_win_textarea_clear(s_setup_user_input);
    if (s_setup_pass_input) purr_win_textarea_clear(s_setup_pass_input);
    on_setup_push_click(w, e, user);
}

static void on_setup_cancel_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    if (s_setup_dlg) purr_win_hide(s_setup_dlg);
}

// Lazy create-then-show, same pattern every other sub-window in this file
// uses. Reset fresh every open — s_setup_target_mac is set by the caller
// (on_nearby_setup_click()) just before this.
static void open_setup_dialog(void) {
    if (!s_setup_dlg) {
        s_setup_dlg = purr_win_create("Push Setup");
        purr_win_label(s_setup_dlg, "Username (blank = keep default):");
        s_setup_user_input = purr_win_textarea(s_setup_dlg, 100, 16);
        purr_win_label(s_setup_dlg, "Password (optional):");
        s_setup_pass_input = purr_win_textarea(s_setup_dlg, 100, 16);
        s_setup_status_lbl = purr_win_label(s_setup_dlg, "");

        purr_wid_t row = purr_win_row(s_setup_dlg, 3);
        purr_win_button(s_setup_dlg, "Push", on_setup_push_click, NULL);
        purr_win_button(s_setup_dlg, "Keep Default", on_setup_keep_default_click, NULL);
        purr_win_button(s_setup_dlg, "Cancel", on_setup_cancel_click, NULL);
        purr_win_layout_end(row);
    }

    if (s_setup_user_input) purr_win_textarea_clear(s_setup_user_input);
    if (s_setup_pass_input) purr_win_textarea_clear(s_setup_pass_input);
    if (s_setup_status_lbl) purr_win_label_set(s_setup_status_lbl, "");
    purr_win_show(s_setup_dlg);
}

static void on_nearby_setup_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    int idx = purr_win_list_get_selected(s_nearby_paired_list);
    if (idx < 0) return;
    paired_device_t pd;
    if (!pairing_device_at(idx, &pd)) return;
    memcpy(s_setup_target_mac, pd.mac, 6);
    open_setup_dialog();
}

static void open_nearby(purr_wid_t w, purr_event_t e, void *u) {
    (void)w; (void)e; (void)u;
    if (s_nearby_win) { purr_win_show(s_nearby_win); refresh_nearby(); return; }

    s_nearby_win = purr_win_create("Nearby");
    add_back_button(s_nearby_win);
    s_nearby_status_lbl = purr_win_label(s_nearby_win, "Proximity: starting...");

    purr_wid_t row = purr_win_row(s_nearby_win, 5);
    purr_win_button(s_nearby_win, "Refresh", on_nearby_refresh_click, NULL);
    purr_win_button(s_nearby_win, "Pair", on_nearby_pair_click, NULL);
    purr_win_button(s_nearby_win, "Unpair", on_nearby_unpair_click, NULL);
    purr_win_button(s_nearby_win, "Set Home", on_nearby_set_home_click, NULL);
    purr_win_button(s_nearby_win, "Setup", on_nearby_setup_click, NULL);
    purr_win_layout_end(row);

    s_nearby_list = purr_win_list(s_nearby_win, 100, 40);
    s_nearby_paired_status_lbl = purr_win_label(s_nearby_win, "Paired devices: 0");
    s_nearby_paired_list = purr_win_list(s_nearby_win, 100, 20);

    purr_win_show(s_nearby_win);
    // refresh_nearby() itself is gated on s_nearby_win, which is only just
    // now non-zero — call directly rather than waiting for refresh_task()'s
    // next pass, so the section isn't blank for up to REFRESH_MS on open.
    refresh_nearby();
}

static void refresh_task(void *arg) {
    (void)arg;
    while (s_running) {
        refresh_device_list();
        refresh_nearby();      // no-op fast path until Nearby is opened once
        refresh_dashboard();   // no-op fast path until Dashboard is opened
        // Short steps, not one REFRESH_MS vTaskDelay — same reasoning as
        // nearby_app.c's own refresh_task(): milkbar_app_deinit() blocks on
        // this task actually exiting, so how quickly it notices
        // s_running == false directly bounds how long a close/Kill stalls.
        for (int waited_ms = 0; waited_ms < REFRESH_MS && s_running; waited_ms += 200) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    if (s_refresh_done) xSemaphoreGive(s_refresh_done);
    vTaskDeleteWithCaps(NULL);
}

static int milkbar_app_init(void) {
    if (!s_refresh_done) s_refresh_done = xSemaphoreCreateBinary();

    // Connection screen — always built (disconnect_to_connection_screen()
    // needs somewhere valid to land back on regardless of how this app was
    // opened), but only actually SHOWN below when there's no already-active
    // remote session to route past instead. No login fields here any more
    // (see this file's top comment) — just paired-device browsing/pairing
    // management, plus its own Disconnect for ending an active session.
    s_win = purr_win_create("Connect");
    purr_win_label(s_win, "Paired devices:");
    s_device_list = purr_win_list(s_win, 100, 30);
    purr_win_list_on_select(s_device_list, on_device_list_event, NULL);

    purr_wid_t row = purr_win_row(s_win, 3);
    purr_win_button(s_win, "Refresh", on_refresh_click, NULL);
    purr_win_button(s_win, "Nearby", open_nearby, NULL);
    purr_win_button(s_win, "Disconnect", on_connection_disconnect_click, NULL);
    purr_win_layout_end(row);

    s_have_selection = false;
    refresh_device_list();
    milkbar_cfg_load();   // may flip s_have_selection back to true — see its own comment

    // Post-login hand-off: the systemui "Log in to a server" screen
    // (systemui_login.c) already ran the real pairing.h Phase A/B/C
    // exchange and called user_mgr_set_logged_in() before launching this
    // app (app_manager_launch_by_name("milkbar")) — by the time this runs,
    // user_mgr_current_user() IS that authenticated remote identity. Route
    // straight past Connection in that case: admin accounts land on
    // Dashboard, everyone else goes straight to the remote desktop — same
    // split this file always documented, just no longer decided by a login
    // this app itself ran.
    //
    // KNOWN LIMITATION: this only fires on a FRESH launch of this app — if
    // Milkbar was already open (browsing Connection/Nearby) when a separate
    // systemui login completed, app_manager_launch_by_name()'s own
    // "already running, just re-show" early return (app_manager.c) means
    // init() never runs again and this check never fires. Same limitation
    // every native app in this codebase already has (tapping a running
    // app's icon never re-runs its init()), not new here — just newly
    // reachable via this path. The user is left on whatever screen Milkbar
    // already had open; reopening it a second time doesn't help either,
    // for the same reason.
    const char *cur = user_mgr_current_user();
    if (user_mgr_is_logged_in() && user_mgr_account_type(cur) == USER_ACCOUNT_REMOTE) {
        if (!s_have_selection && pairing_get_home_base(s_selected_mac)) {
            s_have_selection = true;
        }
        if (user_mgr_is_admin(cur)) open_dashboard();
        else                        enter_remote_desktop();
    } else {
        purr_win_show(s_win);
    }

    s_running = true;
    // Background task refreshes Connection/Nearby/Dashboard's own local
    // reads — none of them block on a network round trip any more (the
    // remote-mode list/launch/stop RPCs now live entirely in app_manager.c/
    // app_manager_remote.c's own background tasks, see ensure_remote_
    // connected()) — but this still rides its own task rather than
    // cupcake_task, same as nearby_app.c's/meshdiag.c's identical pattern.
    xTaskCreateWithCaps(refresh_task, "milkbar_ref", 4096, NULL, 3, &s_refresh_task, MALLOC_CAP_SPIRAM);
    return 0;
}

static void milkbar_app_deinit(void) {
    s_running = false;
    if (s_refresh_done) xSemaphoreTake(s_refresh_done, pdMS_TO_TICKS(2000));
    s_refresh_task = NULL;

    // This app is the one that turned remote mode on (ensure_remote_
    // connected()) — turn it back off if it's still running when this app
    // is stopped, same hygiene reasoning systemui_xp.c's menu_logoff_cb()
    // already applies on a local log-off.
    app_manager_clear_remote();

    close_pair_dialog();
    if (s_setup_dlg) {
        purr_win_destroy(s_setup_dlg);
        s_setup_dlg = 0; s_setup_user_input = 0; s_setup_pass_input = 0; s_setup_status_lbl = 0;
    }
    if (s_nearby_win) {
        purr_win_destroy(s_nearby_win);
        s_nearby_win = 0; s_nearby_list = 0; s_nearby_status_lbl = 0;
        s_nearby_paired_list = 0; s_nearby_paired_status_lbl = 0;
    }

    if (s_dashboard_win) { purr_win_destroy(s_dashboard_win); s_dashboard_win = 0; s_dashboard_info_lbl = 0; }

    purr_win_destroy(s_win);
    s_win = 0; s_device_list = 0;
    s_have_selection = false;
}

// ── Module header ─────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(milkbar) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_APP,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "milkbar",
    .version           = "1.3.0",
    .kernel_min        = "0.11.1",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = milkbar_app_init,
    .deinit            = milkbar_app_deinit,
};
