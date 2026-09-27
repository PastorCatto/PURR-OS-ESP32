// proximity_rpc.c — see proximity_rpc.h for the full design writeup.

#include "proximity_rpc.h"
#include "../proximity/proximity.h"
#include "../pairing/pairing.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/core/purr_module.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "proximity_rpc";

typedef enum {
    RPC_KIND_REQUEST  = 1,
    RPC_KIND_RESPONSE = 2,
} rpc_kind_t;

// Wire envelope, leads every PROXIMITY_FRAME_RELAY frame. chunk_len is the
// payload length carried in THIS specific frame (the last chunk of a
// message is typically shorter than the others); chunk_count is the total
// number of chunks in the whole logical message, known up front by the
// sender (it already has the full payload in hand before sending chunk 0).
typedef struct __attribute__((packed)) {
    uint8_t  kind;         // rpc_kind_t
    uint16_t seq_id;
    uint16_t action_id;    // meaningful for REQUEST; echoed back on RESPONSE for logging only
    uint8_t  chunk_idx;    // 0-based
    uint8_t  chunk_count;
    uint8_t  status;       // RESPONSE only: 0 = ok, 1 = handler reported failure
    uint16_t chunk_len;
} rpc_envelope_t;

#define RPC_HEADER_LEN       ((int)sizeof(rpc_envelope_t))
#define RPC_MAX_CHUNK_PAYLOAD (PROXIMITY_MAX_PAYLOAD - RPC_HEADER_LEN)

// Chunks are assumed to arrive in order — see proximity_rpc.h's top
// comment. A chunk that doesn't match next_chunk_idx aborts that
// reassembly slot outright (treated the same as a lost frame: the waiting
// caller just times out) rather than attempting out-of-order buffering.

#define RPC_MAX_INFLIGHT_CALLS      4
#define RPC_MAX_INBOUND_REASSEMBLY  2
// 16 silently overflowed on real hardware once app_manager_remote.c's
// two new DOWNLOAD_* actions pushed the real total past it: pairing_
// module.c registers 8 (6 USERAUTH_* + 2 OOBE_*), app_manager_remote.c
// registers 5 (LIST/LAUNCH/STOP + DOWNLOAD_INFO/DOWNLOAD_CHUNK), server_
// mgr.c registers 5 (WIFI_STATUS/SET + 3 APP_UPLOAD_*) — 18 already,
// with server_mgr's own last two (UPLOAD_CHUNK/END) losing the race and
// silently never registering (confirmed live: "action table full (16) —
// action_id=20483 not registered"), breaking its app-push feature
// entirely with no compile-time signal at all. 32 gives real headroom
// for what's already here plus room to grow — each action_slot_t is a
// few bytes (bool + uint16_t + a function pointer), so doubling costs
// nothing that matters.
#define RPC_MAX_ACTIONS             32

typedef struct {
    bool              in_use;
    uint16_t          seq_id;
    uint8_t           mac[6];
    uint8_t          *buf;          // PSRAM, allocated once at init, reused across calls
    size_t            received_len;
    uint8_t           next_chunk_idx;
    uint8_t           total_chunks; // 0 until chunk 0 tells us
    volatile bool     complete;
    volatile bool     failed;       // remote status != 0, or reassembly desync
    SemaphoreHandle_t done_sem;
} inflight_call_t;

typedef struct {
    bool     in_use;
    uint16_t seq_id;
    uint16_t action_id;
    uint8_t  mac[6];
    uint8_t *buf;                   // PSRAM, allocated once at init, reused
    size_t   received_len;
    uint8_t  next_chunk_idx;
    uint8_t  total_chunks;
    uint32_t last_activity_ms;      // for abandoning a stalled reassembly (see cleanup below)
} inbound_reassembly_t;

typedef struct {
    bool                     in_use;
    uint16_t                 action_id;
    proximity_rpc_handler_t  handler;
} action_slot_t;

