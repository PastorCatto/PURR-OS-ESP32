// reticulum_module.cpp — PURR_MOD_SYSTEM registration for the vendored
// Reticulum Network Stack (see vendor/VENDORED.md).
//
// Stage 3 of the plan doc: real Link establishment (both directions —
// this device can initiate a chat link to a heard peer, or accept one a
// peer initiates to it) and plain-text send/receive over it, exposed to
// plain-C consumers (the standalone chat app) via reticulum.h. Peer
// discovery is a Transport::AnnounceHandler, not the earlier Stage 2
// "log path table size" selftest — that's gone now, this IS the real
// consumer of what it was proving.

// Order matters here — some of these headers get away with a forward
// declaration of a type another one fully defines, same as microReticulum's
// own aggregate header (microReticulum.h, not vendored — see vendor/
// VENDORED.md) orders them: Transport before Reticulum before the rest.
// Confirmed live: Identity.h/Destination.h before Reticulum.h left
// RNS::Reticulum an incomplete type at the one place this file actually
// constructs one.
#include "Transport.h"
#include "Reticulum.h"
#include "Link.h"
#include "Destination.h"
#include "Identity.h"
#include "rns_radio_adapter.h"
#include "reticulum_api.h"

#include <microStore/FileSystem.h>
#include <microStore/Adapters/NoopFileSystem.h>

#include <string.h>

extern "C" {
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "../../kernel/core/purr_kernel.h"
}

static const char *TAG = "reticulum";

// Static, not stack-local — this task's own stack goes away if the task
// is ever stopped (purr_kernel_module_set_enabled(..., false), the same
// mesh-backend-switch path meshtastic/meshcore already use), but these
// objects' lifetime is the module's, not the task's.
static RNS::Reticulum   *s_reticulum = nullptr;
static RnsRadioAdapter  *s_radio_impl = nullptr;
static RNS::Interface    s_radio_interface(RNS::Type::NONE);
static RNS::Identity     s_identity(RNS::Type::NONE);
static RNS::Destination *s_destination = nullptr;

static TaskHandle_t s_task = nullptr;
static volatile bool s_stop_requested = false;

// Fetched once at task start (matches proximity_module.c's own "read
// once at init()" convention for the same underlying setting) and
// carried as this Destination's Announce app_data — the human-readable
// name real Reticulum peers (Sideband/NomadNet/MeshChat, not just other
// PURR OS devices) see next to this device's identity hash.
static char s_hostname[PURR_HOSTNAME_MAX];

#define RETICULUM_APP_NAME    "purros"
#define RETICULUM_ASPECT      "chat"
#define RETICULUM_LOOP_MS     100
#define RETICULUM_ANNOUNCE_MS 30000

// ── Peer tracking ────────────────────────────────────────────────────────
// In-RAM only, not persisted — every peer this device has heard Announce
// for, on the "purros.chat" aspect specifically (an AnnounceHandler's own
// aspect_filter, so unrelated Reticulum traffic sharing this radio never
// shows up here even though Transport hears it too).

#define RETICULUM_MAX_PEERS 16

struct ChatPeer {
    RNS::Bytes hash;
    char       name[RETICULUM_PEER_NAME_MAX];
};
static ChatPeer    s_peers[RETICULUM_MAX_PEERS];
static int         s_peer_count = 0;
static SemaphoreHandle_t s_peers_mutex = nullptr;

class ChatAnnounceHandler : public RNS::AnnounceHandler {
public:
    ChatAnnounceHandler() : RNS::AnnounceHandler(RETICULUM_APP_NAME "." RETICULUM_ASPECT) {}

