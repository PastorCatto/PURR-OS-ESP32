// milkbar_app.c — PURR OS Milkbar (Remote Apps manager).
//
// Top list: this device's paired trust list (pairing.h — multi-device,
// see pairing_module.c). Selecting a row queries that device's app list
// over proximity_rpc_call() (REMOTEAPPS_ACTION_LIST, app_manager_remote.h)
// and shows it in the bottom list; Launch/Stop act on whichever app row is
// selected there. Same refresh_task + semaphore-guarded-deinit shape as
// msn.c/meshdiag.c in this codebase — the one difference is that
// proximity_rpc_call() is a real blocking network call (up to its own
// timeout), so it only ever runs on this app's own background task, never
// on cupcake_task (see proximity_rpc.h's own warning about that).
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
// beacon-table read, not a blocking network call like the RPC calls below,
// so it doesn't need the same "own dedicated task" treatment.
//
// "Milk Bottle" — originally a separate standalone app, then briefly a
// "Message" button bolted onto this screen — is neither: it's a synthetic
// row always pinned at index 0 of the (otherwise remote-fetched) app list
// below, right alongside whatever real apps REMOTEAPPS_ACTION_LIST returns
// for the selected device. Launch/Stop act on it exactly like any other
// row. That's deliberate — the whole point of Milk Bottle is to exercise
// this app's own list→launch→stop pipeline end-to-end as a live test, not
// to be a shortcut that bypasses it. It never shows up as its own
// installed app or home-screen icon; it only exists inside Milkbar.
// Reuses this file's own s_selected_mac/s_have_selection and refresh_task()
// — no second background task, no second device list.
//
// Send has two paths, chosen per-target: a full purr_win device (Cupcake/
// MiniWin/etc.) answers over proximity_rpc's MILKBAR_ACTION_MSG_SEND, same
// as always — but that only works if the target is running Milkbar itself
// (a purr_win app), which a headless device like Heltec V3's oled_ui can
// never be (oled_ui doesn't implement purr_win, doesn't run app_manager
// apps at all — see app_manager's own scan turning up 0 apps on it). Those
// devices already advertise PROXIMITY_CAP_RADIO_COMPANION in their beacon
// (oled_ui_module.c calls proximity_set_own_caps() at boot) and already
// have their own built-in message UI (oled_ui's SCREEN_SEND/SCREEN_MESSAGES,
// fed by meshtastic's LoRa broadcast, not proximity_rpc). So when the
// selected device is flagged radio-companion, Send falls back to
// mesh_manager_send_text() instead — landing directly in that device's
// existing SCREEN_MESSAGES ring buffer, no new module or wire format needed.

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
#include "proximity_rpc.h"
#include "app_manager_remote.h"
#include "meshtastic.h"

#ifdef CONFIG_PURR_UI_LVGL
#include "lvgl.h"
#endif

#define REFRESH_MS        2000
#define RPC_TIMEOUT_MS    3000
#define MAX_DEVICE_ROWS   PAIRING_MAX_DEVICES
#define MAX_APP_ROWS      32   // PROXIMITY_RPC_MAX_MSG / sizeof(remote_app_entry_t) headroom, see app_manager_remote.h

// ── Milk Bottle screen ───────────────────────────────────────────────────
#define MILKBAR_ACTION_MSG_SEND 0x3000   // same wire value Milk Bottle used — nothing shipped with it yet, pure rename
#define MSG_MAX_TEXT 64

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

static purr_win_t s_win         = 0;
static purr_wid_t s_device_list = 0;
static purr_wid_t s_app_list    = 0;
static purr_wid_t s_status_lbl  = 0;

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
#define MILKBAR_CFG_MAGIC 0x4D4C4B01u   // "MLK" + struct version 1

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
// have refresh_task() start issuing proximity_rpc_call()s to it regardless.
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