static inflight_call_t        s_inflight[RPC_MAX_INFLIGHT_CALLS];
static inbound_reassembly_t   s_inbound[RPC_MAX_INBOUND_REASSEMBLY];
static action_slot_t          s_actions[RPC_MAX_ACTIONS];
// Guards allocation/lookup across all three tables above — frame handling
// (proximity_task()) and every proximity_rpc_call() caller task can touch
// these concurrently. Held only for short, non-blocking table operations,
// never across a semaphore wait or a handler invocation.
static SemaphoreHandle_t      s_table_lock;

// Abandons a stalled inbound reassembly after this long with no new chunk
// — otherwise a genuinely lost final chunk would permanently occupy one of
// only RPC_MAX_INBOUND_REASSEMBLY=2 slots for the rest of the session.
#define RPC_INBOUND_STALE_MS 5000UL

// Lazily created, not only in proximity_rpc_init() — app_manager (P2/
// IMPORTANT) loads before proximity_rpc (P3/OPTIONAL), so its own remote-
// apps responder registering an action via proximity_rpc_register() can
// genuinely run before this module's own init() has. Registration itself
// doesn't need the reassembly buffers (only touched once a real inbound
// frame arrives, which can't happen before proximity_rpc_init() has also
// run and attached the PROXIMITY_FRAME_RELAY handler) — it only needs
// this lock to exist. Same "if (!x) x = create()" idiom already used
// elsewhere in this codebase (e.g. meshdiag.c's s_refresh_done) rather
// than a heavier compare-and-swap — the realistic race window (two tasks
// both lazily creating this at the exact same instant) isn't exercised by
// this module's actual callers, all of which do so from sequential
// module-init call sites, not genuinely parallel ones.
static void ensure_table_lock(void) {
    if (!s_table_lock) s_table_lock = xSemaphoreCreateMutex();
}
static void table_lock(void)   { ensure_table_lock(); xSemaphoreTake(s_table_lock, portMAX_DELAY); }
static void table_unlock(void) { xSemaphoreGive(s_table_lock); }

// ── Sending (chunking) ──────────────────────────────────────────────────

static bool send_chunked(const uint8_t mac[6], rpc_kind_t kind, uint16_t seq_id,
                          uint16_t action_id, uint8_t status,
                          const uint8_t *payload, size_t payload_len) {
    uint8_t chunk_count = 1;
    if (payload_len > 0) {
        chunk_count = (uint8_t)((payload_len + RPC_MAX_CHUNK_PAYLOAD - 1) / RPC_MAX_CHUNK_PAYLOAD);
    }
    if (chunk_count == 0) chunk_count = 1;   // zero-length payload still sends one (empty) chunk

    uint8_t frame[PROXIMITY_MAX_PAYLOAD];
    size_t  offset = 0;
    for (uint8_t i = 0; i < chunk_count; i++) {
        size_t remaining = payload_len - offset;
        size_t this_len = remaining < (size_t)RPC_MAX_CHUNK_PAYLOAD ? remaining : (size_t)RPC_MAX_CHUNK_PAYLOAD;

        rpc_envelope_t env = {0};
        env.kind        = (uint8_t)kind;
        env.seq_id       = seq_id;
        env.action_id    = action_id;
        env.chunk_idx    = i;
        env.chunk_count  = chunk_count;
        env.status       = status;
        env.chunk_len    = (uint16_t)this_len;

        memcpy(frame, &env, sizeof(env));
        if (this_len > 0) memcpy(frame + sizeof(env), payload + offset, this_len);

        if (!proximity_send_unicast(mac, PROXIMITY_FRAME_RELAY, frame, sizeof(env) + this_len)) {
            ESP_LOGW(TAG, "send_chunked: unicast failed at chunk %u/%u", (unsigned)i, (unsigned)chunk_count);
            return false;
        }
        offset += this_len;
    }
    return true;
}

// ── Inbound frame handling (proximity_task() context) ───────────────────

static inflight_call_t *find_inflight(uint16_t seq_id) {
    for (int i = 0; i < RPC_MAX_INFLIGHT_CALLS; i++) {
        if (s_inflight[i].in_use && s_inflight[i].seq_id == seq_id) return &s_inflight[i];
    }
    return NULL;
}

