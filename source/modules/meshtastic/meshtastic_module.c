// meshtastic_module.c — PURR_MOD_SYSTEM registration + mesh task + public API.
// Ported from PURR-OS-0.11/CoreOS/system/kernel/modules/purr_mesh.cpp's
// purr_mesh_init()/mesh_task() — the key change is that this task waits on
// purr_kernel_radio() (whatever radio driver a device selects) rather than
// assuming a specific chip, and talks to it only through catcall_radio_t.

#include "meshtastic.h"
#include "sdkconfig.h"

// meshtastic_module.c is unconditionally compiled into every device's
// firmware (components under source/modules/ always are — confirmed live
// this session, it's why bt_mgr.c/mesh_ble.c broke 9/10 devices before
// getting the same guard). CONFIG_PURR_FEATURE_MESHTASTIC is off by
// default everywhere; the #else branch below gives every caller
// (meshchat.c's 10 call sites, mesh_ble.c) real, linkable no-op symbols
// instead of a build failure or a deleted app. See Kconfig.projbuild's
// help text.
#ifdef CONFIG_PURR_FEATURE_MESHTASTIC

#include "mesh_radio.h"
#include "mesh_router.h"
#include "mesh_ble.h"
#include "../../kernel/core/purr_kernel.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>
#include <pb_decode.h>
#include "meshtastic/portnums.pb.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/telemetry.pb.h"

static const char *TAG = "meshtastic";

static uint32_t     s_node_id = 0;
static bool          s_ready   = false;
static mesh_rx_cb_t  s_rx_cbs[MESH_MAX_RX_CB];
static TaskHandle_t  s_task    = NULL;
// Which allocator actually backs s_task's stack — decided at init time
// based on runtime PSRAM availability (see mesh_manager_init()'s comment).
// mesh_manager_deinit() must delete the task with the matching function.
static bool          s_task_uses_psram_stack = false;
static TaskHandle_t  s_persist_task = NULL;

// Outgoing packets are encoded synchronously (cheap: AES-CTR + a small
// memcpy, no radio access) but actually transmitted only on mesh_task()'s
// own task, via this queue — see mesh_manager_send_text()'s comment for
// why. Depth 4: a human sending messages faster than the radio can clear
// a ~10ms-polled queue is the actual backpressure signal, not a number
// tuned against a measurement.
typedef struct {
    uint8_t wire[256];
    size_t  len;
} mesh_tx_item_t;
#define MESH_TX_QUEUE_DEPTH 4
static QueueHandle_t s_tx_queue = NULL;

// NODEINFO_APP re-broadcast interval — matches the old code's actual timer
// (15 min), not the abbreviated "every 10 minutes" mentioned in the archived
// plan doc; port from the real, working code, not the doc summary.
#define MESH_ANNOUNCE_INTERVAL_MS (15UL * 60UL * 1000UL)

// Backup-poll interval for mesh_task()'s interrupt-driven RX wait — matches
// Meshtastic's own reasoning for keeping a slow poll even with a working
// DIO1 interrupt (edge-triggered GPIO interrupts can miss an edge; see
// sx1262_rl.cpp's ISR comment). Only used when the radio driver supports
// wait_rx_signal(); a driver without it falls back to the old flat 10ms.
#define MESH_RX_POLL_TIMEOUT_MS 500UL

// ── Liveness heartbeat ────────────────────────────────────────────────────────
// mesh_task() stamps this every loop iteration; mesh_manager_is_alive()
// (registered with purr_kernel_health_register() in mesh_manager_init())
// reads it, so the kernel's shared health watchdog can detect a hung/
// crashed mesh task and the Services app can show its live status.
static volatile uint32_t s_last_heartbeat_ms = 0;
#define MESH_WATCHDOG_STALE_MS   5000UL