static char        s_app_row_bufs[MAX_APP_ROWS][64];
static const char *s_app_row_ptrs[MAX_APP_ROWS];
#ifdef CONFIG_PURR_UI_LVGL
// Every row currently gets the same generic icon — remote app entries
// (remote_app_entry_t, app_manager_remote.h) carry no icon data at all
// (the LOCAL app_entry_t doesn't either; icons are a UI-layer-only lookup
// today, see cupcake's own app-drawer icon mapping), so the real per-row
// differentiator is the name text, not the glyph. Matches how this was
// explicitly scoped when asked for: "the name of the app... because
// they're all the same icon right now."
static const char *s_app_row_icons[MAX_APP_ROWS];
#endif
static int          s_app_count = 0;
static remote_app_entry_t s_last_apps[MAX_APP_ROWS];   // raw entries, for Launch/Stop to read state/name by row index

// ── Milk Bottle screen state ────────────────────────────────────────────
static purr_win_t s_msg_win     = 0;
static purr_wid_t s_msg_big_lbl = 0;
static purr_wid_t s_msg_input   = 0;

// Last received message — written from proximity_rpc's own dispatch
// context (handle_send_msg(), not cupcake_task), read/rendered by
// refresh_task(). Plain flag-guarded copy, same "background writes, one
// UI-owning task reads" shape as homebase.c's s_present.
static char           s_last_rx_text[MSG_MAX_TEXT + 1];
static volatile bool  s_rx_is_new = false;

// proximity_rpc's own SEND_MSG responder — see this file's top comment for
// why "receive" only works while Milkbar itself is open (registered/
// unregistered in milkbar_app_init()/_deinit(), same as the app list
// action IDs don't need registering/unregistering since REMOTEAPPS_
// ACTION_LIST etc. are answered by app_manager_remote.c, an always-on
// module — this one is answered directly by this app instead, so it's
// scoped to the app's own lifetime).
static bool handle_send_msg(const uint8_t mac[6], uint16_t action_id,
                             const uint8_t *req, size_t req_len,
                             uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out) {
    (void)mac; (void)action_id; (void)resp_out; (void)resp_cap;
    *resp_len_out = 0;
    if (req_len == 0 || req_len > MSG_MAX_TEXT) return false;
    memcpy(s_last_rx_text, req, req_len);
    s_last_rx_text[req_len] = 0;
    s_rx_is_new = true;
    return true;
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

// Row 0 is always "Milk Bottle" — a local, in-process test app, not
// something fetched from the remote device — so it stays available even
// when the remote query below fails outright (e.g. a headless device that
// can't answer REMOTEAPPS_ACTION_LIST at all, like Heltec's oled_ui). Real
// remote entries (if any) start at row 1; s_last_apps[] stays 0-indexed to
// match them, so Launch/Stop subtract 1 from the list selection.
static void set_milkbottle_row(int row) {
    snprintf(s_app_row_bufs[row], sizeof(s_app_row_bufs[row]), "Milk Bottle%s",
             s_msg_win ? "  (open)" : "");
    s_app_row_ptrs[row] = s_app_row_bufs[row];
#ifdef CONFIG_PURR_UI_LVGL
    s_app_row_icons[row] = LV_SYMBOL_CALL;
#endif
}

// Runs on refresh_task() — see this file's top comment for why this can
// never be called from cupcake_task.
static void refresh_app_list_from_remote(void) {
    if (!s_have_selection) {
        s_app_count = 0;
        if (s_app_list) purr_win_list_set_items(s_app_list, s_app_row_ptrs, 0);
        if (s_status_lbl) purr_win_label_set(s_status_lbl, "Select a paired device");
        return;
    }

    set_milkbottle_row(0);

    uint8_t resp[PROXIMITY_RPC_MAX_MSG];
    size_t  resp_len = 0;
    bool ok = proximity_rpc_call(s_selected_mac, REMOTEAPPS_ACTION_LIST, NULL, 0,
                                  resp, sizeof(resp), &resp_len, RPC_TIMEOUT_MS);
    if (!ok) {
        s_app_count = 1;   // Milk Bottle row still stands even with no remote app list
        if (s_app_list) {
#ifdef CONFIG_PURR_UI_LVGL
            purr_win_list_set_items_icon(s_app_list, s_app_row_ptrs, s_app_row_icons, s_app_count);
#else
            purr_win_list_set_items(s_app_list, s_app_row_ptrs, s_app_count);
#endif
        }
        if (s_status_lbl) purr_win_label_set(s_status_lbl, "Remote device not responding");
        return;
    }

    int n = (int)(resp_len / sizeof(remote_app_entry_t));
    if (n > MAX_APP_ROWS - 1) n = MAX_APP_ROWS - 1;
    memcpy(s_last_apps, resp, (size_t)n * sizeof(remote_app_entry_t));
    for (int i = 0; i < n; i++) {
        int row = i + 1;
        snprintf(s_app_row_bufs[row], sizeof(s_app_row_bufs[row]), "%s%s",
                 s_last_apps[i].name, s_last_apps[i].state == 1 /* APP_STATE_RUNNING */ ? "  (running)" : "");
        s_app_row_ptrs[row] = s_app_row_bufs[row];
#ifdef CONFIG_PURR_UI_LVGL
        s_app_row_icons[row] = LV_SYMBOL_FILE;
#endif
    }
    s_app_count = n + 1;

    if (s_app_list) {
#ifdef CONFIG_PURR_UI_LVGL
        purr_win_list_set_items_icon(s_app_list, s_app_row_ptrs, s_app_row_icons, s_app_count);
#else
        purr_win_list_set_items(s_app_list, s_app_row_ptrs, s_app_count);
#endif
    }
    if (s_status_lbl) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%d remote app%s", n, n == 1 ? "" : "s");
        purr_win_label_set(s_status_lbl, buf);
    }
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
    if (s_status_lbl) purr_win_label_set(s_status_lbl, "Loading...");
    // Actual fetch happens on refresh_task()'s own next pass, not here —
    // this callback runs on cupcake_task.
}

