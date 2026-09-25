// meshcore_module.cpp — PURR_MOD_SYSTEM registration + mesh task + public API.
//
// meshcore_module.c is unconditionally compiled into every device's
// firmware (components under source/modules/ always are). CONFIG_PURR_
// FEATURE_MESHCORE is off by default everywhere; the #else branch below
// gives every caller real, linkable no-op symbols instead of a build
// failure. Same shape as meshtastic_module.c's own guard.
#include "meshcore_api.h"
#include "sdkconfig.h"

#ifdef CONFIG_PURR_FEATURE_MESHCORE

#include "mc_radio_adapter.h"
#include "mc_packet_pool.h"
#include "mc_contacts.h"
#include "mc_identity.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/core/purr_module.h"  // PURR_MODULE_INIT_DECLINED

#include <Mesh.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "meshcore";

static bool          s_ready = false;
static mc_rx_cb_t    s_rx_cbs[MC_MAX_RX_CB];
static TaskHandle_t  s_task = NULL;
static bool          s_task_uses_psram_stack = false;
static TaskHandle_t  s_persist_task = NULL;

static volatile uint32_t s_last_heartbeat_ms = 0;
#define MC_WATCHDOG_STALE_MS 5000UL

// ── Backing objects for the Mesh instance ──────────────────────────────────
static PurrRadioAdapter    s_radio_adapter;
static PurrMillisecondClock s_clock;
static PurrRNG             s_rng;
static PurrMainBoard       s_board;
static PurrRTCClock        s_rtc;
static PurrPacketManager   s_packet_mgr;
static PurrMeshTables      s_tables;

// ── TX request queue ────────────────────────────────────────────────────────
// mc_manager_send_text() may be called from any task (e.g. a UI button
// press). Mesh's PacketManager isn't thread-safe against being touched
// from two tasks at once (the mesh task's own loop() drains/fills it too),
// so — same decoupling meshtastic_module.c uses for its own tx queue —
// only a lightweight request crosses task boundaries here; the actual
// Mesh::create*()/send*() calls all happen on the mesh task itself.
#define MC_TX_TEXT_MAX 160
typedef struct {
    bool  is_channel;     // true: channel/group text; false: DM to a contact
    int   target_idx;     // channel index, or contact index
    char  text[MC_TX_TEXT_MAX];
} mc_tx_item_t;
#define MC_TX_QUEUE_DEPTH 4
static QueueHandle_t s_tx_queue = NULL;

// #define, not a class-scoped constant — must appear before PurrMesh's
// method bodies below use it (a #define doesn't respect C++ class scope,
// but it does need to be textually earlier in the file than its use).
#define MC_MAX_PEER_MATCHES 4

// ── PurrMesh: the protocol-level integration ────────────────────────────────
// Mesh (vendor/Mesh.h) already implements packet parsing, advert signature
// verification, MAC/decrypt, flood relay and dedup — this subclass only
// supplies contact/channel lookups and the "a message arrived" hooks,
// mirroring how thin mesh_router.c's own callback surface is relative to
// what it reuses from the rest of the module.
class PurrMesh : public mesh::Mesh {
public:
    PurrMesh()
        : mesh::Mesh(s_radio_adapter, s_clock, s_rng, s_rtc, s_packet_mgr, s_tables)
    {}

protected:
    bool allowPacketForward(const mesh::Packet* packet) override {
        (void)packet;
        return true;  // participate in flood relay, like a normal mesh node
    }

    int searchPeersByHash(const uint8_t* hash) override {
        return mc_contacts_find_by_hash(hash, PATH_HASH_SIZE, match_indices_, MC_MAX_PEER_MATCHES);
    }

    void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override {
        const mc_contact_t* c = mc_contacts_at(match_indices_[peer_idx]);
        if (!c) { memset(dest_secret, 0, PUB_KEY_SIZE); return; }
        self_id.calcSharedSecret(dest_secret, c->pub_key);
    }

