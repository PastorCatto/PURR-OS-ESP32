// msn.c — PURR OS MSN (Mesh Social Network): a tile-grid Home screen (Nodes/
// Messages/Channels/Manage) plus private 1:1 chat and multi-channel group
// chat over whichever mesh backend (Meshtastic or MeshCore) is currently
// active. Renamed/refactored from meshchat.c,
// which talked to meshtastic.h directly — this file now talks only through
// msn_backend.h's protocol-agnostic vtable (msn_backend_meshtastic.c /
// msn_backend_meshcore.c own the actual meshtastic.h/meshcore_api.h calls).
//
// Launch screen is a backend chooser: two big buttons (Meshtastic/MeshCore)
// with an active-indicator next to whichever one purr_kernel_get_module()
// shows is actually running. Tapping the active one enters MSN's Home
// screen below. Tapping the inactive one live-switches to it via
// purr_kernel_mesh_backend_switch() — persists the choice and swaps which
// mesh module is loaded, no reboot (see that function's own comment in
// purr_kernel.c for how the one-physical-radio mutual exclusion still holds
// without one).
//
// Home screen (open_msn_home()) is a small tile-grid navigation menu —
// falls back to a plain list on any UI backend that doesn't implement
// purr_win_tile_grid_create() (see catcall_ui.h; MSN must stay portable
// across every backend, not just Cupcake) — with four destinations: Nodes
// (the buddy/contact list, its own full screen), Messages (a recent-
// activity inbox across all DMs/rooms touched since this launch, most-
// recent-first), Channels (the room list, its own full screen, plus Add
// Room), and Manage (forget nodes/rooms). Replaces an earlier design where
// Rooms and Buddies shared one window at 40% height each — cramped once a
// mesh has more than a handful of nodes. Home itself is never destroyed
// once built; sub-screens hide back to it (settings.c's own category-
// picker pattern), not the destroy/recreate the chooser→Home transition
// still uses.
//
// A buddy is DMed using whichever channel/key the active backend's own
// contact record implies (Meshtastic: the contact's last-heard channel;
// MeshCore: per-contact ECDH, no channel involved) — msn_backend_t's
// send_text() owns that distinction internally.
//
// Message logs (both rooms and DMs) persist across closing and reopening a
// chat window (and across closing/relaunching the whole app) — only the
// window widgets are torn down and rebuilt, the log buffers below are
// static/heap globals that outlive them. SD history paths are keyed by
// msn_contact_t.id_str / the channel's on-air hash byte — both stable
// across a contact/channel table re-sort, and (for Meshtastic) byte-for-
// byte identical to what meshchat.c already wrote, so existing users' chat
// history is picked up unchanged after this rename.

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "purr_win.h"
#include "purr_kernel.h"
#include "purr_module.h"
#include "msn_backend.h"
#include <mbedtls/sha256.h>

// Cupcake-only: richer list rows (a real icon glyph per row, matching
// Meshtastic's own device-ui look) instead of the plain-text-only rendering
// every other backend gets. cupcake.h is always compiled (every UI backend's
// component builds for every device — see modulestrap's component-manifest
// comment), so this include is safe even on non-Cupcake devices; only the
// actual cupcake_win_list_set_items_icon() *calls* below are guarded, since
// LV_SYMBOL_* glyphs are meaningless (and their backing font absent) under
// MiniWin/Pounce.
#ifdef CONFIG_PURR_UI_BACKEND_CUPCAKE
#include "cupcake.h"
#endif

#define MAX_CHATS      16
#define CHAT_LOG_LEN   1024
#define MAX_ROOMS      8   // matches MESH_MAX_CHANNELS / MC_MAX_CHANNELS
// A buddy is shown as "online" if heard from within this window; purely a
// UI-freshness cue, computed from msn_contact_t.last_seen_ms_ago (already
// normalized by the active backend), not any protocol-specific clock.
#define ONLINE_WINDOW_MS (15UL * 60UL * 1000UL)

// ── Backend selection ────────────────────────────────────────────────────
static const msn_backend_t *s_backend = NULL;

// ── Chooser screen state ─────────────────────────────────────────────────
static purr_win_t s_chooser_win        = 0;
static purr_wid_t s_meshtastic_status  = 0;
static purr_wid_t s_meshcore_status    = 0;
static purr_wid_t s_force_local_status = 0;
static purr_win_t s_switch_confirm_win = 0;
static purr_wid_t s_switch_confirm_lbl = 0;
static purr_mesh_backend_t s_pending_switch_target;

// ── Home screen + sub-screens ─────────────────────────────────────────────
// s_home_win is created once by open_msn_home() and never destroyed until
// close_chat_ui() — Nodes/Messages/Channels lazily-create-then-show/hide
// back to it, same pattern settings.c already uses for its own category
// picker. s_buddy_list/s_room_list keep their names (still "the widget
// showing buddies"/"the widget showing rooms") even though they now live
// inside the Nodes/Channels windows instead of the old combined screen.

static purr_win_t  s_home_win       = 0;
static purr_wid_t  s_home_tile_grid = 0;   // 0 if the active backend doesn't implement tile grids
static purr_wid_t  s_home_list      = 0;   // fallback, built instead when s_home_tile_grid creation failed
static purr_wid_t  s_status_lbl     = 0;

static purr_win_t  s_nodes_win      = 0;
static purr_wid_t  s_buddy_list     = 0;

static purr_win_t  s_channels_win   = 0;
static purr_wid_t  s_room_list      = 0;

static purr_win_t  s_messages_win   = 0;
static purr_wid_t  s_messages_list  = 0;

static TaskHandle_t s_refresh_task = NULL;
static bool         s_running      = false;
// Given by buddy_refresh_task() right before it self-deletes, waited on by
// close_chat_ui() before it destroys any window — see that function's
// comment for the use-after-free this closes. Same shape as meshchat.c's
// own original fix for the "close_ctrl_body_start" hang.
static SemaphoreHandle_t s_refresh_done = NULL;

// ── Buddies (1:1 DMs) ────────────────────────────────────────────────────

static char         s_buddy_id_strs[MAX_CHATS][24];
static char         s_buddy_labels[MAX_CHATS][80];
static const char   *s_buddy_label_ptrs[MAX_CHATS];
static int           s_buddy_count = 0;
#ifdef CONFIG_PURR_UI_BACKEND_CUPCAKE
// Cupcake-only per-row icon glyphs — parallel to s_buddy_label_ptrs, only
// ever populated/read when the active backend is Cupcake (see
// refresh_buddy_list()).
static const char   *s_buddy_icon_ptrs[MAX_CHATS];
#endif

// Per-buddy chat state (parallel arrays, indexed same as s_buddy_id_strs).
static char        s_chat_logs[MAX_CHATS][CHAT_LOG_LEN];
static size_t      s_chat_loglen[MAX_CHATS];
static purr_win_t  s_chat_win[MAX_CHATS];
static purr_wid_t  s_chat_out[MAX_CHATS];
static purr_wid_t  s_chat_in[MAX_CHATS];
static int         s_chat_scroll[MAX_CHATS];   // lines skipped from the top of the log — see render_chat_view()
// Stamped in chat_log_append() — 0 means "nothing sent/received this
// launch." Drives the Messages screen's recency sort; reset to 0 in
// close_chat_ui() so the inbox starts empty again on relaunch, same as the
// rest of MSN's per-launch (not per-boot, not persisted) UI state.
static uint32_t    s_buddy_last_ms[MAX_CHATS];