// Forward-declared: Milk Bottle's own screen (defined further down, right
// after the message-send/receive machinery it opens).
static void open_message_screen(void);

static void on_launch_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    if (!s_have_selection) return;
    int idx = purr_win_list_get_selected(s_app_list);
    if (idx < 0 || idx >= s_app_count) return;

    if (idx == 0) { open_message_screen(); return; }   // row 0 is always Milk Bottle

    int i = idx - 1;
    uint8_t resp[16]; size_t resp_len = 0;
    bool ok = proximity_rpc_call(s_selected_mac, REMOTEAPPS_ACTION_LAUNCH,
                                  (const uint8_t *)s_last_apps[i].name, strlen(s_last_apps[i].name),
                                  resp, sizeof(resp), &resp_len, RPC_TIMEOUT_MS);
    if (s_status_lbl) purr_win_label_set(s_status_lbl, ok ? "Launched" : "Launch failed");
}

static void on_stop_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    if (!s_have_selection) return;
    int idx = purr_win_list_get_selected(s_app_list);
    if (idx < 0 || idx >= s_app_count) return;

    if (idx == 0) {   // row 0 is always Milk Bottle — "Stop" just closes its own screen
        if (s_msg_win) purr_win_hide(s_msg_win);
        return;
    }

    int i = idx - 1;
    uint8_t resp[16]; size_t resp_len = 0;
    bool ok = proximity_rpc_call(s_selected_mac, REMOTEAPPS_ACTION_STOP,
                                  (const uint8_t *)s_last_apps[i].name, strlen(s_last_apps[i].name),
                                  resp, sizeof(resp), &resp_len, RPC_TIMEOUT_MS);
    if (s_status_lbl) purr_win_label_set(s_status_lbl, ok ? "Stopped" : "Stop failed");
}

static void on_refresh_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    refresh_device_list();
}

typedef struct {
    uint8_t mac[6];
    char    text[MSG_MAX_TEXT + 1];
} msg_send_ctx_t;