    int searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels[], int max_matches) override {
        return mc_channels_find_by_hash(hash, PATH_HASH_SIZE, channels, max_matches);
    }

    void onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp,
                       const uint8_t* app_data, size_t app_data_len) override {
        // app_data's leading bytes are a name string in real MeshCore
        // adverts (companion_radio's own NAME_TYPE flag prefixes it) —
        // treat it as plain text if present, name stays empty otherwise
        // (contact still gets recorded, just unnamed until we learn more).
        char name[32] = {0};
        if (app_data_len > 0) {
            size_t n = app_data_len < sizeof(name) - 1 ? app_data_len : sizeof(name) - 1;
            memcpy(name, app_data, n);
            name[n] = 0;
        }
        int idx = mc_contacts_upsert(id.pub_key, name, timestamp);
        if (idx >= 0 && packet->getPathHashCount() > 0) {
            mc_contacts_set_path(idx, packet->path, packet->path_len);
        }
        s_last_heartbeat_ms = (uint32_t)s_clock.getMillis();
    }

    bool onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret,
                         uint8_t* path, uint8_t path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override {
        (void)packet; (void)secret; (void)extra_type; (void)extra; (void)extra_len;
        mc_contacts_set_path(match_indices_[sender_idx], path, path_len);
        return true;  // accept — triggers a reciprocal path send back to them
    }

    void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret,
                         uint8_t* data, size_t len) override {
        (void)packet; (void)secret;
        if (type != PAYLOAD_TYPE_TXT_MSG || len <= 4) return;
        // decrypted layout: 4-byte timestamp, then text (not NUL-terminated)
        dispatch_rx(match_indices_[sender_idx], -1, (const char*)&data[4], len - 4);
    }

    void onGroupDataRecv(mesh::Packet* packet, uint8_t type, const mesh::GroupChannel& channel,
                          uint8_t* data, size_t len) override {
        (void)packet;
        if (type != PAYLOAD_TYPE_GRP_TXT || len <= 4) return;
        int ch_idx = -1;
        for (int i = 0; i < mc_channels_count(); i++) {
            const mc_channel_t* c = mc_channels_at(i);
            if (c && memcmp(c->hash, channel.hash, PATH_HASH_SIZE) == 0) { ch_idx = i; break; }
        }
        dispatch_rx(-1, ch_idx, (const char*)&data[4], len - 4);
    }

private:
    int match_indices_[MC_MAX_PEER_MATCHES];

    void dispatch_rx(int contact_idx, int channel_idx, const char* text, size_t len) {
        char buf[MC_TX_TEXT_MAX];
        size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
        memcpy(buf, text, n);
        buf[n] = 0;
        for (int i = 0; i < MC_MAX_RX_CB; i++) {
            if (s_rx_cbs[i]) s_rx_cbs[i](contact_idx, channel_idx, buf);
        }
        s_last_heartbeat_ms = (uint32_t)s_clock.getMillis();
    }
};

static PurrMesh s_mesh;

// ── Mesh task ────────────────────────────────────────────────────────────

static void do_send(const mc_tx_item_t& item) {
    uint32_t now = s_rtc.getCurrentTime();
    uint8_t ts_and_text[4 + MC_TX_TEXT_MAX];
    memcpy(ts_and_text, &now, 4);
    size_t text_len = strnlen(item.text, MC_TX_TEXT_MAX);
    memcpy(&ts_and_text[4], item.text, text_len);
    size_t data_len = 4 + text_len;

    if (item.is_channel) {
        const mc_channel_t* ch = mc_channels_at(item.target_idx);
        if (!ch) return;
        mesh::GroupChannel gc;
        memcpy(gc.hash, ch->hash, PATH_HASH_SIZE);
        memcpy(gc.secret, ch->secret, PUB_KEY_SIZE);
        mesh::Packet* pkt = s_mesh.createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, gc, ts_and_text, data_len);
        if (pkt) s_mesh.sendFlood(pkt);
    } else {
        const mc_contact_t* c = mc_contacts_at(item.target_idx);
        if (!c) return;
        mesh::Identity dest(c->pub_key);
        uint8_t secret[PUB_KEY_SIZE];
        s_mesh.self_id.calcSharedSecret(secret, dest);
        mesh::Packet* pkt = s_mesh.createDatagram(PAYLOAD_TYPE_TXT_MSG, dest, secret, ts_and_text, data_len);
        if (!pkt) return;
        if (c->has_path) {
            s_mesh.sendDirect(pkt, c->path, c->path_len);
        } else {
            s_mesh.sendFlood(pkt);
        }
    }
}

