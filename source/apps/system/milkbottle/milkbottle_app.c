// milkbottle_app.c — PURR OS Milk Bottle: minimal two-way ESP-NOW text
// exchange + live connect status between paired devices, over
// proximity_rpc directly (no LoRa mesh involved at all) — a demo/test app
// for the pairing + proximity_rpc stack, independent of MSN/Milkbar.
//
// "Connected" means the selected paired device currently appears in
// proximity_device_at() — same live-presence check homebase.c uses,
// applied here to whichever device is selected rather than a fixed home
// base. Sending dispatches to a dedicated background task (proximity_rpc_
// call() must never run on cupcake_task, same rule as Milkbar/MSN's own
// relay code). Receiving only works while this app is open on the far end
// — MILKBOTTLE_ACTION_SEND_MSG is registered/unregistered from this app's
// own init()/deinit(), not a separate always-on module like msn_relay —
// matches this app's "demo, not a real messaging service" scope.

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

#define MILKBOTTLE_ACTION_SEND_MSG 0x3000
#define MILKBOTTLE_MAX_TEXT 64
#define REFRESH_MS       1000
#define RPC_TIMEOUT_MS   2500

static purr_win_t s_win         = 0;
static purr_wid_t s_device_list = 0;
static purr_wid_t s_status_lbl  = 0;
static purr_wid_t s_big_lbl     = 0;
static purr_wid_t s_input       = 0;

static uint8_t       s_selected_mac[6];
static volatile bool s_have_selection = false;

static char        s_device_row_bufs[PAIRING_MAX_DEVICES][32];
static const char *s_device_row_ptrs[PAIRING_MAX_DEVICES];
static int          s_device_count = 0;

// Last received message — written from proximity_rpc's own dispatch
// context (handle_send_msg(), not cupcake_task), read/rendered by
// refresh_task(). Plain flag-guarded copy, same "background writes, one
// UI-owning task reads" shape as homebase.c's s_present — good enough for
// a demo app; worst case on a torn read is one stale display frame, never
// a crash (single writer, single reader, no free()/realloc() involved).
static char           s_last_rx_text[MILKBOTTLE_MAX_TEXT + 1];
static volatile bool  s_rx_is_new = false;

static TaskHandle_t s_refresh_task = NULL;
static bool         s_running      = false;
static SemaphoreHandle_t s_refresh_done = NULL;

static bool handle_send_msg(const uint8_t mac[6], uint16_t action_id,
                             const uint8_t *req, size_t req_len,
                             uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out) {
    (void)mac; (void)action_id; (void)resp_out; (void)resp_cap;
    *resp_len_out = 0;
    if (req_len == 0 || req_len > MILKBOTTLE_MAX_TEXT) return false;
    memcpy(s_last_rx_text, req, req_len);
    s_last_rx_text[req_len] = 0;
    s_rx_is_new = true;
    return true;
}

static bool device_is_connected(const uint8_t mac[6]) {
    int n = proximity_device_count();
    for (int i = 0; i < n; i++) {
        proximity_device_t d;
        if (proximity_device_at(i, &d) && memcmp(d.mac, mac, 6) == 0) return true;
    }
    return false;
}

static void refresh_device_list(void) {
    int n = pairing_device_count();
    if (n > PAIRING_MAX_DEVICES) n = PAIRING_MAX_DEVICES;
    for (int i = 0; i < n; i++) {
        paired_device_t pd;
        if (!pairing_device_at(i, &pd)) { n = i; break; }
        bool connected = device_is_connected(pd.mac);
        snprintf(s_device_row_bufs[i], sizeof(s_device_row_bufs[i]), "%s%s",
                 pd.name, connected ? "  [connected]" : "");
        s_device_row_ptrs[i] = s_device_row_bufs[i];
    }
    s_device_count = n;
    if (s_device_list) purr_win_list_set_items(s_device_list, s_device_row_ptrs, s_device_count);
}

static void update_status(void) {
    if (!s_status_lbl) return;
    if (!s_have_selection) { purr_win_label_set(s_status_lbl, "Select a paired device"); return; }
    purr_win_label_set(s_status_lbl, device_is_connected(s_selected_mac) ? "Connected" : "Not connected");
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
    update_status();
}

typedef struct {
    uint8_t mac[6];
    char    text[MILKBOTTLE_MAX_TEXT + 1];
} send_ctx_t;