static void send_msg_task(void *arg) {
    msg_send_ctx_t *ctx = (msg_send_ctx_t *)arg;
    uint8_t resp[4]; size_t resp_len = 0;
    // Fire-and-forget from the UI's perspective — result isn't surfaced
    // (same "optimistic, no confirmation" precedent as MSN's own send
    // button).
    proximity_rpc_call(ctx->mac, MILKBAR_ACTION_MSG_SEND,
                        (const uint8_t *)ctx->text, strlen(ctx->text),
                        resp, sizeof(resp), &resp_len, RPC_TIMEOUT_MS);
    free(ctx);
    vTaskDeleteWithCaps(NULL);
}

static void on_msg_send_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    if (!s_have_selection) return;
    const char *text = purr_win_textarea_get(s_msg_input);
    if (!text || !*text) return;

    // Radio-companion target (Heltec's oled_ui, or anything else that
    // advertises the same cap) can't run Milkbar/answer proximity_rpc at
    // all — see this file's top comment. Fall back to a mesh broadcast,
    // which oled_ui's own SCREEN_MESSAGES already listens for.
    // mesh_manager_send_text() is documented safe to call directly from a
    // UI callback (encoding is cheap; the actual radio TX happens later on
    // meshtastic's own task) — no background task needed for this path.
    if (device_caps(s_selected_mac) & PROXIMITY_CAP_RADIO_COMPANION) {
        mesh_manager_send_text(MESH_BROADCAST, 0, text);
        purr_win_textarea_clear(s_msg_input);
        return;
    }

    msg_send_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) return;
    memcpy(ctx->mac, s_selected_mac, 6);
    size_t n = strlen(text);
    if (n > MSG_MAX_TEXT) n = MSG_MAX_TEXT;
    memcpy(ctx->text, text, n);
    ctx->text[n] = 0;

    // proximity_rpc_call() must never run on cupcake_task — dedicated
    // background task per send, same rule as this file's app-list RPCs.
    TaskHandle_t task = NULL;
    BaseType_t ok = xTaskCreateWithCaps(send_msg_task, "milkbar_msgtx", 4096, ctx, 3, &task, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) free(ctx);

    purr_win_textarea_clear(s_msg_input);
}

static void on_msg_back_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    purr_win_hide(s_msg_win);
}