    void received_announce(const RNS::Bytes &destination_hash, const RNS::Identity &announced_identity,
                            const RNS::Bytes &app_data) override
    {
        (void)announced_identity;
        if (!s_peers_mutex || xSemaphoreTake(s_peers_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;

        int idx = -1;
        for (int i = 0; i < s_peer_count; i++) {
            if (s_peers[i].hash == destination_hash) { idx = i; break; }
        }
        if (idx < 0) {
            if (s_peer_count >= RETICULUM_MAX_PEERS) {
                xSemaphoreGive(s_peers_mutex);
                return;   // full — oldest-heard peers just stop updating, same as
                          // any other fixed-capacity table in this codebase
            }
            idx = s_peer_count++;
            s_peers[idx].hash = destination_hash;
        }

        if (app_data && app_data.size() > 0) {
            size_t n = app_data.size() < sizeof(s_peers[idx].name) - 1 ? app_data.size() : sizeof(s_peers[idx].name) - 1;
            memcpy(s_peers[idx].name, app_data.data(), n);
            s_peers[idx].name[n] = 0;
        } else {
            snprintf(s_peers[idx].name, sizeof(s_peers[idx].name), "(unnamed)");
        }
        ESP_LOGI(TAG, "heard peer: %s (%s)", destination_hash.toHex().c_str(), s_peers[idx].name);

        xSemaphoreGive(s_peers_mutex);
    }
};
// A single shared_ptr, reused for both register_announce_handler() and
// deregister_announce_handler() below — constructing two independent
// shared_ptrs from the same raw ChatAnnounceHandler* would give each its
// own control block and double-delete the object once both refcounts
// separately hit zero.
static RNS::HAnnounceHandler s_announce_handler;

// ── Chat link ────────────────────────────────────────────────────────────
// One active link at a time (Stage 3's own deliberately minimal scope,
// see the plan doc) — either this device initiated it (reticulum_link_
// open()) or a peer did (Destination's own link_established callback,
// since s_destination->accepts_links(true) below).

static RNS::Link s_chat_link(RNS::Type::NONE);
static volatile bool s_link_incoming = false;

#define RETICULUM_INBOX_SLOTS 8
#define RETICULUM_INBOX_MSG_MAX 256
static char              s_inbox[RETICULUM_INBOX_SLOTS][RETICULUM_INBOX_MSG_MAX];
static volatile uint8_t  s_inbox_head = 0;   // next slot to write
static volatile uint8_t  s_inbox_tail = 0;   // next slot to read
static SemaphoreHandle_t s_inbox_mutex = nullptr;

static void on_link_packet(const RNS::Bytes &plaintext, const RNS::Packet &packet)
{
    (void)packet;
    if (!s_inbox_mutex || xSemaphoreTake(s_inbox_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;

    uint8_t next_head = (uint8_t)((s_inbox_head + 1) % RETICULUM_INBOX_SLOTS);
    if (next_head == s_inbox_tail) {
        // Full — drop the oldest rather than the newest, same "keep the
        // most recent, not the stalest" choice purr_kernel's own
        // notification ring buffer already makes.
        s_inbox_tail = (uint8_t)((s_inbox_tail + 1) % RETICULUM_INBOX_SLOTS);
    }
    size_t n = plaintext.size() < RETICULUM_INBOX_MSG_MAX - 1 ? plaintext.size() : RETICULUM_INBOX_MSG_MAX - 1;
    memcpy(s_inbox[s_inbox_head], plaintext.data(), n);
    s_inbox[s_inbox_head][n] = 0;
    s_inbox_head = next_head;

    xSemaphoreGive(s_inbox_mutex);
}

// Shared by both the outgoing (passed to RNS::Link's own constructor) and
// incoming (Destination's own link_established callback) paths — either
// way, the bookkeeping is identical: capture the link, wire its packet
// callback (per-instance, not settable until the Link object exists).
static void on_link_established(RNS::Link &link)
{
    s_chat_link = link;
    s_chat_link.set_packet_callback(on_link_packet);
    ESP_LOGI(TAG, "link established (%s)", s_link_incoming ? "incoming" : "outgoing");
}

static void on_link_closed(RNS::Link &link)
{
    (void)link;
    ESP_LOGI(TAG, "link closed");
    s_chat_link = RNS::Link(RNS::Type::NONE);
    s_link_incoming = false;
}

// Peer-initiated link — s_destination's own callback, not tied to any
// specific outgoing attempt. Only fires at all because accepts_links(true)
// is set below.
static void on_incoming_link(RNS::Link &link)
{
    s_link_incoming = true;
    on_link_established(link);
}

static void reticulum_task(void *arg)
{
    (void)arg;

    s_peers_mutex = xSemaphoreCreateMutex();
    s_inbox_mutex = xSemaphoreCreateMutex();

    // Register a filesystem FIRST — Transport::start() (called from
    // Reticulum::start() below) throws "FileSystem has not been
    // registered" if nothing is registered at all, even with RNS_USE_FS
    // off (confirmed live — see CMakeLists.txt's own comment on
    // USTORE_USE_NOOPFS). A real flash-backed adapter is a future item;
    // this is enough to get Transport's own transport-identity setup
    // past that throw and actually complete.
    microStore::FileSystem noop_fs{microStore::Adapters::NoopFileSystem()};
    noop_fs.init();
    RNS::Utilities::OS::register_filesystem(noop_fs);

    // Register + start the interface BEFORE constructing/starting
    // Reticulum itself — matches microReticulum's own reference example's
    // ordering (examples/lora_transport/src/main.cpp) exactly, rather
    // than assuming the two are independent of each other.
    ESP_LOGI(TAG, "starting Reticulum core...");
    purr_kernel_hostname_get(s_hostname, sizeof(s_hostname));
    ESP_LOGI(TAG, "hostname=%s", s_hostname);

    s_radio_impl = new RnsRadioAdapter("sx1262");
    s_radio_interface = s_radio_impl;
    RNS::Transport::register_interface(s_radio_interface);
    s_radio_interface.start();

    s_reticulum = new RNS::Reticulum();
    s_reticulum->start();

    // Fresh Identity every boot for now — a future item wires this to a
    // username-keyed persisted keypair (see the plan doc's "User-
    // management integration" section); this is deliberately still a
    // throwaway.
    s_identity = RNS::Identity();
    ESP_LOGI(TAG, "identity hash=%s", s_identity.hexhash().c_str());

    s_destination = new RNS::Destination(s_identity, RNS::Type::Destination::IN,
                                          RNS::Type::Destination::SINGLE,
                                          RETICULUM_APP_NAME, RETICULUM_ASPECT);
    s_destination->accepts_links(true);
    s_destination->set_link_established_callback(on_incoming_link);
    ESP_LOGI(TAG, "destination created (own hash=%s) — announcing every %ds",
             s_destination->hash().toHex().c_str(), RETICULUM_ANNOUNCE_MS / 1000);

    s_announce_handler = RNS::HAnnounceHandler(new ChatAnnounceHandler());
    RNS::Transport::register_announce_handler(s_announce_handler);

    RNS::Bytes app_data((const uint8_t *)s_hostname, strlen(s_hostname));
    s_destination->announce(app_data);

    uint32_t since_announce_ms = 0;

    while (!s_stop_requested) {
        s_reticulum->loop();
        vTaskDelay(pdMS_TO_TICKS(RETICULUM_LOOP_MS));
        since_announce_ms += RETICULUM_LOOP_MS;
        if (since_announce_ms >= RETICULUM_ANNOUNCE_MS) {
            since_announce_ms = 0;
            s_destination->announce(app_data);
            ESP_LOGI(TAG, "re-announced %s (%s)", s_identity.hexhash().c_str(), s_hostname);
        }
    }

    if (s_chat_link) s_chat_link.teardown();
    RNS::Transport::deregister_announce_handler(s_announce_handler);
    s_radio_interface.stop();
    RNS::Transport::deregister_interface(s_radio_interface);
    s_task = nullptr;
    vTaskDelete(nullptr);
}

// ── Module lifecycle ──────────────────────────────────────────────────────

static int module_init(void)
{
    // Mutually exclusive with meshtastic, meshcore, AND rnode — one
    // physical radio, one catcall_radio_t slot. Same shape as their own
    // guards (meshtastic_module.c/meshcore_module.cpp/rnode_module.c),
    // mirrored here. != RETICULUM already covers rnode too, no separate
    // preference branch needed.
    purr_mesh_backend_t pref = purr_kernel_mesh_backend_get();
    if (pref != PURR_MESH_BACKEND_RETICULUM) {
        ESP_LOGI(TAG, "declining to start — mesh backend preference is not reticulum");
        return PURR_MODULE_INIT_DECLINED;
    }
    if (purr_kernel_get_module("meshtastic")) {
        ESP_LOGW(TAG, "refusing to start — meshtastic is active (stop it first)");
        return PURR_MODULE_INIT_DECLINED;
    }
    if (purr_kernel_get_module("meshcore")) {
        ESP_LOGW(TAG, "refusing to start — meshcore is active (stop it first)");
        return PURR_MODULE_INIT_DECLINED;
    }
    if (purr_kernel_get_module("rnode")) {
        ESP_LOGW(TAG, "refusing to start — rnode is active (stop it first)");
        return PURR_MODULE_INIT_DECLINED;
    }

    s_stop_requested = false;
    BaseType_t ok = xTaskCreate(reticulum_task, "reticulum", 8192, nullptr, 3, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create reticulum task");
        return -1;
    }
    return 0;
}

static void module_deinit(void)
{
    s_stop_requested = true;
    // Best-effort — the task itself deletes on its next loop iteration
    // (within RETICULUM_LOOP_MS); not waited on synchronously here, same
    // "request, don't block deinit on it" shape other modules in this
    // tree already use for their own background tasks.
}

// ── C-callable bridge (reticulum.h) — consumed by the standalone app ──────

extern "C" {

void reticulum_own_hash(char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    if (!s_destination) { out[0] = 0; return; }
    snprintf(out, out_len, "%s", s_destination->hash().toHex().c_str());
}

int reticulum_peer_count(void)
{
    if (!s_peers_mutex) return 0;
    if (xSemaphoreTake(s_peers_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return 0;
    int n = s_peer_count;
    xSemaphoreGive(s_peers_mutex);
    return n;
}

bool reticulum_peer_at(int idx, char *hash_hex_out, size_t hash_hex_len, char *name_out, size_t name_len)
{
    if (!s_peers_mutex || idx < 0) return false;
    if (xSemaphoreTake(s_peers_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    bool ok = idx < s_peer_count;
    if (ok) {
        if (hash_hex_out) snprintf(hash_hex_out, hash_hex_len, "%s", s_peers[idx].hash.toHex().c_str());
        if (name_out) snprintf(name_out, name_len, "%s", s_peers[idx].name);
    }
    xSemaphoreGive(s_peers_mutex);
    return ok;
}

bool reticulum_link_open(const char *peer_hash_hex)
{
    if (!peer_hash_hex || !s_destination) return false;
    if (s_chat_link) return false;   // one at a time — see this file's own top comment

    RNS::Bytes peer_hash;
    peer_hash.assignHex(peer_hash_hex);
    if (!peer_hash) return false;

    // The peer's Identity must already be known — remembered from one of
    // their own Announces (any Announce validates and remembers the
    // sender's Identity, not just ones ChatAnnounceHandler's aspect
    // filter matches, but in practice that's exactly the peers we'd have
    // in our own list anyway).
    RNS::Identity peer_identity = RNS::Identity::recall(peer_hash);
    if (!peer_identity) {
        ESP_LOGW(TAG, "link_open: no known identity for %s — no announce heard yet?", peer_hash_hex);
        return false;
    }

    RNS::Destination peer_destination(peer_identity, RNS::Type::Destination::OUT,
                                       RNS::Type::Destination::SINGLE,
                                       RETICULUM_APP_NAME, RETICULUM_ASPECT);
    s_link_incoming = false;
    s_chat_link = RNS::Link(peer_destination, on_link_established, on_link_closed);
    return true;
}

void reticulum_link_close(void)
{
    if (s_chat_link) s_chat_link.teardown();
}

reticulum_link_status_t reticulum_link_status(void)
{
    if (!s_chat_link) return RETICULUM_LINK_NONE;
    switch (s_chat_link.status()) {
        case RNS::Type::Link::PENDING:   return RETICULUM_LINK_PENDING;
        case RNS::Type::Link::HANDSHAKE: return RETICULUM_LINK_HANDSHAKE;
        case RNS::Type::Link::ACTIVE:    return RETICULUM_LINK_ACTIVE;
        case RNS::Type::Link::STALE:     return RETICULUM_LINK_STALE;
        default:                         return RETICULUM_LINK_NONE;
    }
}

bool reticulum_link_is_incoming(void)
{
    return s_link_incoming;
}

bool reticulum_chat_send(const char *text)
{
    if (!text || !s_chat_link || s_chat_link.status() != RNS::Type::Link::ACTIVE) return false;
    RNS::Bytes data((const uint8_t *)text, strlen(text));
    RNS::Packet(s_chat_link, data).send();
    return true;
}

bool reticulum_chat_poll(char *text_out, size_t text_len)
{
    if (!s_inbox_mutex || !text_out || text_len == 0) return false;
    if (xSemaphoreTake(s_inbox_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;

    bool has_one = s_inbox_tail != s_inbox_head;
    if (has_one) {
        snprintf(text_out, text_len, "%s", s_inbox[s_inbox_tail]);
        s_inbox_tail = (uint8_t)((s_inbox_tail + 1) % RETICULUM_INBOX_SLOTS);
    }
    xSemaphoreGive(s_inbox_mutex);
    return has_one;
}

}  // extern "C"

// ── Module header ─────────────────────────────────────────────────────────
extern "C" {
#include "../../kernel/core/purr_module.h"

PURR_MODULE_REGISTER(reticulum) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "reticulum",
    .version           = "0.2.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = module_init,
    .deinit            = module_deinit,
};
}