// ── Rooms (channel group chat) ───────────────────────────────────────────

static char        s_room_names[MAX_ROOMS][12];
// Separate from s_room_names — that buffer is the raw channel name (reused
// as-is for window titles/room_path() elsewhere), too small to append a
// display tag into without truncating real names longer than a few chars.
static char        s_room_labels[MAX_ROOMS][24];
static const char  *s_room_label_ptrs[MAX_ROOMS];
static int          s_room_count = 0;
#ifdef CONFIG_PURR_UI_BACKEND_CUPCAKE
// Cupcake-only per-row icon glyphs — parallel to s_room_label_ptrs, only
// ever populated/read when the active backend is Cupcake (see
// refresh_room_list()).
static const char  *s_room_icon_ptrs[MAX_ROOMS];
#endif

// Heap-allocated from PSRAM (not static arrays) — internal DRAM on this
// board is already razor-thin (see cupcake_module.c's static-stack-sizing
// comment); MAX_ROOMS(8) * CHAT_LOG_LEN(1024) static buffers here would
// overflow dram0_0_seg the same way one 1024-byte static broadcast log
// previously did.
static char        *s_room_logs[MAX_ROOMS];
static size_t        s_room_loglen[MAX_ROOMS];
static purr_win_t   s_room_win[MAX_ROOMS];
static purr_wid_t   s_room_out[MAX_ROOMS];
static purr_wid_t   s_room_in[MAX_ROOMS];
static int          s_room_scroll[MAX_ROOMS];   // lines skipped from the top of the log
// Same shape/reset rule as s_buddy_last_ms above.
static uint32_t     s_room_last_ms[MAX_ROOMS];

// ── Add Room window ──────────────────────────────────────────────────────

static purr_win_t  s_addroom_win      = 0;
static purr_wid_t  s_addroom_name_in  = 0;
static purr_wid_t  s_addroom_psk_in   = 0;

// ── Manage window (forget nodes/rooms) ───────────────────────────────────

static purr_win_t  s_manage_win       = 0;
static purr_wid_t  s_manage_node_list = 0;
static purr_wid_t  s_manage_room_list = 0;

// ── SD persistence ────────────────────────────────────────────────────────
// Real, unbounded history on /sdcard/meshchat/ (directory name kept exactly
// as-is — not renamed to /sdcard/msn/ — specifically so existing users'
// Meshtastic chat history is picked up unchanged; kernel_tdp_boot.c already
// ensures this directory exists at boot). Independent of the in-memory
// display buffers above (CHAT_LOG_LEN-capped scrollback, unchanged). Keyed
// by msn_contact_t.id_str / a channel's on-air hash byte — stable identity,
// not table index (indices shift when a contact/room is forgotten).
// Everything here no-ops behind purr_kernel_sd_available() — no SD card
// means today's in-memory-only behavior, not a crash.

static void dm_path(const char *id_str, char *out, size_t out_max) {
    snprintf(out, out_max, "/sdcard/meshchat/dm_%s.txt", id_str);
}

static void room_path(int channel_idx, char *out, size_t out_max) {
    uint8_t hash = 0;
    s_backend->channel_hash(channel_idx, &hash);
    snprintf(out, out_max, "/sdcard/meshchat/room_%02X.txt", hash);
}

static void append_to_sd(const char *path, const char *text) {
    if (!purr_kernel_sd_available()) return;
    FILE *f = fopen(path, "a");
    if (!f) return;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

// Seeds an in-memory log buffer from the tail of its SD file — called once
// per conversation, the first time it's opened this boot (buf still empty).
static void load_tail_from_sd(const char *path, char *buf, size_t *len, size_t buf_cap) {
    if (!purr_kernel_sd_available()) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0) { fclose(f); return; }
    size_t cap  = buf_cap - 1;
    size_t want = (size_t)fsize;
    if (want > cap) {
        fseek(f, (long)((size_t)fsize - cap), SEEK_SET);
        want = cap;
    } else {
        fseek(f, 0, SEEK_SET);
    }
    size_t n = fread(buf, 1, want, f);
    buf[n] = '\0';
    *len = n;
    fclose(f);
}

// ── Node status formatting (portable plain text, MUI-inspired) ────────────
// catcall_ui_t has no image/icon widget at all (checked — label/button/list/
// textarea/layout only), and MSN must keep working on non-LVGL backends
// (MiniWin, Pounce) as well as every LVGL one — so unlike cupcake_ui.c's own
// status bar (which can reach for LV_SYMBOL_BATTERY_* directly, being
// backend-internal code), this stays plain ASCII rather than pulling LVGL
// symbols or real bitmap icons into an app. A 5-step bar meter for signal,
// "NN%" for battery, "Nh" for hop count — all omitted individually when the
// backend reports its own "unknown" sentinel (MSN_RSSI_UNKNOWN/HOPS_UNKNOWN/
// BATTERY_UNKNOWN, msn_backend.h) rather than showing a misleading zero.

// RSSI thresholds are a reasonable LoRa-signal bucketing, not calibrated
// against a specific measurement — real Meshtastic doesn't publish official
// dBm-to-bar-count breakpoints for its own UI either.
static const char *signal_bars(int8_t rssi_dbm) {
    if (rssi_dbm == MSN_RSSI_UNKNOWN) return "[?]";
    if (rssi_dbm > -60)  return "[####]";
    if (rssi_dbm > -80)  return "[###.]";
    if (rssi_dbm > -95)  return "[##..]";
    if (rssi_dbm > -110) return "[#...]";
    return "[....]";
}

// Appends "  <bars>  NN%  Nh" to buf (skipping battery/hops individually if
// unknown) — shared by the buddy list and any future per-node detail view.
static void append_node_status(char *buf, size_t buf_max, const msn_contact_t *info) {
    size_t len = strlen(buf);
    int n = snprintf(buf + len, buf_max - len, "  %s", signal_bars(info->rssi_dbm));
    if (n > 0) len += (size_t)n;
    if (info->battery_pct != MSN_BATTERY_UNKNOWN) {
        n = snprintf(buf + len, buf_max - len, "  %d%%", info->battery_pct);
        if (n > 0) len += (size_t)n;
    }
    if (info->hops_away != MSN_HOPS_UNKNOWN) {
        // Spelled out, not "Nh" — a bare "h" abbreviation reads ambiguously
        // (misheard as a unit like inches by at least one accessibility/
        // dictation tool in practice).
        snprintf(buf + len, buf_max - len, "  %d hop%s",
                 info->hops_away, info->hops_away == 1 ? "" : "s");
    }
}