static void send_task(void *arg) {
    send_ctx_t *ctx = (send_ctx_t *)arg;
    uint8_t resp[4]; size_t resp_len = 0;
    // Fire-and-forget from the UI's perspective — result isn't surfaced
    // (same "optimistic, no confirmation" precedent as MSN's own send
    // button); this is a demo app, not a delivery-guaranteed one.
    proximity_rpc_call(ctx->mac, MILKBOTTLE_ACTION_SEND_MSG,
                        (const uint8_t *)ctx->text, strlen(ctx->text),
                        resp, sizeof(resp), &resp_len, RPC_TIMEOUT_MS);
    free(ctx);
    vTaskDeleteWithCaps(NULL);
}

static void on_send_click(purr_wid_t w, purr_event_t e, void *user) {
    (void)w; (void)e; (void)user;
    if (!s_have_selection) return;
    const char *text = purr_win_textarea_get(s_input);
    if (!text || !*text) return;

    send_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) return;
    memcpy(ctx->mac, s_selected_mac, 6);
    size_t n = strlen(text);
    if (n > MILKBOTTLE_MAX_TEXT) n = MILKBOTTLE_MAX_TEXT;
    memcpy(ctx->text, text, n);
    ctx->text[n] = 0;

    // proximity_rpc_call() must never run on cupcake_task — dedicated
    // background task per send, same rule as Milkbar/MSN's relay code.
    TaskHandle_t task = NULL;
    BaseType_t ok = xTaskCreateWithCaps(send_task, "milkbottle_tx", 4096, ctx, 3, &task, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) free(ctx);

    purr_win_textarea_clear(s_input);
}

static void refresh_task(void *arg) {
    (void)arg;
    while (s_running) {
        refresh_device_list();
        update_status();
        if (s_rx_is_new) {
            s_rx_is_new = false;
            if (s_big_lbl) purr_win_label_set_big(s_big_lbl, s_last_rx_text);
        }
        // Short steps, not one REFRESH_MS vTaskDelay — milkbottle_app_
        // deinit() blocks on this task actually exiting, same reasoning as
        // every other refresh_task() in this codebase.
        for (int waited_ms = 0; waited_ms < REFRESH_MS && s_running; waited_ms += 200) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    if (s_refresh_done) xSemaphoreGive(s_refresh_done);
    vTaskDeleteWithCaps(NULL);
}

static int milkbottle_app_init(void) {
    if (!s_refresh_done) s_refresh_done = xSemaphoreCreateBinary();

    proximity_rpc_register(MILKBOTTLE_ACTION_SEND_MSG, handle_send_msg);

    s_win = purr_win_create("Milk Bottle");
    purr_win_label(s_win, "Paired devices:");
    s_device_list = purr_win_list(s_win, 100, 25);
    purr_win_list_on_select(s_device_list, on_device_list_event, NULL);

    s_status_lbl = purr_win_label(s_win, "Select a paired device");

    s_big_lbl = purr_win_label(s_win, "");
    purr_win_label_align(s_big_lbl, PURR_ALIGN_CENTER);

    s_input = purr_win_textarea(s_win, 100, 20);
    purr_win_button(s_win, "Send", on_send_click, NULL);

    s_have_selection = false;
    s_rx_is_new = false;
    s_last_rx_text[0] = 0;
    refresh_device_list();
    purr_win_show(s_win);

    s_running = true;
    // No NVS/flash access anywhere in this task's own body — PSRAM-backed
    // stack, same rationale as nearby_app.c's own refresh_task().
    xTaskCreateWithCaps(refresh_task, "milkbottle_ref", 4096, NULL, 3, &s_refresh_task, MALLOC_CAP_SPIRAM);
    return 0;
}

static void milkbottle_app_deinit(void) {
    s_running = false;
    if (s_refresh_done) xSemaphoreTake(s_refresh_done, pdMS_TO_TICKS(2000));
    s_refresh_task = NULL;

    // Stop answering SEND_MSG once the app isn't open to show it anymore —
    // see this file's top comment on receive-only-while-open scope.
    proximity_rpc_register(MILKBOTTLE_ACTION_SEND_MSG, NULL);

    purr_win_destroy(s_win);
    s_win = 0; s_device_list = 0; s_status_lbl = 0; s_big_lbl = 0; s_input = 0;
    s_have_selection = false;
}

// ── Module header ─────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(milkbottle) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_APP,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "milkbottle",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = milkbottle_app_init,
    .deinit            = milkbottle_app_deinit,
};
