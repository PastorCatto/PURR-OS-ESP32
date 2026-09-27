// reticulum_app.c — basic chat over the vendored Reticulum Network Stack
// (source/modules/reticulum). Built entirely on the portable purr_win_*
// API (source/kernel/catcalls/purr_win.h) — no raw LVGL, no per-UI-style
// code — so it works unmodified under whichever purr_win backend a given
// device is actually running (Cheetah/Cupcake/Mochi/Tabby/...), same as
// every other system app in this tree.
//
// Talks to the reticulum module only through reticulum_api.h's plain-C
// bridge — this file never touches the vendored C++ stack directly. One
// active chat link at a time (that module's own deliberately minimal
// scope for now, see its own top comment) — this app is the UI for
// exactly that, not a multi-conversation chat client.

#include "../../../kernel/core/purr_module.h"
#include "../../../kernel/core/purr_kernel.h"
#include "../../../kernel/catcalls/purr_win.h"
#include "reticulum_api.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "reticulum_app";

#define MAX_PEERS_SHOWN 16
#define REFRESH_MS 500

static purr_win_t s_win = 0;
static purr_wid_t s_own_hash_lbl  = 0;
static purr_wid_t s_peer_list     = 0;
static purr_wid_t s_link_status_lbl = 0;
static purr_wid_t s_chat_log      = 0;
static purr_wid_t s_compose_input = 0;

static char  s_peer_names[MAX_PEERS_SHOWN][RETICULUM_PEER_NAME_MAX + RETICULUM_HASH_HEX_LEN + 4];
static const char *s_peer_name_ptrs[MAX_PEERS_SHOWN];
static char  s_peer_hashes[MAX_PEERS_SHOWN][RETICULUM_HASH_HEX_LEN];
static int   s_peer_count = 0;

static reticulum_link_status_t s_last_link_status = RETICULUM_LINK_NONE;

static TaskHandle_t   s_refresh_task = NULL;
static volatile bool  s_running = false;
static SemaphoreHandle_t s_refresh_done = NULL;

// ── Peer list ────────────────────────────────────────────────────────────

static void refresh_peer_list(void) {
    if (!s_peer_list) return;

    int count = reticulum_peer_count();
    if (count > MAX_PEERS_SHOWN) count = MAX_PEERS_SHOWN;
    s_peer_count = count;

    for (int i = 0; i < count; i++) {
        char hash[RETICULUM_HASH_HEX_LEN];
        char name[RETICULUM_PEER_NAME_MAX];
        if (!reticulum_peer_at(i, hash, sizeof(hash), name, sizeof(name))) {
            s_peer_hashes[i][0] = 0;
            snprintf(s_peer_names[i], sizeof(s_peer_names[i]), "(gone)");
            continue;
        }
        snprintf(s_peer_hashes[i], sizeof(s_peer_hashes[i]), "%s", hash);
        // Short hash suffix alongside the name — same "name (id)" shape
        // milkbar's own paired-device list already uses, so a repeated
        // hostname (two peers both saying "PurrOS-XXXX") is still
        // distinguishable in the list. Deliberate truncation to 8 hex
        // chars — memcpy(), not snprintf("%s", ...), since the intent
        // here really is "cut it short", not a genuinely-bounded case
        // GCC just can't prove (-Werror=format-truncation correctly
        // flags the snprintf form; this sidesteps the diagnostic by not
        // giving it a variable-length %s to worry about at all).
        char short_hash[9];
        memcpy(short_hash, hash, 8);
        short_hash[8] = 0;
        snprintf(s_peer_names[i], sizeof(s_peer_names[i]), "%s (%s)", name, short_hash);
        s_peer_name_ptrs[i] = s_peer_names[i];
    }
    purr_win_list_set_items(s_peer_list, s_peer_name_ptrs, count);
}

// ── Link status ──────────────────────────────────────────────────────────

static const char *link_status_text(reticulum_link_status_t st) {
    switch (st) {
        case RETICULUM_LINK_PENDING:   return "Link: connecting...";
        case RETICULUM_LINK_HANDSHAKE: return "Link: handshaking...";
        case RETICULUM_LINK_ACTIVE:    return reticulum_link_is_incoming() ?
                                               "Link: active (incoming)" : "Link: active (outgoing)";
        case RETICULUM_LINK_STALE:     return "Link: stale";
        case RETICULUM_LINK_NONE:
        default:                       return "Link: none";
    }
}

static void refresh_link_status(void) {
    if (!s_link_status_lbl) return;
    reticulum_link_status_t st = reticulum_link_status();
    if (st == s_last_link_status) return;
    s_last_link_status = st;
    purr_win_label_set(s_link_status_lbl, link_status_text(st));
    if (st == RETICULUM_LINK_ACTIVE && s_chat_log) {
        purr_win_textarea_append(s_chat_log, "-- connected --");
    } else if (st == RETICULUM_LINK_NONE && s_chat_log) {
        purr_win_textarea_append(s_chat_log, "-- disconnected --");
    }
}