#ifdef CONFIG_PURR_UI_BACKEND_CUPCAKE
// Per-node icon glyph for Cupcake's richer list rendering — battery level
// takes priority (thresholds match cupcake_ui.c's own status-bar battery
// icon selection, for a consistent look), falling back to a generic
// signal-present glyph when battery is unknown but the node has been heard
// at all, and no icon otherwise. LV_SYMBOL_* has no bar-graduated wifi
// glyph, so unlike battery this can't reflect actual signal strength —
// that's still carried by append_node_status()'s text suffix below.
static const char *node_icon_symbol(const msn_contact_t *info) {
    if (info->battery_pct != MSN_BATTERY_UNKNOWN) {
        int pct = info->battery_pct;
        if (pct > 80) return LV_SYMBOL_BATTERY_FULL;
        if (pct > 55) return LV_SYMBOL_BATTERY_3;
        if (pct > 30) return LV_SYMBOL_BATTERY_2;
        if (pct > 10) return LV_SYMBOL_BATTERY_1;
        return LV_SYMBOL_BATTERY_EMPTY;
    }
    if (info->rssi_dbm != MSN_RSSI_UNKNOWN) return LV_SYMBOL_WIFI;
    return NULL;
}
#endif

// ── Buddy list ────────────────────────────────────────────────────────────

static void refresh_buddy_list(void) {
    purr_kernel_ui_breadcrumb("msn:refresh_buddy");
    int n = s_backend->contact_count();
    if (n > MAX_CHATS) n = MAX_CHATS;
    s_buddy_count = n;

    for (int i = 0; i < n; i++) {
        msn_contact_t info;
        if (!s_backend->contact_at(i, &info)) { s_buddy_count = i; break; }
        strncpy(s_buddy_id_strs[i], info.id_str, sizeof(s_buddy_id_strs[i]) - 1);
        if (info.last_seen_ms_ago == MSN_LAST_SEEN_UNKNOWN) {
            snprintf(s_buddy_labels[i], sizeof(s_buddy_labels[i]), "%s (never heard)", info.name);
        } else if (info.last_seen_ms_ago < ONLINE_WINDOW_MS) {
            snprintf(s_buddy_labels[i], sizeof(s_buddy_labels[i]), "%s (online)", info.name);
        } else {
            uint32_t age_min = info.last_seen_ms_ago / 60000UL;
            snprintf(s_buddy_labels[i], sizeof(s_buddy_labels[i]), "%s (%lum ago)",
                     info.name, (unsigned long)age_min);
        }
        append_node_status(s_buddy_labels[i], sizeof(s_buddy_labels[i]), &info);
        s_buddy_label_ptrs[i] = s_buddy_labels[i];
#ifdef CONFIG_PURR_UI_BACKEND_CUPCAKE
        s_buddy_icon_ptrs[i] = node_icon_symbol(&info);
#endif
    }

    if (s_buddy_list) {
        purr_kernel_ui_breadcrumb("msn:buddy_list_set_items");
#ifdef CONFIG_PURR_UI_BACKEND_CUPCAKE
        cupcake_win_list_set_items_icon(s_buddy_list, s_buddy_label_ptrs, s_buddy_icon_ptrs, s_buddy_count);
#else
        purr_win_list_set_items(s_buddy_list, s_buddy_label_ptrs, s_buddy_count);
#endif
    }
}

// ── Room list ─────────────────────────────────────────────────────────────

static void refresh_room_list(void) {
    purr_kernel_ui_breadcrumb("msn:refresh_room");
    int n = s_backend->channel_count();
    if (n > MAX_ROOMS) n = MAX_ROOMS;
    s_room_count = n;

    for (int i = 0; i < n; i++) {
        s_backend->channel_name(i, s_room_names[i], sizeof(s_room_names[i]));
        // Index 0 is always the fixed default channel (LongFast for
        // meshtastic, the analogous primary for meshcore — both backends'
        // channel_add() only ever appends starting at index 1) — every
        // channel here is actually PSK-encrypted including the default, so
        // this isn't an "encrypted or not" claim, just "the public default
        // every device speaks out of the box" vs. "a private room only
        // people who know the passphrase can join," which is the real
        // distinction MUI's lock/key icons communicate.
        snprintf(s_room_labels[i], sizeof(s_room_labels[i]), "%s %s",
                 s_room_names[i], i == 0 ? "(default)" : "(private)");
        s_room_label_ptrs[i] = s_room_labels[i];
#ifdef CONFIG_PURR_UI_BACKEND_CUPCAKE
        // No lock/key glyph exists in this LVGL build's symbol set (checked
        // lv_symbol_def.h) — WIFI for the public default channel (broadcast
        // metaphor) and EYE_CLOSE for a private/PSK room ("not visible
        // without the passphrase") is the closest available substitute for
        // MUI's own lock/key icons.
        s_room_icon_ptrs[i] = (i == 0) ? LV_SYMBOL_WIFI : LV_SYMBOL_EYE_CLOSE;
#endif
    }

    if (s_room_list) {
        purr_kernel_ui_breadcrumb("msn:room_list_set_items");
#ifdef CONFIG_PURR_UI_BACKEND_CUPCAKE
        cupcake_win_list_set_items_icon(s_room_list, s_room_label_ptrs, s_room_icon_ptrs, s_room_count);
#else
        purr_win_list_set_items(s_room_list, s_room_label_ptrs, s_room_count);
#endif
    }
}

// Always-present status line — this and the Refresh/Add Room row below it
// are the fix for the "blank window" bug: previously the buddy list was the
// *only* widget in the window, so 0 discovered peers meant a fully empty
// box with nothing to look at or tap.
static void refresh_status_label(void) {
    purr_kernel_ui_breadcrumb("msn:refresh_status");
    if (!s_status_lbl) return;
    if (!s_backend->ready()) {
        purr_win_label_set(s_status_lbl, "Mesh: starting...");
    } else if (!s_backend->is_alive()) {
        purr_win_label_set(s_status_lbl, "Mesh: not responding");
    } else {
        // Best-signal rollup across known nodes — MUI's own header shows a
        // single signal-strength summary rather than making the user open
        // the node list to gauge mesh health at a glance; this is the same
        // idea in one label. MSN_RSSI_UNKNOWN sorts last on purpose (never
        // wins "best") since a backend that can't report signal at all
        // (MeshCore, today) shouldn't make every node look terrible.
        int8_t best_rssi = MSN_RSSI_UNKNOWN;
        for (int i = 0; i < s_buddy_count; i++) {
            msn_contact_t info;
            if (!s_backend->contact_at(i, &info)) continue;
            if (info.rssi_dbm != MSN_RSSI_UNKNOWN &&
                (best_rssi == MSN_RSSI_UNKNOWN || info.rssi_dbm > best_rssi)) {
                best_rssi = info.rssi_dbm;
            }
        }
        char buf[80];
        snprintf(buf, sizeof(buf), "%s: ready (%d node%s)",
                 s_backend->name, s_buddy_count, s_buddy_count == 1 ? "" : "s");
        if (best_rssi != MSN_RSSI_UNKNOWN) {
            size_t len = strlen(buf);
            snprintf(buf + len, sizeof(buf) - len, "  %s", signal_bars(best_rssi));
        }
        // Home-base relay indicator (Meshtastic backend only, this pass —
        // see msn_backend.h's msn_mt_relay_is_active()) — makes the
        // automatic handoff visible without requiring the user to check
        // anywhere else. Refreshed every buddy_refresh_task() tick (10s),
        // same cadence as the rest of this label.
        if (s_backend == msn_backend_meshtastic() && msn_mt_relay_is_active()) {
            size_t len = strlen(buf);
            snprintf(buf + len, sizeof(buf) - len, "  [via home base]");
            // Contact count above already reflects the home base's own
            // node table at this point (backend_contact_count() switches
            // automatically — see msn_backend_meshtastic.c's Phase D2
            // additions), so no separate "remote view" wording is needed;
            // the node count itself is the fresh-slate signal.
        }
        purr_win_label_set(s_status_lbl, buf);
    }
}