// Lazy create-then-show, Back hides — same pattern MSN's Nodes/Channels/
// Messages screens use. Operates on this file's own s_selected_mac —
// whichever device is selected in the main list, no separate picker.
static void open_message_screen(void) {
    if (s_msg_win) { purr_win_show(s_msg_win); return; }
    s_msg_win = purr_win_create("Message");
    purr_win_button(s_msg_win, "< Back", on_msg_back_click, NULL);
    s_msg_big_lbl = purr_win_label(s_msg_win, "");
    purr_win_label_align(s_msg_big_lbl, PURR_ALIGN_CENTER);
    s_msg_input = purr_win_textarea(s_msg_win, 100, 20);
    purr_win_button(s_msg_win, "Send", on_msg_send_click, NULL);
    purr_win_show(s_msg_win);
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

    // Auto-close the confirm dialog once the peer accepts (PAIRED) or the
    // request ends any other way (rejected/timed out both surface as a
    // reset back to NONE — see pairing_module.c) — the dialog has nothing
    // further to wait for in either case.
    if (s_pair_win && pairing_get_state() != PAIRING_STATE_PENDING_OUTGOING) {
        close_pair_dialog();
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

static void open_nearby(purr_wid_t w, purr_event_t e, void *u) {
    (void)w; (void)e; (void)u;
    if (s_nearby_win) { purr_win_show(s_nearby_win); refresh_nearby(); return; }

    s_nearby_win = purr_win_create("Nearby");
    add_back_button(s_nearby_win);
    s_nearby_status_lbl = purr_win_label(s_nearby_win, "Proximity: starting...");

    purr_wid_t row = purr_win_row(s_nearby_win, 4);
    purr_win_button(s_nearby_win, "Refresh", on_nearby_refresh_click, NULL);
    purr_win_button(s_nearby_win, "Pair", on_nearby_pair_click, NULL);
    purr_win_button(s_nearby_win, "Unpair", on_nearby_unpair_click, NULL);
    purr_win_button(s_nearby_win, "Set Home", on_nearby_set_home_click, NULL);
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
        refresh_nearby();                 // no-op fast path until Nearby is opened once
        refresh_app_list_from_remote();   // no-op fast path if nothing selected yet
        if (s_rx_is_new) {
            s_rx_is_new = false;
            if (s_msg_big_lbl) purr_win_label_set_big(s_msg_big_lbl, s_last_rx_text);
        }
        // Short steps, not one REFRESH_MS vTaskDelay — same reasoning as
        // nearby_app.c's own refresh_task(): milkbar_app_deinit() blocks on
        // this task actually exiting, so how quickly it notices
        // s_running == false directly bounds how long a close/Kill stalls.
        // A live proximity_rpc_call() in flight when s_running flips to
        // false still has to finish or time out first either way — up to
        // RPC_TIMEOUT_MS, not bounded by this loop's own step size.
        for (int waited_ms = 0; waited_ms < REFRESH_MS && s_running; waited_ms += 200) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    if (s_refresh_done) xSemaphoreGive(s_refresh_done);
    vTaskDeleteWithCaps(NULL);
}

static int milkbar_app_init(void) {
    if (!s_refresh_done) s_refresh_done = xSemaphoreCreateBinary();

    proximity_rpc_register(MILKBAR_ACTION_MSG_SEND, handle_send_msg);

    s_win = purr_win_create("Milkbar");
    purr_win_label(s_win, "Paired devices:");
    s_device_list = purr_win_list(s_win, 100, 30);
    purr_win_list_on_select(s_device_list, on_device_list_event, NULL);

    purr_wid_t row = purr_win_row(s_win, 3);
    purr_win_button(s_win, "Refresh", on_refresh_click, NULL);
    purr_win_button(s_win, "Launch", on_launch_click, NULL);
    purr_win_button(s_win, "Stop", on_stop_click, NULL);
    purr_win_button(s_win, "Nearby", open_nearby, NULL);
    purr_win_layout_end(row);

    s_status_lbl = purr_win_label(s_win, "Select a paired device");
    s_app_list = purr_win_list(s_win, 100, 30);

    s_have_selection = false;
    s_rx_is_new = false;
    s_last_rx_text[0] = 0;
    refresh_device_list();
    milkbar_cfg_load();   // may flip s_have_selection back to true — see its own comment
    purr_win_show(s_win);

    s_running = true;
    // Background task does the (potentially slow, blocking) proximity_rpc_
    // call() work — see this file's top comment. PSRAM-backed stack: no
    // NVS/flash access anywhere in this task's own body, same rationale as
    // nearby_app.c's identical refresh_task() pattern.
    xTaskCreateWithCaps(refresh_task, "milkbar_ref", 4096, NULL, 3, &s_refresh_task, MALLOC_CAP_SPIRAM);
    return 0;
}

static void milkbar_app_deinit(void) {
    s_running = false;
    if (s_refresh_done) xSemaphoreTake(s_refresh_done, pdMS_TO_TICKS(RPC_TIMEOUT_MS + 500));
    s_refresh_task = NULL;

    // Stop answering MSG_SEND once the app isn't open to show it anymore —
    // see this file's top comment on receive-only-while-open scope.
    proximity_rpc_register(MILKBAR_ACTION_MSG_SEND, NULL);

    if (s_msg_win) { purr_win_destroy(s_msg_win); s_msg_win = 0; s_msg_big_lbl = 0; s_msg_input = 0; }

    close_pair_dialog();
    if (s_nearby_win) {
        purr_win_destroy(s_nearby_win);
        s_nearby_win = 0; s_nearby_list = 0; s_nearby_status_lbl = 0;
        s_nearby_paired_list = 0; s_nearby_paired_status_lbl = 0;
    }

    purr_win_destroy(s_win);
    s_win = 0; s_device_list = 0; s_app_list = 0; s_status_lbl = 0;
    s_have_selection = false;
}

// ── Module header ─────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(milkbar) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_APP,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "milkbar",
    .version           = "1.2.0",
    .kernel_min        = "0.11.1",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = milkbar_app_init,
    .deinit            = milkbar_app_deinit,
};