static inbound_reassembly_t *find_or_alloc_inbound(const uint8_t mac[6], uint16_t seq_id, uint16_t action_id) {
    inbound_reassembly_t *stale_victim = NULL;
    for (int i = 0; i < RPC_MAX_INBOUND_REASSEMBLY; i++) {
        inbound_reassembly_t *r = &s_inbound[i];
        if (r->in_use && r->seq_id == seq_id && memcmp(r->mac, mac, 6) == 0) return r;
        if (!r->in_use && !stale_victim) stale_victim = r;
        if (r->in_use && !stale_victim &&
            (uint32_t)purr_kernel_uptime_ms() - r->last_activity_ms > RPC_INBOUND_STALE_MS) {
            stale_victim = r;   // reclaim an abandoned reassembly over a truly free slot, if any is stale
        }
    }
    if (!stale_victim) return NULL;   // both slots genuinely busy with live reassembly — drop this request
    stale_victim->in_use          = true;
    stale_victim->seq_id          = seq_id;
    stale_victim->action_id       = action_id;
    memcpy(stale_victim->mac, mac, 6);
    stale_victim->received_len    = 0;
    stale_victim->next_chunk_idx  = 0;
    stale_victim->total_chunks    = 0;
    stale_victim->last_activity_ms = (uint32_t)purr_kernel_uptime_ms();
    return stale_victim;
}

static proximity_rpc_handler_t find_handler(uint16_t action_id) {
    for (int i = 0; i < RPC_MAX_ACTIONS; i++) {
        if (s_actions[i].in_use && s_actions[i].action_id == action_id) return s_actions[i].handler;
    }
    return NULL;
}

static void handle_request_complete(inbound_reassembly_t *r) {
    proximity_rpc_handler_t handler = find_handler(r->action_id);
    if (!handler) {
        ESP_LOGW(TAG, "no handler registered for action_id=%u", (unsigned)r->action_id);
        send_chunked(r->mac, RPC_KIND_RESPONSE, r->seq_id, r->action_id, 1, NULL, 0);
        return;
    }

    // Response buffer reuses this same reassembly slot's PSRAM buffer —
    // the request has already been fully consumed into a local copy the
    // handler needs (req/req_len below point into r->buf directly, so the
    // handler must finish reading req before writing resp_out if it were
    // ever the SAME buffer — it isn't; see the separate resp scratch
    // buffer immediately below), avoiding a second PROXIMITY_RPC_MAX_MSG
    // PSRAM allocation per inbound reassembly slot.
    static uint8_t *s_resp_scratch = NULL;
    if (!s_resp_scratch) {
        s_resp_scratch = heap_caps_malloc(PROXIMITY_RPC_MAX_MSG, MALLOC_CAP_SPIRAM);
        if (!s_resp_scratch) s_resp_scratch = heap_caps_malloc(PROXIMITY_RPC_MAX_MSG, MALLOC_CAP_DEFAULT);
    }
    size_t resp_len = 0;
    bool ok = s_resp_scratch && handler(r->mac, r->action_id, r->buf, r->received_len,
                                         s_resp_scratch, s_resp_scratch ? PROXIMITY_RPC_MAX_MSG : 0, &resp_len);
    send_chunked(r->mac, RPC_KIND_RESPONSE, r->seq_id, r->action_id, ok ? 0 : 1,
                 ok ? s_resp_scratch : NULL, ok ? resp_len : 0);
}