// Defined further down (Manage section) — reused here for the remote-mode
// transition below, forward-declared to avoid reordering the file.
static void reset_all_buddy_windows(void);
static void reset_all_room_windows(void);

// True the last time we checked — compared against msn_mt_relay_is_active()
// each tick to detect the home-base connect/disconnect edge (Phase D2:
// "fresh slate" remote view). Meshtastic-only, matching msn_mt_relay_*()'s
// own scope this pass — MeshCore's chooser slot never flips this.
static bool s_was_remote = false;

static void check_remote_transition(void) {
    if (s_backend != msn_backend_meshtastic()) return;
    bool now_remote = msn_mt_relay_is_active();
    if (now_remote == s_was_remote) return;
    s_was_remote = now_remote;

    // Contact/channel table indices now mean something entirely different
    // (home base's own vs. this device's own) — any already-open chat/room
    // window is showing content keyed to a now-stale index. Same hazard
    // (and same fix) as on_forget_node()/on_forget_room()'s own comment.
    reset_all_buddy_windows();
    reset_all_room_windows();
    refresh_buddy_list();
    refresh_room_list();
    refresh_status_label();
}

static void buddy_refresh_task(void *arg) {
    (void)arg;
    while (s_running) {
        check_remote_transition();
        refresh_buddy_list();
        refresh_room_list();
        refresh_status_label();
        // Slept in short steps (not one 10s vTaskDelay) so a stop request
        // (s_running = false) is noticed within ~200ms, not up to 10s late.
        for (int waited_ms = 0; waited_ms < 10000 && s_running; waited_ms += 200) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    if (s_refresh_done) xSemaphoreGive(s_refresh_done);
    // Must match the WithCaps variant used to create this task (see its
    // xTaskCreateWithCaps() call site below).
    vTaskDeleteWithCaps(NULL);
}

// ── Chat log helper (same scroll-drop-half pattern as terminal.c) ───────────

static void log_append(char *buf, size_t *len, const char *text) {
    size_t tlen = strlen(text);
    if (*len + tlen >= CHAT_LOG_LEN - 1) {
        size_t half = CHAT_LOG_LEN / 2;
        memmove(buf, buf + half, *len - half);
        *len -= half;
        buf[*len] = '\0';
    }
    memcpy(buf + *len, text, tlen + 1);
    *len += tlen;
}

// ── Scrolling ────────────────────────────────────────────────────────────
// No backend exposes real textarea scrolling through the purr_win_* catcall
// layer — see the original meshchat.c's comment for the full rationale.
// Renders a scrolled-down "window" into the log by re-calling textarea_set()
// with the first N lines skipped, rather than adding new engine plumbing.
#define CHAT_SCROLL_STEP 4   // lines revealed per "v" button press

static int count_lines(const char *s) {
    int n = 1;
    for (; *s; s++) if (*s == '\n') n++;
    return n;
}

static const char *skip_lines(const char *text, int n) {
    const char *p = text;
    while (n > 0 && *p) {
        if (*p == '\n') n--;
        p++;
    }
    return p;
}

static void render_chat_view(int idx) {
    if (!s_chat_win[idx] || !s_chat_out[idx]) return;
    int max_scroll = count_lines(s_chat_logs[idx]) - 1;
    if (max_scroll < 0) max_scroll = 0;
    if (s_chat_scroll[idx] > max_scroll) s_chat_scroll[idx] = max_scroll;
    purr_win_textarea_set(s_chat_out[idx], skip_lines(s_chat_logs[idx], s_chat_scroll[idx]));
}

static void render_room_view(int idx) {
    if (!s_room_win[idx] || !s_room_out[idx] || !s_room_logs[idx]) return;
    int max_scroll = count_lines(s_room_logs[idx]) - 1;
    if (max_scroll < 0) max_scroll = 0;
    if (s_room_scroll[idx] > max_scroll) s_room_scroll[idx] = max_scroll;
    purr_win_textarea_set(s_room_out[idx], skip_lines(s_room_logs[idx], s_room_scroll[idx]));
}

static void on_chat_scroll_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e;
    int idx = (int)(intptr_t)user;
    if (idx < 0 || idx >= MAX_CHATS) return;
    s_chat_scroll[idx] += CHAT_SCROLL_STEP;
    render_chat_view(idx);
}

static void on_room_scroll_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e;
    int idx = (int)(intptr_t)user;
    if (idx < 0 || idx >= MAX_ROOMS) return;
    s_room_scroll[idx] += CHAT_SCROLL_STEP;
    render_room_view(idx);
}

static void chat_log_append(int idx, const char *text) {
    log_append(s_chat_logs[idx], &s_chat_loglen[idx], text);
    s_buddy_last_ms[idx] = (uint32_t)purr_kernel_uptime_ms();
    render_chat_view(idx);
    char path[64];
    dm_path(s_buddy_id_strs[idx], path, sizeof(path));
    append_to_sd(path, text);
}

static void room_log_append(int idx, const char *text) {
    if (!s_room_logs[idx]) return;
    log_append(s_room_logs[idx], &s_room_loglen[idx], text);
    s_room_last_ms[idx] = (uint32_t)purr_kernel_uptime_ms();
    render_room_view(idx);
    char path[64];
    room_path(idx, path, sizeof(path));
    append_to_sd(path, text);
}

// ── DM chat window ───────────────────────────────────────────────────────────

static void on_chat_send(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e;
    int idx = (int)(intptr_t)user;
    if (idx < 0 || idx >= s_buddy_count) return;
    const char *text = purr_win_textarea_get(s_chat_in[idx]);
    if (!text || !*text) return;

    char line[160];
    snprintf(line, sizeof(line), "> %s\n", text);
    chat_log_append(idx, line);

    s_backend->send_text(idx, -1, text);
    purr_win_textarea_clear(s_chat_in[idx]);
}