static void mesh_task(void *arg)
{
    (void)arg;

    // Wait for a radio catcall to be registered — don't assume a specific
    // chip or load-priority ordering against it (same reasoning as
    // cardstack_module.c waiting on purr_kernel_boot_ready(), just waiting
    // on this module's actual dependency instead).
    while (!purr_kernel_radio()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    uint32_t freq_hz = mesh_radio_freq_for_region(NULL);  // US default for now
    mesh_radio_apply_preset(freq_hz);
    mesh_router_init(s_node_id);
    purr_kernel_set_lora_available(true);

    s_ready = true;
    ESP_LOGI(TAG, "ready id=%08lX  %luHz  SF%d BW%dkHz",
             (unsigned long)s_node_id, (unsigned long)freq_hz,
             MESH_SF, MESH_BW_HZ / 1000);
    purr_kernel_notify("Mesh ready", "Meshtastic mesh online", "meshtastic");

    // Explicit self-subscribe, not the default idle-task watch (project-
    // wide TWDT is off — see CoreOS/sdkconfig.defaults's own comment —
    // specifically to avoid false positives from legitimately long boot/
    // module-init sequences). This task is the other half of the fix: a
    // real hang here (radio wedged on the shared SPI2 bus, same bus as
    // display/SD) gets a full register/backtrace dump over the console
    // before rebooting, instead of only ever showing up as a cupcake
    // "UI TASK UNRESPONSIVE" symptom with no visibility into what mesh_task
    // itself was actually doing. Timeout is set device-wide in
    // sdkconfig_tdeck_plus.overrides, deliberately shorter than
    // purr_kernel.c's own UI_HANG_THRESHOLD_MS so this wins the race and
    // captures a real backtrace instead of crash_guard's blind reboot.
    //
    // #ifdef CONFIG_ESP_TASK_WDT_EN, not unconditional — this file is
    // shared across every device with the meshtastic module, and TWDT is
    // currently only opted into per-device via that device's own sdkconfig
    // overrides (T-Deck Plus's, specifically) — esp_task_wdt_add()/_reset()
    // aren't even linkable when a target hasn't enabled it (confirmed live
    // building for Heltec, which doesn't have the override yet).
#ifdef CONFIG_ESP_TASK_WDT_EN
    esp_task_wdt_add(NULL);
#endif

    // Announce ourselves on the mesh immediately, then on the interval above.
    uint32_t last_announce = (uint32_t)(esp_timer_get_time() / 1000ULL) - MESH_ANNOUNCE_INTERVAL_MS + 1000UL;

    for (;;) {
        const catcall_radio_t *radio = purr_kernel_radio();

        // ── RX ────────────────────────────────────────────────────────────
        // Locked for the whole data_available()/receive()/rssi()/snr()
        // sequence — see mesh_radio_lock()'s doc comment. Everything past
        // this point (decode, dedup, notify, callbacks) doesn't touch the
        // radio and doesn't need the lock held.
        mesh_radio_lock();
        bool have_data = radio && radio->data_available && radio->data_available();
        uint8_t raw[256];
        int raw_len = 0;
        int rssi = 0;
        float snr = 0.0f;
        if (have_data) {
            raw_len = radio->receive ? radio->receive(raw, sizeof(raw)) : 0;
            rssi = radio->rssi ? radio->rssi() : 0;
            snr  = radio->snr  ? radio->snr()  : 0.0f;
        }
        mesh_radio_unlock();
        if (have_data) {

            if (raw_len > 0) {
                uint32_t from = 0, to = 0, pkt_id = 0;
                uint8_t  hop_limit  = 0;
                bool     want_ack   = false;
                int      channel_idx = -1;
                int      portnum   = 0;
                uint8_t  payload[240];
                size_t   payload_len = 0;

                // Diagnostic — previously a failed decode (bad CRC, wrong
                // channel key, malformed header) was completely silent, the
                // same as nothing arriving at all. With a live "0 nodes"
                // report and no other visible errors, that ambiguity is the
                // actual blocker: the else branch below is what tells apart
                // "no RF ever lands" (never fires) from "RF lands but won't
                // decode" (fires with a real raw_len/rssi) — a wrong channel
                // PSK or sync word looks identical to dead silence otherwise.
                // A channel-hash-doesn't-match-anything-we-know packet also
                // returns false here, but that's expected/normal (someone
                // else's private room) — mesh_router_decode() itself only
                // logs a warning for genuine decode failures, not that case.
                // TEMPORARY diagnostic — raw header ground truth for every
                // arrival, decoded or not, while chasing a live DM-delivery
                // bug. Read directly out of raw[] (not mesh_router_decode()'s
                // out-params, which aren't populated on failure) so this
                // still shows a packet whose channel-hash doesn't match
                // anything in our table. Remove once resolved.
                if ((size_t)raw_len > 13) {
                    uint32_t raw_to, raw_from;
                    memcpy(&raw_to,   raw,     4);
                    memcpy(&raw_from, raw + 4, 4);
                    ESP_LOGI(TAG, "raw rx: %d bytes to=%08lX from=%08lX ch_hash=%02X rssi=%d snr=%.1f",
                             raw_len, (unsigned long)raw_to, (unsigned long)raw_from, raw[13], rssi, snr);
                }

                bool decoded = mesh_router_decode(raw, (size_t)raw_len, &from, &to, &pkt_id, &hop_limit,
                                        &want_ack, &channel_idx, &portnum, payload, &payload_len, sizeof(payload) - 1);
                if (!decoded) {
                    ESP_LOGD(TAG, "rx: %d bytes received but not decoded (rssi=%d snr=%.1f)",
                             raw_len, rssi, snr);
                }
                // Half-duplex radios can hear their own transmission echoed
                // back (antenna reflection, or the RX re-arm landing before
                // the TX's own tail has fully cleared) — without this check
                // that showed up as "messaging myself" in MeshChat and a
                // phantom self-entry in the node list. Real Meshtastic
                // filters this the same way (NodeDB never adds the local
                // node as a remote peer).
                if (decoded && from == s_node_id) decoded = false;

                if (decoded) {
                    if (!mesh_router_dedup_seen(from, pkt_id)) {
                        mesh_router_dedup_add(from, pkt_id);
                        mesh_router_node_touch(from, (int8_t)rssi, channel_idx, hop_limit);

                        ESP_LOGI(TAG, "rx from=%08lX to=%08lX ch=%d port=%d len=%u rssi=%d snr=%.1f",
                                 (unsigned long)from, (unsigned long)to, channel_idx,
                                 portnum, (unsigned)payload_len, rssi, snr);

                        // Implicit ACK — real Meshtastic firmware always
                        // sends this back for a unicast packet with
                        // want_ack set; without it, every real client
                        // (phone app, another node's own screen) sits
                        // waiting and eventually shows a delivery error,
                        // even though we received and processed the
                        // message fine. Never for broadcasts — only a
                        // packet addressed specifically to us.
                        if (to == s_node_id && want_ack) {
                            mesh_tx_item_t ack;
                            ack.len = mesh_router_encode_ack(ack.wire, sizeof(ack.wire), from, channel_idx, pkt_id);
                            if (ack.len > 0) xQueueSend(s_tx_queue, &ack, 0);
                        }

                        if (portnum == (int)meshtastic_PortNum_TEXT_MESSAGE_APP) {
                            payload[payload_len] = '\0';
                            char title[PURR_NOTIFY_TITLE_LEN];
                            snprintf(title, sizeof(title), "Mesh: %08lX", (unsigned long)from);
                            purr_kernel_notify(title, (const char *)payload, "meshtastic");
                        }

                        // NodeInfo carries the node's real display name — was
                        // previously decoded and thrown away (only rssi/
                        // last-seen were tracked via node_touch() above).
                        if (portnum == (int)meshtastic_PortNum_NODEINFO_APP) {
                            meshtastic_User user = meshtastic_User_init_zero;
                            pb_istream_t us = pb_istream_from_buffer(payload, payload_len);
                            if (pb_decode(&us, meshtastic_User_fields, &user)) {
                                mesh_router_node_set_name(from, user.long_name, user.short_name);
                                // This is the other half of the PKI upgrade
                                // (see mesh_router_encode_nodeinfo()'s
                                // matching comment) — once we have a node's
                                // public key on file, every future send to
                                // it automatically switches to real
                                // Meshtastic's PKI encryption instead of
                                // channel-PSK.
                                if (user.public_key.size == 32) {
                                    mesh_router_node_set_pubkey(from, user.public_key.bytes);
                                }
                            }
                        }

                        // TELEMETRY_APP carries DeviceMetrics.battery_level
                        // (0-100, >100 means "powered") among other metrics
                        // this codebase doesn't track yet (voltage, channel/
                        // air utilization — see mesh_router.h's node-list-
                        // enrichment plan for what's actually consumed).
                        if (portnum == (int)meshtastic_PortNum_TELEMETRY_APP) {
                            meshtastic_Telemetry tm = meshtastic_Telemetry_init_zero;
                            pb_istream_t ts = pb_istream_from_buffer(payload, payload_len);
                            if (pb_decode(&ts, meshtastic_Telemetry_fields, &tm) &&
                                tm.which_variant == meshtastic_Telemetry_device_metrics_tag &&
                                tm.variant.device_metrics.has_battery_level) {
                                mesh_router_node_set_battery(from, (uint8_t)tm.variant.device_metrics.battery_level);
                            }
                        }

                        for (int rcb = 0; rcb < MESH_MAX_RX_CB; rcb++) {
                            if (s_rx_cbs[rcb]) s_rx_cbs[rcb](from, to, channel_idx, portnum, payload, payload_len);
                        }

                        // Relay anything not addressed to us with hops still
                        // remaining — not just broadcasts. A direct message
                        // to some other node needs the same flood-relay
                        // treatment to reach a destination beyond direct
                        // 1-hop range (see mesh_router_relay()'s comment;
                        // matches real Meshtastic's !isToUs(p) check, which
                        // has no separate broadcast condition at all).
                        if (to != s_node_id && hop_limit > 0) {
                            mesh_router_relay(raw, (size_t)raw_len);
                        }
                    }
                }
            }
        }

        // ── Periodic NodeInfo announcement ─────────────────────────────────
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        s_last_heartbeat_ms = now_ms;
        if (now_ms - last_announce >= MESH_ANNOUNCE_INTERVAL_MS) {
            last_announce = now_ms;
            uint8_t wire[256];
            size_t  len = mesh_router_encode_nodeinfo(wire, sizeof(wire));
            if (len > 0 && radio && radio->send) {
                mesh_radio_lock();
                radio->send(wire, len);
                mesh_radio_unlock();
                ESP_LOGI(TAG, "nodeinfo broadcast id=%08lX", (unsigned long)s_node_id);
            }
        }

        // ── Outgoing message queue ───────────────────────────────────────
        // The actual transmit happens here, on this task, never on
        // whatever caller queued it (mesh_manager_send_text() is called
        // directly from a UI button press) — RadioLib's transmit() is a
        // blocking call, and at this preset's SF11/BW250kHz it can easily
        // take hundreds of ms of real airtime. Doing that synchronously on
        // cupcake_task used to happen inside its own purr_kernel_ui_lock()-
        // held lv_timer_handler() call, freezing the entire UI for the
        // duration — confirmed live via the frame-time watchdog: a single
        // send produced a 515ms "lv_timer_handler() took 515ms" warning.
        // Draining the whole queue each pass (not just one item) keeps a
        // burst of messages from backing up behind mesh_task()'s own 10ms
        // poll cadence.
        mesh_tx_item_t tx_item;
        while (xQueueReceive(s_tx_queue, &tx_item, 0) == pdTRUE) {
            if (radio && radio->send) {
                mesh_radio_lock();
                esp_err_t ret = radio->send(tx_item.wire, tx_item.len);
                mesh_radio_unlock();
                // to/ch pulled straight back out of the already-encoded
                // wire buffer (bytes 0-3 are always `to` in our
                // PacketHeader layout) rather than threading them through
                // mesh_tx_item_t separately — this is diagnostic-only, and
                // the wire bytes are the actual ground truth of what went
                // out, not whatever the caller thinks it asked for.
                uint32_t wire_to; memcpy(&wire_to, tx_item.wire, 4);
                ESP_LOGI(TAG, "tx to=%08lX ch_hash=%02X len=%u ok=%d",
                         (unsigned long)wire_to, tx_item.wire[13], (unsigned)tx_item.len, ret == ESP_OK);
            }
        }

#ifdef CONFIG_ESP_TASK_WDT_EN
#ifdef CONFIG_ESP_TASK_WDT_EN
        esp_task_wdt_reset();
#endif
#endif

        // Interrupt-driven wake instead of a flat vTaskDelay(10) — the
        // radio's DIO1 ISR (sx1262_rl.cpp) signals this the moment a real
        // RX event happens, so the unconditional every-10ms
        // data_available() SPI transaction at the top of this loop only
        // fires when something actually occurred, or MESH_RX_POLL_
        // TIMEOUT_MS elapses as a backup-poll safety net (see that
        // constant's own comment). Falls back to the exact old behavior
        // if the active radio driver doesn't support wait_rx_signal.
        if (!radio || !radio->wait_rx_signal) {
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            radio->wait_rx_signal(MESH_RX_POLL_TIMEOUT_MS);
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

// Registered with purr_kernel_health_register() below — the kernel's own
// shared watchdog polls this and pushes a purr_kernel_notify() the moment
// it transitions alive<->stale, so a hung/crashed mesh task is surfaced
// instead of silently going dark. Also what the Services app reads for
// Meshtastic's row.
bool mesh_manager_is_alive(void)
{
    if (!s_ready) return false;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    return (now_ms - s_last_heartbeat_ms) < MESH_WATCHDOG_STALE_MS;
}

bool mesh_manager_send_text(uint32_t to, int channel_idx, const char *text)
{
    if (!s_ready) return false;
    // Encoding (AES-CTR + memcpy) is cheap and doesn't touch the radio —
    // safe to do right here on the caller's own task (a UI button press).
    // The actual radio->send() does not happen here — it's queued for
    // mesh_task() to perform, since RadioLib's transmit() blocks for real
    // LoRa airtime (hundreds of ms at this preset) and doing that inline
    // on a UI callback froze the whole screen for the duration. Returning
    // true here means "queued for transmission", not "confirmed sent" —
    // see meshtastic.h's doc comment.
    mesh_tx_item_t item;
    item.len = mesh_router_encode_text(item.wire, sizeof(item.wire), to, channel_idx, text);
    if (item.len == 0) { ESP_LOGE(TAG, "encode failed"); return false; }

    if (xQueueSend(s_tx_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "tx queue full — message dropped");
        return false;
    }
    // mesh_task() may now be blocked in wait_rx_signal() for up to
    // MESH_RX_POLL_TIMEOUT_MS with nothing queued — wake it immediately
    // so this message doesn't sit for up to that long before it's
    // actually drained and sent. Called from this task's own context
    // (a UI button press), never an ISR, matching wake_rx_wait()'s
    // contract.
    const catcall_radio_t *radio = purr_kernel_radio();
    if (radio && radio->wake_rx_wait) radio->wake_rx_wait();
    return true;
}

int mesh_manager_channel_count(void) { return mesh_radio_channel_count(); }

bool mesh_manager_channel_name(int idx, char *name_out, size_t name_max)
{
    const mesh_channel_t *ch = mesh_radio_channel_at(idx);
    if (!ch || !name_out) return false;
    strncpy(name_out, ch->name, name_max - 1);
    name_out[name_max - 1] = '\0';
    return true;
}

int mesh_manager_add_channel(const char *name, const uint8_t psk16[16])
{
    return mesh_radio_add_channel(name, psk16);
}

void mesh_manager_remove_channel(int idx)
{
    mesh_radio_remove_channel(idx);
}

bool mesh_manager_channel_hash(int idx, uint8_t *hash_out)
{
    const mesh_channel_t *ch = mesh_radio_channel_at(idx);
    if (!ch || !hash_out) return false;
    *hash_out = ch->hash;
    return true;
}

void mesh_manager_add_rx_callback(mesh_rx_cb_t cb) {
    if (!cb) return;
    for (int i = 0; i < MESH_MAX_RX_CB; i++) {
        if (s_rx_cbs[i] == cb) return;         // already registered
        if (!s_rx_cbs[i]) { s_rx_cbs[i] = cb; return; }
    }
    ESP_LOGW(TAG, "rx callback table full (%d) — not registered", MESH_MAX_RX_CB);
}

void mesh_manager_remove_rx_callback(mesh_rx_cb_t cb) {
    for (int i = 0; i < MESH_MAX_RX_CB; i++) {
        if (s_rx_cbs[i] == cb) { s_rx_cbs[i] = NULL; return; }
    }
}
uint32_t mesh_manager_node_id(void)    { return s_node_id; }
int      mesh_manager_node_count(void) { return mesh_router_node_count(); }
bool     mesh_manager_ready(void)      { return s_ready; }

int mesh_manager_node_at(int idx, mesh_node_info_t *out)
{
    const mesh_node_t *n = mesh_router_node_at(idx);
    if (!n || !out) return -1;
    out->id          = n->id;
    out->rssi        = n->rssi;
    out->last_ms     = n->last_ms;
    out->channel_idx = n->channel_idx;
    out->battery_pct = n->battery_pct;
    // Approximation, not a real wire field — see mesh_node_t.hop_limit_at_
    // last_heard's doc comment. 0 (never heard, hop_limit_at_last_heard
    // still its zero-init default) reads as "0 hops away", indistinguishable
    // from a genuine direct neighbor — acceptable since both cases show the
    // same "no relay involved" meaning to the user.
    out->hops_away = (n->hop_limit_at_last_heard <= MESH_HOP_LIMIT)
                          ? (int)(MESH_HOP_LIMIT - n->hop_limit_at_last_heard) : 0;
    if (n->long_name[0]) {
        strncpy(out->long_name, n->long_name, sizeof(out->long_name) - 1);
        out->long_name[sizeof(out->long_name) - 1] = '\0';
    } else {
        // No NodeInfo heard from this node yet — "(pending)" makes clear
        // this is a temporary placeholder that'll resolve to a real name
        // once one arrives, not a garbled/broken ID. Keeps the hex handle
        // alongside it (not replaced with a bare "Unknown") since it's
        // still the only way to tell two not-yet-named nodes apart in a
        // list.
        snprintf(out->long_name, sizeof(out->long_name), "!%08lX (pending)", (unsigned long)n->id);
    }
    strncpy(out->short_name, n->short_name, sizeof(out->short_name) - 1);
    out->short_name[sizeof(out->short_name) - 1] = '\0';
    return 0;
}

void mesh_manager_node_forget(uint32_t id)
{
    mesh_router_node_forget(id);
}

// ── Deferred NVS persistence ───────────────────────────────────────────────────
// mesh_router.c's node table and mesh_radio.c's channel table are both
// mutated from PSRAM-backed stacks (mesh_task()'s RX loop, MeshChat's own
// app task) and so never call into NVS themselves anymore — they just flip
// a dirty flag. This task exists solely to notice that flag and do the
// actual flash write, on a plain internal-RAM stack where that's safe. A
// 1s poll is plenty — this is "known nodes/rooms" bookkeeping, not anything
// latency-sensitive.
// Set by mesh_manager_deinit() to ask this task to leave the loop and delete
// ITSELF, rather than being deleted from outside. See the deinit for the crash
// that forced this.
static volatile bool s_persist_stop = false;

static void mesh_persist_task(void *arg)
{
    (void)arg;
    while (!s_persist_stop) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_persist_stop) break;
        mesh_router_flush_nodes_if_dirty();
        mesh_radio_flush_channels_if_dirty();
    }
    // Self-delete on a known-safe boundary: never inside a flush, so newlib's
    // per-task reentrancy state (stdio buffers from the SD writes above) is
    // consistent when the TCB is reclaimed.
    s_persist_task = NULL;
    vTaskDelete(NULL);
}

// ── Module lifecycle ──────────────────────────────────────────────────────────

int mesh_manager_init(void)
{
    // Mutually exclusive with meshcore — one physical radio, one
    // catcall_radio_t slot, two incompatible on-air presets.
    //
    // Preference check first: MSN's backend chooser (and Settings'
    // mirrored control) persist which protocol the user actually wants via
    // purr_kernel_mesh_backend_set(), then reboot — so by the time this
    // runs again, the preference is the authoritative source of truth, not
    // [flash] load order. Declines via PURR_MODULE_INIT_DECLINED (not -1)
    // since this fires on every boot where the preference names meshcore —
    // not a crash-loop symptom, see PURR_MODULE_INIT_DECLINED's doc comment.
    if (purr_kernel_mesh_backend_get() == PURR_MESH_BACKEND_MESHCORE) {
        ESP_LOGI(TAG, "declining to start — mesh backend preference is meshcore");
        return PURR_MODULE_INIT_DECLINED;
    }
    // Secondary safety net, kept from before the preference existed: covers
    // a manual Terminal `start meshtastic` while meshcore happens to already
    // be running (preference and actually-loaded state can legitimately
    // diverge until the next reboot).
    if (purr_kernel_get_module("meshcore")) {
        ESP_LOGW(TAG, "refusing to start — meshcore is active (stop it first)");
        return PURR_MODULE_INIT_DECLINED;
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    s_node_id = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16)
              | ((uint32_t)mac[4] <<  8) |  (uint32_t)mac[5];

    s_tx_queue = xQueueCreate(MESH_TX_QUEUE_DEPTH, sizeof(mesh_tx_item_t));
    if (!s_tx_queue) {
        ESP_LOGE(TAG, "failed to create tx queue");
        return -1;
    }

    // Every NVS/flash touch this module needs (channel table, PKI identity
    // keypair, known-nodes table) happens here, on this task, before
    // mesh_task() exists — see mesh_radio_init()'s doc comment for the live
    // boot-loop crash this fixes (ESP32-S3 can't disable the flash cache
    // from a task whose own stack lives in PSRAM, which mesh_task()'s does).
    mesh_radio_init();
    mesh_router_load_nodes();

    // No NVS/flash/SD access anywhere in mesh_task()'s own call graph
    // (mesh_radio.c/mesh_router.c audited) — safe on a PSRAM-backed stack,
    // matching app_manager.c's launch_native()/launch_meow() pattern. Must
    // be paired with vTaskDeleteWithCaps() in mesh_manager_deinit() below.
    //
    // PSRAM-less devices (Heltec V3's device.pcat: psram=false, confirmed —
    // no "esp_psram: Found" line in its boot log, unlike tdeck_plus's)
    // can't satisfy a MALLOC_CAP_SPIRAM request at all — xTaskCreateWithCaps
    // would just fail outright. Fall back to a plain internal-RAM stack in
    // that case; the "NVS-free call graph" auditing above is what actually
    // makes a PSRAM stack safe, not a requirement — an internal-RAM stack
    // is safe regardless (if anything, more so, since it doesn't share the
    // cache-disable hazard PSRAM access has).
    // Tested forcing this to internal RAM this session as a diagnostic for
    // possible PSRAM/DMA bus contention against cupcake_task's PSRAM+DMA
    // display flushes — clean negative result (identical hang timing and
    // signature with mesh_task fully off PSRAM), so back to the normal
    // PSRAM-if-available rule.
    s_task_uses_psram_stack = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    // Pinned to core 1 on both paths — deliberate, repo-wide grouping:
    // mesh_task/mc_task + the UI's own cupcake_task share core 1, while
    // app tasks (native_task/meow_task) move to core 0 alongside WiFi/BT
    // (see app_manager.c's matching change). Originally this fallback path
    // alone was pinned to core 1 specifically to dodge WiFi/BT contention
    // on core 0 (confirmed live on Heltec V3 — mesh_task() never printed
    // even its first log line without this); that rationale still holds
    // here, just now applied consistently to the PSRAM-caps path too
    // rather than being fallback-path-only.
    //
    // DIAGNOSTIC — priority bumped from 4 (same as cupcake_task) to 5.
    // At equal priority, FreeRTOS time-slices between the two on their
    // shared core, meaning the 1ms tick can preempt mesh_task mid-way
    // through a held SPI bus lock and let cupcake_task try to touch the
    // same bus before mesh_task's own critical section (acquire -> CS ->
    // transfer -> CS -> release) has actually finished. A strictly higher
    // priority means the scheduler never preempts mesh_task in favor of
    // cupcake_task at all (only for an even-higher-priority task, and
    // nothing else on this core is above 4) -- its own SPI sequence runs
    // atomically instead. Testing whether this closes the still-
    // unexplained SPI stall this project has chased all session. Revert
    // to 4 if this doesn't change anything.
    BaseType_t ret = s_task_uses_psram_stack
        ? xTaskCreatePinnedToCoreWithCaps(mesh_task, "meshtastic", 8192, NULL, 5, &s_task, 1, MALLOC_CAP_SPIRAM)
        : xTaskCreatePinnedToCore(mesh_task, "meshtastic", 8192, NULL, 5, &s_task, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create mesh task");
        return -1;
    }

    // Plain xTaskCreate() — internal-RAM stack on purpose, see
    // mesh_persist_task()'s doc comment. Small and idle almost always, so a
    // failure here (extremely unlikely on a ~2KB ask) just means node/
    // channel-table changes stop persisting, not a hard init failure.
    // Clear the stop flag BEFORE creating the task. It is a file-static that
    // survives an unload/reload cycle, so a module restored after
    // mesh_manager_deinit() set it would otherwise start a task that
    // immediately exits — meshtastic would come back looking loaded, with node
    // and channel changes silently never persisting again. Speed demon restores
    // this module on every game exit, so that is the normal path, not an edge
    // case.
    s_persist_stop = false;
    ret = xTaskCreatePinnedToCore(mesh_persist_task, "mesh_persist", 3072, NULL, 2, &s_persist_task, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create mesh persist task — node/channel persistence disabled");
    }

    // bt_mgr (device.pcat [flash] priority 2) has already brought up
    // Bluedroid by the time this module (priority 3) loads — safe to
    // register the GATTS companion service here.
    mesh_ble_init();

    purr_kernel_health_register("meshtastic", mesh_manager_is_alive);
    return 0;
}

void mesh_manager_deinit(void)
{
    mesh_ble_deinit();
    // Must match whichever allocator actually created this task's stack
    // (see mesh_manager_init()'s comment — PSRAM-less devices fall back to
    // a plain internal-RAM stack, which vTaskDeleteWithCaps() isn't the
    // right call for).
    // Unsubscribe from the task watchdog BEFORE deleting the task.
    //
    // esp_task_wdt_add(NULL) in the task above registers its handle with the
    // TWDT. vTaskDelete() does not unregister it, so the watchdog keeps waiting
    // for resets from a task that no longer exists and panics when
    // CONFIG_ESP_TASK_WDT_TIMEOUT_S (4s here) elapses.
    //
    // This never mattered while this module was only unloaded at shutdown. Game
    // mode unloads it at runtime, and the result was a hard panic roughly four
    // seconds into every game-mode session, which then struck the crash guard
    // and rebooted. Harmless if the task was never subscribed - delete just
    // returns ESP_ERR_NOT_FOUND.
    if (s_task) {
        #ifdef CONFIG_ESP_TASK_WDT_EN
        esp_task_wdt_delete(s_task);
        #endif
        if (s_task_uses_psram_stack) vTaskDeleteWithCaps(s_task);
        else                         vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_persist_task) {
        // ASK it to stop, then wait — never vTaskDelete() it from here.
        //
        // This task flushes node/channel state to the SD card every second.
        // Deleting it from outside could land mid-write, and reclaiming the TCB
        // of a task killed inside stdio leaves newlib's per-task reentrancy
        // structures inconsistent. Reproduced on the fourth consecutive
        // game-mode round trip:
        //
        //   assert failed: heap_caps_free — "free() target pointer is outside
        //   heap areas"
        //   mesh_manager_deinit -> vTaskDelete -> prvDeleteTCB -> _reclaim_reent
        //
        // Flushing here first did not help and arguably made it worse: this runs
        // on a different task, so it could be writing the same files the persist
        // task was already writing.
        //
        // Bounded at ~1.5s, comfortably over the 1s poll. If it somehow does not
        // exit, leaving it running is strictly better than deleting it mid-write
        // — the pointer it would corrupt is the whole heap's.
        s_persist_stop = true;
        for (int i = 0; i < 150 && s_persist_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
        if (s_persist_task) {
            ESP_LOGW(TAG, "persist task did not exit — leaving it running");
            s_persist_task = NULL;
        }

        // Safe now: nothing else is touching these files.
        mesh_router_flush_nodes_if_dirty();
        mesh_radio_flush_channels_if_dirty();
    }
    // mesh_manager_init() unconditionally creates a new s_tx_queue every
    // call — without freeing the old one here first, a stop/start cycle
    // (see purr_kernel_module_restart()) leaked one queue's worth of memory
    // per restart. After the task deletions above, nothing can still be
    // reading/writing it.
    if (s_tx_queue) {
        vQueueDelete(s_tx_queue);
        s_tx_queue = NULL;
    }
    s_ready = false;
}

#else  // !CONFIG_PURR_FEATURE_MESHTASTIC — see this file's top-of-file comment

bool     mesh_manager_is_alive(void)                            { return false; }
bool     mesh_manager_send_text(uint32_t to, int channel_idx, const char *text)
                                                                 { (void)to; (void)channel_idx; (void)text; return false; }
void     mesh_manager_add_rx_callback(mesh_rx_cb_t cb)          { (void)cb; }
void     mesh_manager_remove_rx_callback(mesh_rx_cb_t cb)       { (void)cb; }
uint32_t mesh_manager_node_id(void)                             { return 0; }
int      mesh_manager_node_count(void)                          { return 0; }
bool     mesh_manager_ready(void)                                { return false; }
int      mesh_manager_node_at(int idx, mesh_node_info_t *out)   { (void)idx; (void)out; return -1; }
int      mesh_manager_channel_count(void)                       { return 0; }
bool     mesh_manager_channel_name(int idx, char *name_out, size_t name_max)
                                                                 { (void)idx; (void)name_out; (void)name_max; return false; }
int      mesh_manager_add_channel(const char *name, const uint8_t psk16[16])
                                                                 { (void)name; (void)psk16; return -1; }
void     mesh_manager_remove_channel(int idx)                   { (void)idx; }
bool     mesh_manager_channel_hash(int idx, uint8_t *hash_out)  { (void)idx; (void)hash_out; return false; }
void     mesh_manager_node_forget(uint32_t id)                  { (void)id; }
int      mesh_manager_init(void)                                { return 0; }
void     mesh_manager_deinit(void)                               {}

#endif  // CONFIG_PURR_FEATURE_MESHTASTIC

// ── Module header ─────────────────────────────────────────────────────────────
#include "../../kernel/core/purr_module.h"

PURR_MODULE_REGISTER(meshtastic) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "meshtastic",
    .version           = "1.0.1",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,   // checked at runtime via purr_kernel_radio(), not this field (see app_manager.c's own comment on the same pattern)
    .init              = mesh_manager_init,
    .deinit            = mesh_manager_deinit,
};