static void on_relay_frame(const uint8_t *mac, int8_t rssi, const uint8_t *payload, size_t len) {
    (void)rssi;
    if (len < sizeof(rpc_envelope_t)) return;
    // Authorization boundary — see proximity_rpc.h's top comment. Every
    // inbound frame, request or response, must be from a trusted mac
    // before anything below touches a reassembly slot for it.
    if (!pairing_is_trusted(mac)) return;

    rpc_envelope_t env;
    memcpy(&env, payload, sizeof(env));
    const uint8_t *chunk_data = payload + sizeof(env);
    size_t chunk_data_len = len - sizeof(env);
    if (chunk_data_len < env.chunk_len) return;   // truncated frame, drop

    if (env.kind == RPC_KIND_REQUEST) {
        table_lock();
        inbound_reassembly_t *r = find_or_alloc_inbound(mac, env.seq_id, env.action_id);
        if (!r) { table_unlock(); return; }
        if (env.chunk_idx != r->next_chunk_idx ||
            r->received_len + env.chunk_len > PROXIMITY_RPC_MAX_MSG) {
            r->in_use = false;   // desync or oversize claim — abandon, next REQUEST chunk 0 gets a fresh slot
            table_unlock();
            return;
        }
        if (env.chunk_len > 0) memcpy(r->buf + r->received_len, chunk_data, env.chunk_len);
        r->received_len += env.chunk_len;
        r->next_chunk_idx++;
        r->total_chunks = env.chunk_count;
        r->last_activity_ms = (uint32_t)purr_kernel_uptime_ms();
        bool complete = (r->next_chunk_idx >= r->total_chunks);
        if (complete) r->in_use = false;   // free the slot now; handle_request_complete() below uses local copies
        table_unlock();

        if (complete) handle_request_complete(r);
        return;
    }

    // RPC_KIND_RESPONSE
    table_lock();
    inflight_call_t *c = find_inflight(env.seq_id);
    if (!c || memcmp(c->mac, mac, 6) != 0) { table_unlock(); return; }
    if (env.chunk_idx != c->next_chunk_idx ||
        c->received_len + env.chunk_len > PROXIMITY_RPC_MAX_MSG) {
        c->failed = true;
        c->complete = true;
        SemaphoreHandle_t sem = c->done_sem;
        table_unlock();
        xSemaphoreGive(sem);
        return;
    }
    if (env.chunk_len > 0) memcpy(c->buf + c->received_len, chunk_data, env.chunk_len);
    c->received_len += env.chunk_len;
    c->next_chunk_idx++;
    c->total_chunks = env.chunk_count;
    if (env.status != 0) c->failed = true;
    bool complete = (c->next_chunk_idx >= c->total_chunks);
    if (complete) c->complete = true;
    SemaphoreHandle_t sem = c->done_sem;
    table_unlock();
    if (complete) xSemaphoreGive(sem);
}

// ── Public API ────────────────────────────────────────────────────────────

void proximity_rpc_register(uint16_t action_id, proximity_rpc_handler_t handler) {
    table_lock();
    for (int i = 0; i < RPC_MAX_ACTIONS; i++) {
        if (s_actions[i].in_use && s_actions[i].action_id == action_id) {
            s_actions[i].handler = handler;
            table_unlock();
            return;
        }
    }
    for (int i = 0; i < RPC_MAX_ACTIONS; i++) {
        if (!s_actions[i].in_use) {
            s_actions[i].in_use    = true;
            s_actions[i].action_id = action_id;
            s_actions[i].handler   = handler;
            table_unlock();
            return;
        }
    }
    table_unlock();
    ESP_LOGE(TAG, "action table full (%d) — action_id=%u not registered", RPC_MAX_ACTIONS, (unsigned)action_id);
}