static void open_chat(int idx) {
    if (idx < 0 || idx >= s_buddy_count) return;
    purr_kernel_ui_breadcrumb("msn:open_chat");
    if (s_chat_win[idx]) {
        purr_win_show(s_chat_win[idx]);
        return;
    }

    // First time this conversation's been opened this boot — resume its
    // history from SD before building the window.
    if (s_chat_loglen[idx] == 0) {
        char path[64];
        dm_path(s_buddy_id_strs[idx], path, sizeof(path));
        load_tail_from_sd(path, s_chat_logs[idx], &s_chat_loglen[idx], CHAT_LOG_LEN);
    }

    purr_kernel_ui_breadcrumb("msn:open_chat:win_create");
    s_chat_win[idx] = purr_win_create(s_buddy_labels[idx]);
    s_chat_out[idx] = purr_win_textarea(s_chat_win[idx], 100, 75);
    purr_wid_t row = purr_win_row(s_chat_win[idx], 4);
    s_chat_in[idx]  = purr_win_textarea(s_chat_win[idx], 60, 10);
    purr_win_button(s_chat_win[idx], "Send", on_chat_send, (void *)(intptr_t)idx);
    purr_win_button(s_chat_win[idx], "v", on_chat_scroll_click, (void *)(intptr_t)idx);
    purr_win_layout_end(row);

    purr_kernel_ui_breadcrumb("msn:open_chat:render");
    render_chat_view(idx);
    purr_win_textarea_focus(s_chat_in[idx]);
    // win_show() first — see terminal.c's terminal_init() for why (Cupcake's
    // win_show() raises the window above whatever kb_show() just showed).
    purr_kernel_ui_breadcrumb("msn:open_chat:show");
    purr_win_show(s_chat_win[idx]);
    purr_kernel_ui_breadcrumb("msn:open_chat:kb_show");
    purr_win_keyboard_show(s_chat_win[idx], s_chat_in[idx]);
    purr_kernel_ui_breadcrumb("msn:open_chat:done");
}

static void on_buddy_list_event(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)user;
    purr_kernel_ui_breadcrumb("msn:on_buddy_list_event");
    if (e != PURR_EVENT_ACTIVATED) return;
    int idx = purr_win_list_get_selected(s_buddy_list);
    open_chat(idx);
}

// ── Room chat window ──────────────────────────────────────────────────────────

static void on_room_send(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e;
    int idx = (int)(intptr_t)user;
    if (idx < 0 || idx >= s_room_count) return;
    const char *text = purr_win_textarea_get(s_room_in[idx]);
    if (!text || !*text) return;

    char line[160];
    snprintf(line, sizeof(line), "> %s\n", text);
    room_log_append(idx, line);

    s_backend->send_text(-1, idx, text);
    purr_win_textarea_clear(s_room_in[idx]);
}

static void open_room(int idx) {
    if (idx < 0 || idx >= s_room_count) return;
    purr_kernel_ui_breadcrumb("msn:open_room");
    if (s_room_win[idx]) {
        purr_win_show(s_room_win[idx]);
        return;
    }
    if (!s_room_logs[idx]) {
        s_room_logs[idx] = heap_caps_malloc(CHAT_LOG_LEN, MALLOC_CAP_SPIRAM);
        if (s_room_logs[idx]) s_room_logs[idx][0] = '\0';
        else return;
    }
    // First time this room's been opened this boot — resume its history
    // from SD before building the window.
    if (s_room_loglen[idx] == 0) {
        char path[64];
        room_path(idx, path, sizeof(path));
        load_tail_from_sd(path, s_room_logs[idx], &s_room_loglen[idx], CHAT_LOG_LEN);
    }

    purr_kernel_ui_breadcrumb("msn:open_room:win_create");
    s_room_win[idx] = purr_win_create(s_room_names[idx]);
    s_room_out[idx] = purr_win_textarea(s_room_win[idx], 100, 75);
    purr_wid_t row = purr_win_row(s_room_win[idx], 4);
    s_room_in[idx]  = purr_win_textarea(s_room_win[idx], 60, 10);
    purr_win_button(s_room_win[idx], "Send", on_room_send, (void *)(intptr_t)idx);
    purr_win_button(s_room_win[idx], "v", on_room_scroll_click, (void *)(intptr_t)idx);
    purr_win_layout_end(row);

    purr_kernel_ui_breadcrumb("msn:open_room:render");
    render_room_view(idx);
    purr_win_textarea_focus(s_room_in[idx]);
    purr_kernel_ui_breadcrumb("msn:open_room:show");
    purr_win_show(s_room_win[idx]);
    purr_kernel_ui_breadcrumb("msn:open_room:kb_show");
    purr_win_keyboard_show(s_room_win[idx], s_room_in[idx]);
    purr_kernel_ui_breadcrumb("msn:open_room:done");
}

static void on_room_list_event(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)user;
    purr_kernel_ui_breadcrumb("msn:on_room_list_event");
    if (e != PURR_EVENT_ACTIVATED) return;
    int idx = purr_win_list_get_selected(s_room_list);
    open_room(idx);
}

// ── Add Room ──────────────────────────────────────────────────────────────────
// No QR/URL channel-sharing (out of scope) — the key is derived from
// whatever passphrase text the user types, via SHA-256 truncated to 16
// bytes, so two PURR devices typing the same passphrase land on the same
// key deterministically. For Meshtastic this won't match a real node's own
// custom-channel PSK (those are raw random bytes shared via QR/URL, not a
// passphrase) — interop there would need the exact same 16 raw bytes.

static void derive_psk_from_passphrase(const char *passphrase, uint8_t psk16[16]) {
    uint8_t hash[32];
    mbedtls_sha256((const unsigned char *)passphrase, strlen(passphrase), hash, 0);
    memcpy(psk16, hash, 16);
}

static void on_addroom_create(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    const char *name = purr_win_textarea_get(s_addroom_name_in);
    const char *pass = purr_win_textarea_get(s_addroom_psk_in);
    if (!name || !*name || !pass || !*pass) return;

    uint8_t psk16[16];
    derive_psk_from_passphrase(pass, psk16);
    int idx = s_backend->channel_add(name, psk16, sizeof(psk16));
    if (idx < 0) return;   // full, or hash collision — nothing to show for it yet

    refresh_room_list();
    purr_win_destroy(s_addroom_win);
    s_addroom_win = 0; s_addroom_name_in = 0; s_addroom_psk_in = 0;
    open_room(idx);
}

static void open_addroom(void) {
    if (s_addroom_win) {
        purr_win_show(s_addroom_win);
        return;
    }
    if (s_backend->channel_count() >= MAX_ROOMS) return;   // full

    s_addroom_win     = purr_win_create("Add Room");
    purr_win_label(s_addroom_win, "Room name:");
    s_addroom_name_in = purr_win_textarea(s_addroom_win, 90, 15);
    purr_win_label(s_addroom_win, "Passphrase (shared with whoever else joins):");
    s_addroom_psk_in  = purr_win_textarea(s_addroom_win, 90, 15);
    purr_win_button(s_addroom_win, "Create", on_addroom_create, NULL);

    purr_win_textarea_focus(s_addroom_name_in);
    purr_win_show(s_addroom_win);
    purr_win_keyboard_show(s_addroom_win, s_addroom_name_in);
}

static void on_addroom_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    open_addroom();
}