static void mc_task(void *arg) {
    (void)arg;

    while (!purr_kernel_radio()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    mc_radio_apply_preset();  // SPI/RadioLib only, no flash — safe here

    s_mesh.begin();
    s_ready = true;
    ESP_LOGI(TAG, "mesh ready, pub_key[0]=%02X", s_mesh.self_id.pub_key[0]);

    for (;;) {
        s_mesh.loop();

        mc_tx_item_t item;
        while (xQueueReceive(s_tx_queue, &item, 0) == pdTRUE) {
            do_send(item);
        }

        s_last_heartbeat_ms = (uint32_t)s_clock.getMillis();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void mc_persist_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        mc_contacts_flush_if_dirty();
        mc_channels_flush_if_dirty();
    }
}

// ── Module lifecycle ────────────────────────────────────────────────────────

bool mc_manager_is_alive(void) {
    if (!s_ready) return false;
    return (s_clock.getMillis() - s_last_heartbeat_ms) < MC_WATCHDOG_STALE_MS;
}

uint32_t mc_manager_now_seconds(void) { return s_rtc.getCurrentTime(); }

int mc_manager_init(void) {
    // Mutually exclusive with meshtastic, reticulum, AND rnode — one
    // physical radio, one catcall_radio_t slot, four incompatible on-air
    // protocols.
    //
    // Preference check first: MSN's backend chooser (and Settings' mirrored
    // control) persist which protocol the user actually wants via
    // purr_kernel_mesh_backend_set(), then reboot — the preference is the
    // authoritative source of truth, not [flash] load order. != MESHCORE
    // already covers reticulum and rnode too, no separate branch needed.
    if (purr_kernel_mesh_backend_get() != PURR_MESH_BACKEND_MESHCORE) {
        ESP_LOGI(TAG, "declining to start — mesh backend preference is not meshcore");
        return PURR_MODULE_INIT_DECLINED;
    }
    // Secondary safety net, kept from before the preference existed: covers
    // a manual Terminal `start meshcore` while another backend happens to
    // already be running (preference and actually-loaded state can
    // legitimately diverge until the next reboot). PURR_MODULE_INIT_DECLINED,
    // not -1: this isn't a crash-loop symptom, see that constant's doc comment.
    if (purr_kernel_get_module("meshtastic")) {
        ESP_LOGW(TAG, "refusing to start — meshtastic is active (stop it first)");
        return PURR_MODULE_INIT_DECLINED;
    }
    if (purr_kernel_get_module("reticulum")) {
        ESP_LOGW(TAG, "refusing to start — reticulum is active (stop it first)");
        return PURR_MODULE_INIT_DECLINED;
    }
    if (purr_kernel_get_module("rnode")) {
        ESP_LOGW(TAG, "refusing to start — rnode is active (stop it first)");
        return PURR_MODULE_INIT_DECLINED;
    }

    s_tx_queue = xQueueCreate(MC_TX_QUEUE_DEPTH, sizeof(mc_tx_item_t));
    if (!s_tx_queue) {
        ESP_LOGE(TAG, "failed to create tx queue");
        return -1;
    }

    // Every NVS/flash touch this module needs (identity keypair, contact
    // table, channel table) happens here, synchronously, on the module-
    // loader's own (internal-RAM) task, before mc_task() exists — matches
    // mesh_radio_init()'s "all flash access before the task's main loop
    // begins" rule exactly. This is load-bearing, not just tidy: mc_task()
    // runs on a PSRAM-backed stack whenever PSRAM is available (see
    // below), and ESP32-S3 asserts (esp_task_stack_is_sane_cache_disabled())
    // if a flash op needs to disable cache while the calling task's own
    // stack lives in PSRAM — confirmed live as a hard crash/reboot loop
    // when mc_identity_get()'s NVS call was still inside mc_task().
    s_mesh.self_id = mc_identity_get(s_rng);
    mc_contacts_load();
    mc_channels_load();

    // Pinned to core 1 on both paths — mirrors meshtastic_module.c's mesh_task
    // grouping: mesh_task/mc_task + cupcake_task (UI) share core 1, app tasks
    // move to core 0 alongside WiFi/BT (see app_manager.c/cupcake_module.c).
    s_task_uses_psram_stack = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    BaseType_t ret = s_task_uses_psram_stack
        ? xTaskCreatePinnedToCoreWithCaps(mc_task, "meshcore", 8192, NULL, 4, &s_task, 1, MALLOC_CAP_SPIRAM)
        : xTaskCreatePinnedToCore(mc_task, "meshcore", 8192, NULL, 4, &s_task, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create mesh task");
        vQueueDelete(s_tx_queue);
        s_tx_queue = NULL;
        return -1;
    }

    ret = xTaskCreatePinnedToCore(mc_persist_task, "mc_persist", 3072, NULL, 2, &s_persist_task, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create persist task — contact/channel persistence disabled");
    }

    purr_kernel_health_register("meshcore", mc_manager_is_alive);
    return 0;
}

void mc_manager_deinit(void) {
    if (s_task) {
        if (s_task_uses_psram_stack) vTaskDeleteWithCaps(s_task);
        else                         vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_persist_task) {
        mc_contacts_flush_if_dirty();
        mc_channels_flush_if_dirty();
        vTaskDelete(s_persist_task);
        s_persist_task = NULL;
    }
    if (s_tx_queue) {
        vQueueDelete(s_tx_queue);
        s_tx_queue = NULL;
    }
    s_ready = false;
}

// ── Public API ───────────────────────────────────────────────────────────

bool mc_manager_ready(void) { return s_ready; }

bool mc_manager_send_text(int to_contact_idx, int channel_idx, const char *text) {
    if (!s_ready || !s_tx_queue || !text) return false;

    mc_tx_item_t item;
    memset(&item, 0, sizeof(item));
    strncpy(item.text, text, MC_TX_TEXT_MAX - 1);

    if (to_contact_idx >= 0) {
        item.is_channel = false;
        item.target_idx = to_contact_idx;
    } else {
        if (channel_idx < 0) return false;
        item.is_channel = true;
        item.target_idx = channel_idx;
    }
    return xQueueSend(s_tx_queue, &item, 0) == pdTRUE;
}

int mc_manager_channel_count(void) { return mc_channels_count(); }

bool mc_manager_channel_name(int idx, char *name_out, size_t name_max) {
    const mc_channel_t* c = mc_channels_at(idx);
    if (!c) return false;
    strncpy(name_out, c->name, name_max - 1);
    name_out[name_max - 1] = 0;
    return true;
}

int mc_manager_channel_add(const char *name, const uint8_t *psk, size_t psk_len) {
    if (psk_len != 16 && psk_len != 32) return -1;
    uint8_t secret[PUB_KEY_SIZE] = {0};
    memcpy(secret, psk, psk_len);
    return mc_channels_add(name, secret);
}

void mc_manager_channel_remove(int idx) { mc_channels_remove(idx); }

bool mc_manager_channel_hash(int idx, uint8_t *hash_out) {
    const mc_channel_t* c = mc_channels_at(idx);
    if (!c || !hash_out) return false;
    *hash_out = c->hash[0];
    return true;
}

int mc_manager_contact_count(void) { return mc_contacts_count(); }

bool mc_manager_contact_at(int idx, mc_contact_info_t *out) {
    const mc_contact_t* c = mc_contacts_at(idx);
    if (!c) return false;
    memcpy(out->pub_key, c->pub_key, sizeof(out->pub_key));
    strncpy(out->name, c->name, sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = 0;
    out->has_path = c->has_path;
    out->last_advert_timestamp = c->last_advert_timestamp;
    return true;
}

void mc_manager_contact_forget(int idx) { mc_contacts_forget(idx); }

void mc_manager_add_rx_callback(mc_rx_cb_t cb) {
    for (int i = 0; i < MC_MAX_RX_CB; i++) {
        if (!s_rx_cbs[i]) { s_rx_cbs[i] = cb; return; }
    }
}

void mc_manager_remove_rx_callback(mc_rx_cb_t cb) {
    for (int i = 0; i < MC_MAX_RX_CB; i++) {
        if (s_rx_cbs[i] == cb) { s_rx_cbs[i] = NULL; return; }
    }
}

#else  // !CONFIG_PURR_FEATURE_MESHCORE — see this file's top-of-file comment

bool     mc_manager_is_alive(void)                                       { return false; }
uint32_t mc_manager_now_seconds(void)                                    { return 0; }
int      mc_manager_init(void)                                           { return -1; }
void     mc_manager_deinit(void)                                         { }
bool     mc_manager_ready(void)                                          { return false; }
bool     mc_manager_send_text(int to, int channel_idx, const char *text) { (void)to; (void)channel_idx; (void)text; return false; }
int      mc_manager_channel_count(void)                                  { return 0; }
bool     mc_manager_channel_name(int idx, char *name_out, size_t max)    { (void)idx; (void)name_out; (void)max; return false; }
int      mc_manager_channel_add(const char *n, const uint8_t *p, size_t l) { (void)n; (void)p; (void)l; return -1; }
void     mc_manager_channel_remove(int idx)                              { (void)idx; }
bool     mc_manager_channel_hash(int idx, uint8_t *hash_out)             { (void)idx; (void)hash_out; return false; }
int      mc_manager_contact_count(void)                                  { return 0; }
bool     mc_manager_contact_at(int idx, mc_contact_info_t *out)          { (void)idx; (void)out; return false; }
void     mc_manager_contact_forget(int idx)                              { (void)idx; }
void     mc_manager_add_rx_callback(mc_rx_cb_t cb)                       { (void)cb; }
void     mc_manager_remove_rx_callback(mc_rx_cb_t cb)                    { (void)cb; }

#endif // CONFIG_PURR_FEATURE_MESHCORE

// ── Module header ─────────────────────────────────────────────────────────
// Placed outside the #ifdef, same as meshtastic_module.c's own — always
// links and registers regardless of the Kconfig flag; only the internal
// behavior above is stubbed when the feature is off.

extern "C" {
#include "purr_module.h"

PURR_MODULE_REGISTER(meshcore) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "meshcore",
    .version           = "0.1.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,   // checked at runtime via purr_kernel_radio(), matching meshtastic_module.c
    .init              = mc_manager_init,
    .deinit            = mc_manager_deinit,
};

}