bool proximity_rpc_call(const uint8_t mac[6], uint16_t action_id,
                         const uint8_t *req, size_t req_len,
                         uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out,
                         uint32_t timeout_ms) {
    if (!mac || !pairing_is_trusted(mac)) return false;
    if (req_len > PROXIMITY_RPC_MAX_MSG) return false;

    table_lock();
    inflight_call_t *c = NULL;
    for (int i = 0; i < RPC_MAX_INFLIGHT_CALLS; i++) {
        if (!s_inflight[i].in_use) { c = &s_inflight[i]; break; }
    }
    if (!c) { table_unlock(); ESP_LOGW(TAG, "no free inflight-call slot"); return false; }
    c->in_use         = true;
    c->seq_id          = (uint16_t)(esp_random() & 0xFFFF);
    memcpy(c->mac, mac, 6);
    c->received_len    = 0;
    c->next_chunk_idx  = 0;
    c->total_chunks    = 0;
    c->complete        = false;
    c->failed          = false;
    uint16_t seq_id = c->seq_id;
    table_unlock();

    if (!send_chunked(mac, RPC_KIND_REQUEST, seq_id, action_id, 0, req, req_len)) {
        table_lock(); c->in_use = false; table_unlock();
        return false;
    }

    bool got = xSemaphoreTake(c->done_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;

    table_lock();
    bool ok = got && c->complete && !c->failed;
    if (ok) {
        size_t n = c->received_len < resp_cap ? c->received_len : resp_cap;
        memcpy(resp_out, c->buf, n);
        if (resp_len_out) *resp_len_out = n;
    }
    c->in_use = false;   // release the slot regardless of outcome — a late-arriving stray chunk
                          // after this point just fails find_inflight()'s lookup and is dropped
    table_unlock();
    return ok;
}

// ── Lifecycle ────────────────────────────────────────────────────────────

int proximity_rpc_init(void) {
    ensure_table_lock();   // no-op if a P2 module's action registration already created it
    if (!s_table_lock) {
        ESP_LOGE(TAG, "failed to create table lock");
        return -1;
    }

    // PSRAM-preferred, not PSRAM-required — this module must work on
    // Heltec V3 (psram=false in its device.pcat), the actual "headless
    // radio companion" target this whole feature is for, not just on
    // PSRAM-equipped devices like T-Deck Plus. heap_caps_malloc() with
    // MALLOC_CAP_SPIRAM as a hard requirement returns NULL outright on a
    // no-PSRAM board instead of substituting internal RAM; falling back
    // to MALLOC_CAP_DEFAULT explicitly here is what makes that substitution
    // happen. ~14KB total internal RAM if every fallback is hit
    // (RPC_MAX_INFLIGHT_CALLS + RPC_MAX_INBOUND_REASSEMBLY + 1 response-
    // scratch buffer, PROXIMITY_RPC_MAX_MSG each) — worth watching on
    // Heltec's own tight internal-RAM budget once this is on real
    // hardware; not pre-shrunk here since the actual cost is unverified
    // until then.
    bool alloc_ok = true;
    for (int i = 0; i < RPC_MAX_INFLIGHT_CALLS; i++) {
        s_inflight[i].buf = heap_caps_malloc(PROXIMITY_RPC_MAX_MSG, MALLOC_CAP_SPIRAM);
        if (!s_inflight[i].buf) s_inflight[i].buf = heap_caps_malloc(PROXIMITY_RPC_MAX_MSG, MALLOC_CAP_DEFAULT);
        s_inflight[i].done_sem = xSemaphoreCreateBinary();
        alloc_ok = alloc_ok && s_inflight[i].buf && s_inflight[i].done_sem;
    }
    for (int i = 0; i < RPC_MAX_INBOUND_REASSEMBLY; i++) {
        s_inbound[i].buf = heap_caps_malloc(PROXIMITY_RPC_MAX_MSG, MALLOC_CAP_SPIRAM);
        if (!s_inbound[i].buf) s_inbound[i].buf = heap_caps_malloc(PROXIMITY_RPC_MAX_MSG, MALLOC_CAP_DEFAULT);
        alloc_ok = alloc_ok && s_inbound[i].buf;
    }
    if (!alloc_ok) {
        ESP_LOGE(TAG, "alloc failed for reassembly buffers (PSRAM and internal RAM both exhausted)");
        return -1;
    }

    proximity_register_handler(PROXIMITY_FRAME_RELAY, on_relay_frame);
    ESP_LOGI(TAG, "ready");
    return 0;
}

void proximity_rpc_deinit(void) {
    proximity_register_handler(PROXIMITY_FRAME_RELAY, NULL);
    // Buffers/semaphores deliberately not freed — this module has no
    // runtime restart path today (same "static for module lifetime"
    // reasoning as most PURR_MOD_SYSTEM modules in this codebase); a
    // future deinit-then-reinit cycle would need to guard against
    // double-alloc here, not implemented since nothing exercises it yet.
}

// ── Module header ─────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(proximity_rpc) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "proximity_rpc",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = proximity_rpc_init,
    .deinit            = proximity_rpc_deinit,
};