// ── Manage (forget nodes/rooms) ───────────────────────────────────────────────
// A forget action changes which buddy/room ends up at which table index —
// rather than try to shift the parallel s_chat_*/s_room_* window/log arrays
// to match, every open chat/room window just gets closed and its in-memory
// log cleared. Re-opening any of them reloads correctly (dm_path()/
// room_path() are keyed by id_str / channel hash, not index) — a small UX
// cost for a deliberate, infrequent action, in exchange for zero risk of a
// stale log silently appearing under the wrong buddy's name.

static void reset_all_buddy_windows(void) {
    for (int i = 0; i < MAX_CHATS; i++) {
        if (s_chat_win[i]) {
            purr_win_destroy(s_chat_win[i]);
            s_chat_win[i] = 0; s_chat_out[i] = 0; s_chat_in[i] = 0;
        }
        s_chat_logs[i][0] = '\0';
        s_chat_loglen[i]  = 0;
    }
}

static void reset_all_room_windows(void) {
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (s_room_win[i]) {
            purr_win_destroy(s_room_win[i]);
            s_room_win[i] = 0; s_room_out[i] = 0; s_room_in[i] = 0;
        }
        if (s_room_logs[i]) s_room_logs[i][0] = '\0';
        s_room_loglen[i] = 0;
    }
}

static void refresh_manage_lists(void) {
    if (s_manage_node_list) purr_win_list_set_items(s_manage_node_list, s_buddy_label_ptrs, s_buddy_count);
    if (s_manage_room_list) purr_win_list_set_items(s_manage_room_list, s_room_label_ptrs, s_room_count);
}

static void on_forget_node(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    int idx = purr_win_list_get_selected(s_manage_node_list);
    if (idx < 0 || idx >= s_buddy_count) return;

    char path[64];
    dm_path(s_buddy_id_strs[idx], path, sizeof(path));
    remove(path);
    s_backend->contact_forget(idx);
    reset_all_buddy_windows();
    refresh_buddy_list();
    refresh_manage_lists();
}

static void on_forget_room(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    int idx = purr_win_list_get_selected(s_manage_room_list);
    if (idx < 0 || idx >= s_room_count) return;

    char path[64];
    room_path(idx, path, sizeof(path));
    remove(path);
    s_backend->channel_remove(idx);
    reset_all_room_windows();
    refresh_room_list();
    refresh_manage_lists();
}

static void open_manage(void) {
    if (s_manage_win) {
        purr_win_show(s_manage_win);
        refresh_manage_lists();
        return;
    }

    s_manage_win = purr_win_create("Manage");
    purr_win_label(s_manage_win, "Known Nodes");
    s_manage_node_list = purr_win_list(s_manage_win, 100, 35);
    purr_win_button(s_manage_win, "Forget Node", on_forget_node, NULL);

    purr_win_label(s_manage_win, "Rooms");
    s_manage_room_list = purr_win_list(s_manage_win, 100, 35);
    purr_win_button(s_manage_win, "Forget Room", on_forget_room, NULL);

    refresh_manage_lists();
    purr_win_show(s_manage_win);
}

// ── Incoming messages ─────────────────────────────────────────────────────────
// Registers with the active backend's single unified RX-callback slot —
// both mesh modules already fire purr_kernel_notify() for every text
// message regardless (unchanged), so a message for a buddy/room whose
// window isn't open right now still surfaces as a notification banner.

static void on_mesh_rx(int contact_idx, int channel_idx, const char *text) {
    if (channel_idx >= 0) {
        if (channel_idx >= s_room_count) return;
        char line[280];
        snprintf(line, sizeof(line), "%s: %s\n", s_room_names[channel_idx], text);
        room_log_append(channel_idx, line);
        return;
    }

    if (contact_idx < 0 || contact_idx >= s_buddy_count) return;
    char line[280];
    snprintf(line, sizeof(line), "%s: %s\n", s_buddy_labels[contact_idx], text);
    chat_log_append(contact_idx, line);
}

// ── Nodes / Channels / Messages screens ─────────────────────────────────────
// Lazy-create-then-show/hide, same shape as open_chat()/open_room() below
// and settings.c's own category-sub-window pattern — each screen persists
// once built until close_chat_ui() tears everything down; Back just hides.

static void on_nodes_back_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    purr_win_hide(s_nodes_win);
}

static void open_nodes_screen(void) {
    if (s_nodes_win) { purr_win_show(s_nodes_win); return; }
    s_nodes_win = purr_win_create("Nodes");
    purr_win_button(s_nodes_win, "< Back", on_nodes_back_click, NULL);
    s_buddy_list = purr_win_list(s_nodes_win, 100, 85);
    purr_win_list_on_select(s_buddy_list, on_buddy_list_event, NULL);
    refresh_buddy_list();
    purr_win_show(s_nodes_win);
}

static void on_channels_back_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    purr_win_hide(s_channels_win);
}

static void open_channels_screen(void) {
    if (s_channels_win) { purr_win_show(s_channels_win); return; }
    s_channels_win = purr_win_create("Channels");
    purr_wid_t row = purr_win_row(s_channels_win, 4);
    purr_win_button(s_channels_win, "< Back", on_channels_back_click, NULL);
    purr_win_button(s_channels_win, "Add Room", on_addroom_click, NULL);
    purr_win_layout_end(row);
    s_room_list = purr_win_list(s_channels_win, 100, 80);
    purr_win_list_on_select(s_room_list, on_room_list_event, NULL);
    refresh_room_list();
    purr_win_show(s_channels_win);
}

// Recent-activity inbox — most-recent-first across every DM/room touched
// since MSN was last (re)launched (s_buddy_last_ms/s_room_last_ms reset in
// close_chat_ui()). No new message storage: the preview text is read on
// demand from the tail of the existing s_chat_logs/s_room_logs buffer.
#define MAX_INBOX (MAX_CHATS + MAX_ROOMS)
typedef struct { bool is_room; int idx; uint32_t last_ms; } inbox_entry_t;
static inbox_entry_t s_inbox_entries[MAX_INBOX];
static int           s_inbox_count = 0;
static char          s_inbox_row_bufs[MAX_INBOX][100];
static const char    *s_inbox_row_ptrs[MAX_INBOX];

// Extracts the last non-empty line from a log buffer (each entry already
// ends in '\n' — see chat_log_append()/room_log_append()) into out.
static void last_line(const char *log, char *out, size_t out_max) {
    out[0] = 0;
    if (!log || !*log) return;
    size_t end = strlen(log);
    while (end > 0 && log[end - 1] == '\n') end--;
    size_t start = end;
    while (start > 0 && log[start - 1] != '\n') start--;
    size_t n = end - start;
    if (n >= out_max) n = out_max - 1;
    memcpy(out, log + start, n);
    out[n] = 0;
}

static int inbox_cmp(const void *a, const void *b) {
    uint32_t ma = ((const inbox_entry_t *)a)->last_ms;
    uint32_t mb = ((const inbox_entry_t *)b)->last_ms;
    if (mb > ma) return 1;
    if (mb < ma) return -1;
    return 0;
}