// ── Background refresh — peer list, link status, incoming messages ───────
// Same "short vTaskDelay steps, checked against s_running" shape milkbar's
// own refresh_task() uses, so app close (s_running = false) doesn't stall
// waiting out a long sleep. purr_win_* setters are safe to call from here
// directly (the backend defers internally) — same established convention
// this whole codebase already relies on everywhere else.

static void refresh_task(void *arg) {
    (void)arg;
    uint32_t since_peer_refresh_ms = 0;
    while (s_running) {
        refresh_link_status();

        char msg[RETICULUM_HASH_HEX_LEN + 256];
        while (reticulum_chat_poll(msg, sizeof(msg))) {
            if (s_chat_log) {
                char line[300];
                snprintf(line, sizeof(line), "them: %s", msg);
                purr_win_textarea_append(s_chat_log, line);
            }
        }

        since_peer_refresh_ms += REFRESH_MS;
        if (since_peer_refresh_ms >= 2000) {
            since_peer_refresh_ms = 0;
            refresh_peer_list();
        }

        for (int waited_ms = 0; waited_ms < REFRESH_MS && s_running; waited_ms += 100) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (s_refresh_done) xSemaphoreGive(s_refresh_done);
    vTaskDelete(NULL);
}

// ── UI callbacks ─────────────────────────────────────────────────────────

static void on_connect_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    int idx = purr_win_list_get_selected(s_peer_list);
    if (idx < 0 || idx >= s_peer_count) return;
    if (!reticulum_link_open(s_peer_hashes[idx])) {
        if (s_chat_log) purr_win_textarea_append(s_chat_log, "-- could not start link (no announce heard yet?) --");
        return;
    }
    s_last_link_status = RETICULUM_LINK_NONE;   // force refresh_link_status() to re-post
}

static void on_disconnect_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    reticulum_link_close();
}

static void on_send_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    const char *text = s_compose_input ? purr_win_textarea_get(s_compose_input) : NULL;
    if (!text || !text[0]) return;
    if (!reticulum_chat_send(text)) {
        if (s_chat_log) purr_win_textarea_append(s_chat_log, "-- send failed (link not active) --");
        return;
    }
    if (s_chat_log) {
        char line[300];
        snprintf(line, sizeof(line), "me: %s", text);
        purr_win_textarea_append(s_chat_log, line);
    }
    purr_win_textarea_clear(s_compose_input);
}

static void on_refresh_peers_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    refresh_peer_list();
}

// ── Module lifecycle ──────────────────────────────────────────────────────

static int reticulum_app_init(void) {
    s_win = purr_win_create("Reticulum Chat");

    char own_hash[RETICULUM_HASH_HEX_LEN];
    reticulum_own_hash(own_hash, sizeof(own_hash));
    char own_lbl[48];
    snprintf(own_lbl, sizeof(own_lbl), "Me: %s", own_hash[0] ? own_hash : "(starting...)");
    s_own_hash_lbl = purr_win_label(s_win, own_lbl);

    purr_win_label(s_win, "Peers heard:");
    s_peer_list = purr_win_list(s_win, 100, 25);

    purr_wid_t pr = purr_win_row(s_win, 3);
    purr_win_button(s_win, "Refresh", on_refresh_peers_click, NULL);
    purr_win_button(s_win, "Connect", on_connect_click,       NULL);
    purr_win_button(s_win, "Disconnect", on_disconnect_click, NULL);
    purr_win_layout_end(pr);

    s_link_status_lbl = purr_win_label(s_win, "Link: none");

    purr_win_label(s_win, "Chat:");
    s_chat_log = purr_win_textarea(s_win, 100, 30);

    s_compose_input = purr_win_textarea(s_win, 100, 15);
    purr_wid_t sr = purr_win_row(s_win, 1);
    purr_win_button(s_win, "Send", on_send_click, NULL);
    purr_win_layout_end(sr);

    purr_win_show(s_win);

    s_last_link_status = RETICULUM_LINK_NONE;
    s_running = true;
    if (!s_refresh_done) s_refresh_done = xSemaphoreCreateBinary();
    BaseType_t ok = xTaskCreate(refresh_task, "reticulum_app", 4096, NULL, 2, &s_refresh_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create refresh task");
    }

    return 0;
}

static void reticulum_app_deinit(void) {
    s_running = false;
    if (s_refresh_done) xSemaphoreTake(s_refresh_done, pdMS_TO_TICKS(2000));
    s_refresh_task = NULL;

    if (s_win) {
        purr_win_destroy(s_win);
        s_win = 0;
        s_own_hash_lbl = 0;
        s_peer_list = 0;
        s_link_status_lbl = 0;
        s_chat_log = 0;
        s_compose_input = 0;
    }
    s_peer_count = 0;
}

// ── Module header ─────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(reticulum_app) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_APP,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "reticulum_app",
    .version           = "0.1.0",
    .kernel_min        = "0.11.1",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = reticulum_app_init,
    .deinit            = reticulum_app_deinit,
};