static void refresh_messages_list(void) {
    s_inbox_count = 0;
    for (int i = 0; i < s_buddy_count && s_inbox_count < MAX_INBOX; i++) {
        if (s_buddy_last_ms[i] == 0) continue;
        s_inbox_entries[s_inbox_count++] = (inbox_entry_t){ false, i, s_buddy_last_ms[i] };
    }
    for (int i = 0; i < s_room_count && s_inbox_count < MAX_INBOX; i++) {
        if (s_room_last_ms[i] == 0) continue;
        s_inbox_entries[s_inbox_count++] = (inbox_entry_t){ true, i, s_room_last_ms[i] };
    }
    qsort(s_inbox_entries, (size_t)s_inbox_count, sizeof(inbox_entry_t), inbox_cmp);

    for (int i = 0; i < s_inbox_count; i++) {
        const inbox_entry_t *e = &s_inbox_entries[i];
        char preview[64];
        last_line(e->is_room ? s_room_logs[e->idx] : s_chat_logs[e->idx], preview, sizeof(preview));
        snprintf(s_inbox_row_bufs[i], sizeof(s_inbox_row_bufs[i]), "%s: %s",
                 e->is_room ? s_room_names[e->idx] : s_buddy_labels[e->idx], preview);
        s_inbox_row_ptrs[i] = s_inbox_row_bufs[i];
    }
    if (s_messages_list) purr_win_list_set_items(s_messages_list, s_inbox_row_ptrs, s_inbox_count);
}

static void on_messages_list_event(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)user;
    if (e != PURR_EVENT_ACTIVATED) return;
    int idx = purr_win_list_get_selected(s_messages_list);
    if (idx < 0 || idx >= s_inbox_count) return;
    const inbox_entry_t *entry = &s_inbox_entries[idx];
    if (entry->is_room) open_room(entry->idx);
    else                open_chat(entry->idx);
}

static void on_messages_back_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    purr_win_hide(s_messages_win);
}

static void open_messages_screen(void) {
    if (s_messages_win) { purr_win_show(s_messages_win); refresh_messages_list(); return; }
    s_messages_win = purr_win_create("Messages");
    purr_win_button(s_messages_win, "< Back", on_messages_back_click, NULL);
    s_messages_list = purr_win_list(s_messages_win, 100, 85);
    purr_win_list_on_select(s_messages_list, on_messages_list_event, NULL);
    refresh_messages_list();
    purr_win_show(s_messages_win);
}

// ── Home screen (tile grid, falls back to a plain list) ─────────────────────

static void on_nodes_tile(purr_wid_t w, purr_event_t e, void *user)    { (void)w; (void)e; (void)user; open_nodes_screen(); }
static void on_messages_tile(purr_wid_t w, purr_event_t e, void *user) { (void)w; (void)e; (void)user; open_messages_screen(); }
static void on_channels_tile(purr_wid_t w, purr_event_t e, void *user) { (void)w; (void)e; (void)user; open_channels_screen(); }
static void on_manage_tile(purr_wid_t w, purr_event_t e, void *user)   { (void)w; (void)e; (void)user; open_manage(); }

static const char *s_home_labels[4] = { "Nodes", "Messages", "Channels", "Manage" };

// Fallback path only — the tile grid path dispatches per-tile via cbs[]
// below and never reaches this (PURR_EVENT_CLICKED per tile, not a shared
// list-selection callback).
static void on_home_list_event(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)user;
    if (e != PURR_EVENT_ACTIVATED) return;
    switch (purr_win_list_get_selected(s_home_list)) {
        case 0: open_nodes_screen(); break;
        case 1: open_messages_screen(); break;
        case 2: open_channels_screen(); break;
        case 3: open_manage(); break;
        default: break;
    }
}

static void build_home_screen_nav(void) {
    s_home_tile_grid = purr_win_tile_grid(s_home_win, 100, 75);
    if (s_home_tile_grid) {
#ifdef CONFIG_PURR_UI_BACKEND_CUPCAKE
        static const char *symbols[4] = { LV_SYMBOL_LIST, LV_SYMBOL_ENVELOPE, LV_SYMBOL_WIFI, LV_SYMBOL_TRASH };
#else
        // Backend implements tile_grid but isn't Cupcake (a future LVGL-
        // based backend) — no LV_SYMBOL_* available at compile time here
        // (cupcake.h/lvgl.h are only pulled in under the Cupcake ifdef,
        // see this file's top-of-file include comment), so tiles render
        // label-only. Functionally complete either way.
        static const char *symbols[4] = { NULL, NULL, NULL, NULL };
#endif
        // MSN's own accent — a consistent blue, distinct from the App
        // Drawer's per-app-name hash-tinted rainbow (see catcall_ui.h's
        // own comment on why tile colors are caller-supplied here).
        static const uint32_t colors[4] = { 0x2E6F9E, 0x2E6F9E, 0x2E6F9E, 0x2E6F9E };
        static purr_win_cb_t cbs[4] = { on_nodes_tile, on_messages_tile, on_channels_tile, on_manage_tile };
        static void *users[4] = { NULL, NULL, NULL, NULL };
        purr_win_tile_grid_set_items(s_home_tile_grid, s_home_labels, symbols, colors, cbs, users, 4);
    } else {
        // Active backend doesn't implement the tile grid (e.g. MiniWin,
        // until/unless it's added there) — plain list of the same 4
        // destinations, fully functional, just less decorated.
        s_home_list = purr_win_list(s_home_win, 100, 75);
        purr_win_list_set_items(s_home_list, s_home_labels, 4);
        purr_win_list_on_select(s_home_list, on_home_list_event, NULL);
    }
}

// ── Chat UI lifecycle (Home screen + its background task) ──────────────────

static void open_msn_home(void) {
    // Reused across relaunches like s_room_logs below — starts "empty"
    // (taken), which is exactly the state close_chat_ui()'s xSemaphoreTake()
    // below needs at the start of every run.
    if (!s_refresh_done) s_refresh_done = xSemaphoreCreateBinary();

    s_home_win   = purr_win_create("MSN");
    s_status_lbl = purr_win_label(s_home_win, "Mesh: starting...");
    build_home_screen_nav();

    refresh_buddy_list();
    refresh_room_list();
    refresh_status_label();
    purr_win_show(s_home_win);

    s_backend->add_rx_cb(on_mesh_rx);

    s_running = true;
    // No NVS/flash/SD access anywhere in this task's own body — safe on a
    // PSRAM-backed stack (see app_manager.c's launch_native()/launch_meow()
    // for the same pattern).
    xTaskCreateWithCaps(buddy_refresh_task, "msn_ref", 4096, NULL, 3, &s_refresh_task, MALLOC_CAP_SPIRAM);
}

static void close_chat_ui(void) {
    s_running = false;
    // Wait for buddy_refresh_task() to actually notice s_running == false
    // and exit before touching any window/widget below — see
    // s_refresh_done's declaration comment for the use-after-free this
    // closes.
    if (s_refresh_done) xSemaphoreTake(s_refresh_done, pdMS_TO_TICKS(2000));
    s_refresh_task = NULL;

    if (s_backend) s_backend->remove_rx_cb(on_mesh_rx);

    for (int i = 0; i < MAX_CHATS; i++) {
        if (s_chat_win[i]) {
            purr_win_destroy(s_chat_win[i]);
            s_chat_win[i] = 0; s_chat_out[i] = 0; s_chat_in[i] = 0;
        }
        s_buddy_last_ms[i] = 0;   // Messages inbox starts empty again next launch
    }
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (s_room_win[i]) {
            purr_win_destroy(s_room_win[i]);
            s_room_win[i] = 0; s_room_out[i] = 0; s_room_in[i] = 0;
        }
        s_room_last_ms[i] = 0;
    }
    if (s_addroom_win) {
        purr_win_destroy(s_addroom_win);
        s_addroom_win = 0; s_addroom_name_in = 0; s_addroom_psk_in = 0;
    }
    if (s_manage_win) {
        purr_win_destroy(s_manage_win);
        s_manage_win = 0; s_manage_node_list = 0; s_manage_room_list = 0;
    }
    if (s_nodes_win)    { purr_win_destroy(s_nodes_win);    s_nodes_win = 0; }
    if (s_channels_win) { purr_win_destroy(s_channels_win); s_channels_win = 0; }
    if (s_messages_win) { purr_win_destroy(s_messages_win); s_messages_win = 0; s_messages_list = 0; }
    if (s_home_win) purr_win_destroy(s_home_win);
    s_home_win = 0; s_home_tile_grid = 0; s_home_list = 0; s_status_lbl = 0;
    s_buddy_list = 0; s_room_list = 0;
    // s_chat_logs/s_chat_loglen/s_room_logs/s_room_loglen deliberately NOT
    // cleared — chat history persists across closing and relaunching the app.
}

// ── Chooser screen ───────────────────────────────────────────────────────

static void update_chooser_status(void) {
    bool mt_active = purr_kernel_get_module("meshtastic") != NULL;
    bool mc_active = purr_kernel_get_module("meshcore") != NULL;
    if (s_meshtastic_status) purr_win_label_set(s_meshtastic_status, mt_active ? "* active" : "");
    if (s_meshcore_status)   purr_win_label_set(s_meshcore_status,   mc_active ? "* active" : "");
    if (s_force_local_status) {
        purr_win_label_set(s_force_local_status, msn_mt_relay_get_force_local() ? "* ON" : "");
    }
}

static void close_chooser(void) {
    if (s_chooser_win) {
        purr_win_destroy(s_chooser_win);
        s_chooser_win = 0; s_meshtastic_status = 0; s_meshcore_status = 0;
        s_force_local_status = 0;
    }
}

// Manual override for the automatic home-base relay (msn_backend_
// meshtastic.c) — runtime-only, resets to automatic (OFF) on reboot.
static void on_force_local_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    msn_mt_relay_set_force_local(!msn_mt_relay_get_force_local());
    update_chooser_status();
}

static void enter_chat_ui(const msn_backend_t *backend) {
    s_backend = backend;
    close_chooser();
    open_msn_home();
}

static void do_switch(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    purr_mesh_backend_t target = s_pending_switch_target;
    int rc = purr_kernel_mesh_backend_switch(target);

    if (s_switch_confirm_win) {
        purr_win_destroy(s_switch_confirm_win);
        s_switch_confirm_win = 0; s_switch_confirm_lbl = 0;
    }

    if (rc != PURR_MODCTL_OK && rc != PURR_MODCTL_ERR_ALREADY) {
        update_chooser_status();   // switch failed — stay on the chooser, reflect real state
        return;
    }
    enter_chat_ui(target == PURR_MESH_BACKEND_MESHCORE ? msn_backend_meshcore() : msn_backend_meshtastic());
}

static void on_switch_cancel(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    if (s_switch_confirm_win) {
        purr_win_destroy(s_switch_confirm_win);
        s_switch_confirm_win = 0; s_switch_confirm_lbl = 0;
    }
}

static void open_switch_confirm(purr_mesh_backend_t target, const char *name) {
    s_pending_switch_target = target;
    if (s_switch_confirm_win) {
        purr_win_show(s_switch_confirm_win);
        return;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "Switch to %s?", name);

    s_switch_confirm_win = purr_win_create("Switch Backend");
    s_switch_confirm_lbl = purr_win_label(s_switch_confirm_win, msg);
    purr_wid_t row = purr_win_row(s_switch_confirm_win, 4);
    purr_win_button(s_switch_confirm_win, "Switch", do_switch, NULL);
    purr_win_button(s_switch_confirm_win, "Cancel", on_switch_cancel, NULL);
    purr_win_layout_end(row);

    purr_win_show(s_switch_confirm_win);
}

static void on_meshtastic_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    if (purr_kernel_get_module("meshtastic")) {
        enter_chat_ui(msn_backend_meshtastic());
    } else {
        open_switch_confirm(PURR_MESH_BACKEND_MESHTASTIC, "Meshtastic");
    }
}

static void on_meshcore_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    if (purr_kernel_get_module("meshcore")) {
        enter_chat_ui(msn_backend_meshcore());
    } else {
        open_switch_confirm(PURR_MESH_BACKEND_MESHCORE, "MeshCore");
    }
}

static void open_chooser(void) {
    s_chooser_win = purr_win_create("MSN");
    purr_win_label(s_chooser_win, "Choose a mesh backend:");

    purr_wid_t mt_row = purr_win_row(s_chooser_win, 4);
    purr_win_button(s_chooser_win, "Meshtastic", on_meshtastic_click, NULL);
    s_meshtastic_status = purr_win_label(s_chooser_win, "");
    purr_win_layout_end(mt_row);

    purr_wid_t mc_row = purr_win_row(s_chooser_win, 4);
    purr_win_button(s_chooser_win, "MeshCore", on_meshcore_click, NULL);
    s_meshcore_status = purr_win_label(s_chooser_win, "");
    purr_win_layout_end(mc_row);

    // Manual override for MSN's automatic home-base radio relay (Meshtastic
    // backend only, this pass) — see msn_backend.h's msn_mt_relay_* comment.
    purr_wid_t fl_row = purr_win_row(s_chooser_win, 4);
    purr_win_button(s_chooser_win, "Force local radio", on_force_local_click, NULL);
    s_force_local_status = purr_win_label(s_chooser_win, "");
    purr_win_layout_end(fl_row);

    update_chooser_status();
    purr_win_show(s_chooser_win);
}

// ── App lifecycle ─────────────────────────────────────────────────────────────

static int msn_init(void) {
    s_backend = NULL;
    open_chooser();
    return 0;
}

static void msn_deinit(void) {
    close_chat_ui();
    close_chooser();
    if (s_switch_confirm_win) {
        purr_win_destroy(s_switch_confirm_win);
        s_switch_confirm_win = 0; s_switch_confirm_lbl = 0;
    }
    s_backend = NULL;
}

// ── Module header ─────────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(msn) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_APP,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "msn",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = msn_init,
    .deinit            = msn_deinit,
};
