// mesh_router.c — packet encode/decode, flood routing, dedup, node table.
// Ported from PURR-OS-0.11/CoreOS/system/kernel/modules/purr_mesh.cpp, but
// the wire format this file previously used (a nanopb-encoded
// meshtastic_MeshPacket directly as the LoRa payload) was WRONG — confirmed
// live once real Meshtastic RF traffic was finally being received (after
// this session's RadioLib driver rewrite fixed chip bring-up): every real
// packet's first 4 bytes decoded as 0xFF 0xFF 0xFF 0xFF, which looked like
// SPI/driver corruption but is actually the literal, correct broadcast
// value of a 4-byte native-endian `to` field. Real Meshtastic firmware
// (RadioInterface.h's PacketHeader struct, RadioLibInterface.cpp's
// handleReceiveInterrupt()) sends a fixed 16-byte RAW BINARY header —
// to,from,id (uint32 each, native/little-endian), flags, channel-hash,
// next_hop, relay_node — directly followed by the encrypted Data payload
// bytes. There is no protobuf-encoded outer MeshPacket on the air at all;
// only the inner Data (portnum+payload) is protobuf, and only after
// decryption. Confirmed byte-for-byte against real captured packets: byte
// 13 (the channel-hash byte) was 0x08 in every strong-signal capture,
// exactly matching xorHash("LongFast") ^ xorHash(defaultpsk) computed by
// hand — this is what real nearby nodes on the default channel actually
// transmit.
//
// relay_packet() is the one place this talks to the radio directly (a
// verbatim re-send of an already-encoded packet, not a new encode), via
// purr_kernel_radio() instead of the old lora_manager singleton.

#include "sdkconfig.h"

// No external caller besides meshtastic_module.c (see meshtastic.h vs
// mesh_router.h's own "internal, not a public API" comment) — when
// CONFIG_PURR_FEATURE_MESHTASTIC is off this whole file compiles to an
// empty translation unit, no stub needed. See Kconfig.projbuild's help
// text for why this needs to be a real compile-time gate.
#ifdef CONFIG_PURR_FEATURE_MESHTASTIC

#include "mesh_router.h"
#include "mesh_radio.h"
#include "../../kernel/core/purr_kernel.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <pb_encode.h>
#include <pb_decode.h>
#include "meshtastic/mesh.pb.h"
#include "meshtastic/portnums.pb.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "mesh_router";

static uint32_t s_node_id    = 0;
static uint32_t s_packet_seq = 1;

// ── Raw over-the-air PacketHeader — matches RadioInterface.h exactly ───────
// to,from,id (uint32 native-endian) + flags + channel-hash + next_hop +
// relay_node = 16 bytes, directly followed by the encrypted Data payload.
#define MESH_HDR_LEN                  16
#define MESH_FLAG_HOP_LIMIT_MASK      0x07
#define MESH_FLAG_WANT_ACK_MASK       0x08
#define MESH_FLAG_VIA_MQTT_MASK       0x10
#define MESH_FLAG_HOP_START_MASK      0xE0
#define MESH_FLAG_HOP_START_SHIFT     5

void mesh_router_init(uint32_t node_id)
{
    s_node_id = node_id;
    s_packet_seq = 1;
}

// ── Dedup ring buffer ─────────────────────────────────────────────────────────

#define DEDUP_SLOTS 32
typedef struct { uint32_t from; uint32_t id; } seen_pkt_t;
static seen_pkt_t s_seen[DEDUP_SLOTS];
static int        s_seen_head = 0;

bool mesh_router_dedup_seen(uint32_t from, uint32_t id)
{
    for (int i = 0; i < DEDUP_SLOTS; i++)
        if (s_seen[i].from == from && s_seen[i].id == id) return true;
    return false;
}

void mesh_router_dedup_add(uint32_t from, uint32_t id)
{
    s_seen[s_seen_head].from = from;
    s_seen[s_seen_head].id   = id;
    s_seen_head = (s_seen_head + 1) % DEDUP_SLOTS;
}

// ── Node table ────────────────────────────────────────────────────────────────

#define MAX_NODES 16
static mesh_node_t s_nodes[MAX_NODES];
static int         s_nnode = 0;

// Same NVS namespace mesh_radio.c's channel table uses ("purr_mesh"), just
// different keys ("nodes"/"node_count" vs. its "channels"/"count") — no
// collision, and keeping all of this module's persisted state in one
// namespace is simpler than inventing a second one.
#define MESH_NODES_NVS_NS "purr_mesh"

// node_touch() is called from mesh_task()'s own RX handler for every
// decoded packet — that task's stack is PSRAM-backed by design (see
// mesh_radio_init()'s doc comment for the boot-loop crash a direct NVS
// write from there caused previously). So no save_nodes() call below is
// ever made directly; every mutator just flips this flag, and the actual
// NVS write happens on mesh_persist_task()'s internal-RAM stack instead
// (see meshtastic_module.c) — up to ~1s of latency before a change is
// durable, which is fine for "known nodes" bookkeeping.
static volatile bool s_nodes_dirty = false;

static void save_nodes(void)
{
    nvs_handle_t h;
    if (nvs_open(MESH_NODES_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "nodes", s_nodes, sizeof(mesh_node_t) * (size_t)s_nnode);
    nvs_set_i32(h, "node_count", s_nnode);
    nvs_commit(h);
    nvs_close(h);
}

void mesh_router_flush_nodes_if_dirty(void)
{
    if (!s_nodes_dirty) return;
    s_nodes_dirty = false;
    save_nodes();
}

void mesh_router_load_nodes(void)
{
    nvs_handle_t h;
    if (nvs_open(MESH_NODES_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    int32_t count = 0;
    nvs_get_i32(h, "node_count", &count);
    if (count > 0) {
        if (count > MAX_NODES) count = MAX_NODES;
        size_t blob_len = sizeof(mesh_node_t) * (size_t)count;
        if (nvs_get_blob(h, "nodes", s_nodes, &blob_len) == ESP_OK) {
            s_nnode = count;
            // rssi/last_ms are stale the moment they're loaded from a
            // previous boot — reset so the UI doesn't show a fake-fresh
            // "just seen" state for a node we haven't actually heard yet
            // this boot. Repopulated the instant it's heard again.
            for (int i = 0; i < s_nnode; i++) {
                s_nodes[i].rssi                    = 0;
                s_nodes[i].last_ms                 = 0;
                s_nodes[i].hop_limit_at_last_heard = 0;
                s_nodes[i].battery_pct             = -1;
            }
        }
    }
    nvs_close(h);
}

void mesh_router_node_touch(uint32_t id, int8_t rssi, int channel_idx, uint8_t hop_limit)
{
    for (int i = 0; i < s_nnode; i++) {
        if (s_nodes[i].id == id) {
            // This runs on every single decoded packet from a known node —
            // rssi/last_ms/hop_limit_at_last_heard aren't even part of what
            // gets persisted (reset on load, see mesh_router_load_nodes()'s
            // comment), so saving here every time would mean an NVS write
            // per packet on a busy mesh. Only channel_idx is persisted
            // state, and only actually save when it *changes* (a node
            // heard on a different channel than before) — the common case
            // (same channel, every touch) costs nothing.
            bool channel_changed = s_nodes[i].channel_idx != channel_idx;
            s_nodes[i].rssi                    = rssi;
            s_nodes[i].last_ms                 = (uint32_t)(esp_timer_get_time() / 1000ULL);
            s_nodes[i].channel_idx             = channel_idx;
            s_nodes[i].hop_limit_at_last_heard = hop_limit;
            if (channel_changed) s_nodes_dirty = true;
            return;
        }
    }
    if (s_nnode < MAX_NODES) {
        // long_name/short_name start empty (static array, zero-initialized)
        // until a NodeInfo packet fills them in via node_set_name().
        s_nodes[s_nnode].id                      = id;
        s_nodes[s_nnode].rssi                    = rssi;
        s_nodes[s_nnode].last_ms                 = (uint32_t)(esp_timer_get_time() / 1000ULL);
        s_nodes[s_nnode].channel_idx             = channel_idx;
        s_nodes[s_nnode].hop_limit_at_last_heard = hop_limit;
        s_nodes[s_nnode].battery_pct             = -1;   // unknown until a TELEMETRY_APP packet arrives
        s_nnode++;
        s_nodes_dirty = true;
    }
}

void mesh_router_node_set_battery(uint32_t id, uint8_t pct)
{
    for (int i = 0; i < s_nnode; i++) {
        if (s_nodes[i].id == id) {
            s_nodes[i].battery_pct = (int8_t)(pct > 100 ? 100 : pct);   // >100 on the wire means "powered", clamp for display
            // Not persisted (s_nodes_dirty NOT set) — same staleness
            // rationale as rssi/last_ms: this is live telemetry, not
            // identity state worth an NVS write, and it's reset to -1 on
            // every load anyway (see mesh_router_load_nodes()).
            return;
        }
    }
    // See mesh_router_node_set_name()'s matching comment — node_touch()
    // always runs first for every decoded packet, including this one.
}

void mesh_router_node_set_name(uint32_t id, const char *long_name, const char *short_name)
{
    for (int i = 0; i < s_nnode; i++) {
        if (s_nodes[i].id == id) {
            if (long_name)  strncpy(s_nodes[i].long_name,  long_name,  sizeof(s_nodes[i].long_name) - 1);
            if (short_name) strncpy(s_nodes[i].short_name, short_name, sizeof(s_nodes[i].short_name) - 1);
            s_nodes_dirty = true;
            return;
        }
    }
    // mesh_task() always calls mesh_router_node_touch() first for every
    // decoded packet (including this NodeInfo one) — the node should already
    // exist by the time this runs, so there's nothing to do if not.
}

void mesh_router_node_set_pubkey(uint32_t id, const uint8_t pubkey[32])
{
    for (int i = 0; i < s_nnode; i++) {
        if (s_nodes[i].id == id) {
            memcpy(s_nodes[i].public_key, pubkey, 32);
            s_nodes[i].has_public_key = true;
            s_nodes_dirty = true;
            return;
        }
    }
    // See mesh_router_node_set_name()'s matching comment.
}

void mesh_router_node_forget(uint32_t id)
{
    for (int i = 0; i < s_nnode; i++) {
        if (s_nodes[i].id == id) {
            memmove(&s_nodes[i], &s_nodes[i + 1], sizeof(mesh_node_t) * (size_t)(s_nnode - i - 1));
            s_nnode--;
            // Called from MeshChat's "Forget Node" button — that app's own
            // task is also PSRAM-backed (see app_manager.c's launch_native(),
            // only settings/fileman get a safe static internal-RAM stack),
            // so this can't call save_nodes() directly either.
            s_nodes_dirty = true;
            return;
        }
    }
}

int mesh_router_node_count(void) { return s_nnode; }

const mesh_node_t *mesh_router_node_at(int idx)
{
    if (idx < 0 || idx >= s_nnode) return NULL;
    return &s_nodes[idx];
}

// ── Packet helpers ────────────────────────────────────────────────────────────

// Encrypt a plaintext Data payload and wrap it in a raw 16-byte PacketHeader
// (NOT a protobuf-encoded MeshPacket — see this file's top comment).
// Returns wire byte count, or 0 on failure.
static size_t encode_data_packet(uint8_t *wire, size_t wire_max,
                                  uint32_t to, uint8_t hop_limit, int channel_idx,
                                  const uint8_t *plain, size_t plain_len)
{
    if (plain_len > 233) return 0;  // Meshtastic max Data payload
    if (MESH_HDR_LEN + plain_len > wire_max) return 0;

    const mesh_channel_t *ch = mesh_radio_channel_at(channel_idx);
    if (!ch) return 0;

    uint32_t from = s_node_id;
    uint32_t id   = s_packet_seq++;

    uint8_t iv[16], cipher[256];
    mesh_radio_build_iv(iv, id, from);
    if (!mesh_radio_aes_ctr(plain, cipher, plain_len, ch->psk, iv)) return 0;

    uint8_t *p = wire;
    memcpy(p, &to,   4); p += 4;
    memcpy(p, &from, 4); p += 4;
    memcpy(p, &id,   4); p += 4;
    // hop_start == hop_limit at creation time (we're always the originator
    // here — mesh_router_relay() is the only place that decrements an
    // in-flight packet's hop_limit without touching hop_start).
    *p++ = (uint8_t)((hop_limit & MESH_FLAG_HOP_LIMIT_MASK) |
                      ((hop_limit << MESH_FLAG_HOP_START_SHIFT) & MESH_FLAG_HOP_START_MASK));
    *p++ = ch->hash;
    *p++ = 0;  // next_hop — no routing hints yet
    *p++ = 0;  // relay_node
    memcpy(p, cipher, plain_len);

    return MESH_HDR_LEN + plain_len;
}

// NULL if `id` isn't tracked yet or has no known public key on file.
static const uint8_t *find_pubkey(uint32_t id)
{
    for (int i = 0; i < s_nnode; i++) {
        if (s_nodes[i].id == id && s_nodes[i].has_public_key) return s_nodes[i].public_key;
    }
    return NULL;
}

// PKI-encrypted unicast packet — see mesh_radio.h's PKI section for the
// exact wire format this replicates (channel byte forced to 0 as the PKI
// sentinel, AES-256-CCM instead of the channel path's AES-CTR, and a
// 12-byte tag+extraNonce trailer appended after the ciphertext). Real
// Meshtastic's own max Data payload (233 bytes) shrinks to 221 for a PKI
// message — the trailer eats into the same on-air packet size budget.
static size_t encode_pki_packet(uint8_t *wire, size_t wire_max, uint32_t to, uint8_t hop_limit,
                                 const uint8_t their_pub[32], const uint8_t *plain, size_t plain_len)
{
    if (plain_len > 233 - MESH_PKI_OVERHEAD) return 0;
    if (MESH_HDR_LEN + plain_len + MESH_PKI_OVERHEAD > wire_max) return 0;

    uint32_t from = s_node_id;
    uint32_t id   = s_packet_seq++;

    uint8_t shared_key[32];
    if (!mesh_radio_ecdh_shared(their_pub, shared_key)) return 0;

    uint32_t extra_nonce = esp_random();
    uint8_t nonce[MESH_PKI_NONCE_LEN];
    mesh_radio_build_pki_nonce(nonce, id, extra_nonce, from);

    uint8_t cipher[256], tag[MESH_PKI_TAG_LEN];
    if (!mesh_radio_aes_ccm_encrypt(plain, cipher, plain_len, shared_key, nonce, tag)) return 0;

    uint8_t *p = wire;
    memcpy(p, &to,   4); p += 4;
    memcpy(p, &from, 4); p += 4;
    memcpy(p, &id,   4); p += 4;
    *p++ = (uint8_t)((hop_limit & MESH_FLAG_HOP_LIMIT_MASK) |
                      ((hop_limit << MESH_FLAG_HOP_START_SHIFT) & MESH_FLAG_HOP_START_MASK));
    *p++ = 0;  // channel byte 0 == "this is PKI, not a channel" sentinel
    *p++ = 0;  // next_hop
    *p++ = 0;  // relay_node
    memcpy(p, cipher, plain_len); p += plain_len;
    memcpy(p, tag, MESH_PKI_TAG_LEN); p += MESH_PKI_TAG_LEN;
    memcpy(p, &extra_nonce, 4); p += 4;

    return MESH_HDR_LEN + plain_len + MESH_PKI_OVERHEAD;
}

size_t mesh_router_encode_text(uint8_t *wire, size_t wire_max, uint32_t to, int channel_idx, const char *text)
{
    meshtastic_Data d = meshtastic_Data_init_zero;
    d.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    size_t tlen = strlen(text);
    if (tlen > sizeof(d.payload.bytes)) tlen = sizeof(d.payload.bytes);
    memcpy(d.payload.bytes, text, tlen);
    d.payload.size = (pb_size_t)tlen;

    uint8_t plain[meshtastic_Data_size];
    pb_ostream_t os = pb_ostream_from_buffer(plain, sizeof(plain));
    if (!pb_encode(&os, meshtastic_Data_fields, &d)) return 0;

    // Automatic PKI upgrade for a direct message to a node whose public
    // key we already have on file — matches real Meshtastic's own
    // behavior exactly (broadcasts never PKI-encrypt; PKI is inherently
    // point-to-point). Falls back to today's channel-PSK path when we
    // don't have a key for the destination yet (a node we've only heard
    // indirectly, or one running pre-PKI firmware).
    if (to != (uint32_t)MESH_BROADCAST) {
        const uint8_t *their_pub = find_pubkey(to);
        if (their_pub) {
            return encode_pki_packet(wire, wire_max, to, MESH_HOP_LIMIT,
                                      their_pub, plain, os.bytes_written);
        }
    }

    return encode_data_packet(wire, wire_max, to,
                               MESH_HOP_LIMIT, channel_idx, plain, os.bytes_written);
}

size_t mesh_router_encode_nodeinfo(uint8_t *wire, size_t wire_max)
{
    meshtastic_User user = meshtastic_User_init_zero;
    snprintf(user.id,         sizeof(user.id),         "!%08lx",     (unsigned long)s_node_id);
    snprintf(user.long_name,  sizeof(user.long_name),  "PURR-%08lX", (unsigned long)s_node_id);
    snprintf(user.short_name, sizeof(user.short_name), "PRR");
    user.hw_model = meshtastic_HardwareModel_HELTEC_V3;
    // Without this, modern Meshtastic clients (v2.5+, which always PKI-
    // encrypt direct messages) have no key to encrypt a DM to us with —
    // confirmed live as the actual cause of DMs failing instantly on the
    // sender's end, "empty ack"/"unencrypted" warnings, well before any
    // radio transmission happens. See mesh_radio.h's PKI section.
    mesh_radio_identity_pubkey(user.public_key.bytes);
    user.public_key.size = 32;

    uint8_t user_bytes[meshtastic_User_size];
    pb_ostream_t us = pb_ostream_from_buffer(user_bytes, sizeof(user_bytes));
    if (!pb_encode(&us, meshtastic_User_fields, &user)) return 0;

    meshtastic_Data d = meshtastic_Data_init_zero;
    d.portnum = meshtastic_PortNum_NODEINFO_APP;
    memcpy(d.payload.bytes, user_bytes, us.bytes_written);
    d.payload.size = (pb_size_t)us.bytes_written;

    uint8_t plain[meshtastic_Data_size];
    pb_ostream_t ds = pb_ostream_from_buffer(plain, sizeof(plain));
    if (!pb_encode(&ds, meshtastic_Data_fields, &d)) return 0;

    // Always the primary channel (0) — NodeInfo is this node's public
    // identity announcement, not scoped to any one private room, matching
    // this project's whole prior single-channel behavior exactly.
    return encode_data_packet(wire, wire_max, (uint32_t)MESH_BROADCAST,
                               MESH_HOP_LIMIT, 0, plain, ds.bytes_written);
}

size_t mesh_router_encode_ack(uint8_t *wire, size_t wire_max, uint32_t to, int channel_idx, uint32_t request_id)
{
    meshtastic_Routing routing = meshtastic_Routing_init_zero;
    routing.which_variant = meshtastic_Routing_error_reason_tag;
    routing.error_reason  = meshtastic_Routing_Error_NONE;

    uint8_t routing_bytes[meshtastic_Routing_size];
    pb_ostream_t rs = pb_ostream_from_buffer(routing_bytes, sizeof(routing_bytes));
    if (!pb_encode(&rs, meshtastic_Routing_fields, &routing)) return 0;

    meshtastic_Data d = meshtastic_Data_init_zero;
    d.portnum    = meshtastic_PortNum_ROUTING_APP;
    d.request_id = request_id;   // ties this ack back to the sender's original packet id
    memcpy(d.payload.bytes, routing_bytes, rs.bytes_written);
    d.payload.size = (pb_size_t)rs.bytes_written;

    uint8_t plain[meshtastic_Data_size];
    pb_ostream_t ds = pb_ostream_from_buffer(plain, sizeof(plain));
    if (!pb_encode(&ds, meshtastic_Data_fields, &d)) return 0;

    // Same automatic PKI upgrade as mesh_router_encode_text() — an ack is
    // just another outgoing unicast packet, and real Meshtastic makes this
    // same to-node decision for every send regardless of how the packet
    // being acked arrived. Falls back to channel_idx (must be the SAME
    // channel the original packet arrived on, or the sender's own decode
    // won't match it) when we don't have a PKI key for `to`.
    const uint8_t *their_pub = find_pubkey(to);
    if (their_pub) {
        return encode_pki_packet(wire, wire_max, to, MESH_HOP_LIMIT,
                                  their_pub, plain, ds.bytes_written);
    }

    // Acks aren't broadcast and don't need to travel far — same hop_limit
    // as everything else here, MESH_HOP_LIMIT is only 3 anyway.
    return encode_data_packet(wire, wire_max, to,
                               MESH_HOP_LIMIT, channel_idx, plain, ds.bytes_written);
}

void mesh_router_relay(const uint8_t *raw, size_t raw_len)
{
    if (raw_len <= MESH_HDR_LEN || raw_len > 256) return;

    uint32_t hdr_to, hdr_from, hdr_id;
    memcpy(&hdr_to,   raw,     4);
    memcpy(&hdr_from, raw + 4, 4);
    memcpy(&hdr_id,   raw + 8, 4);
    // Relay anything not addressed to us, not just broadcasts — real
    // Meshtastic's FloodingRouter::perhapsRebroadcast() only checks
    // !isToUs(p), no separate broadcast check at all. A direct message
    // to some other node needs exactly the same flood-relay treatment a
    // broadcast gets to ever reach a destination beyond direct 1-hop
    // range; restricting this to broadcast-only (the previous check)
    // silently dropped every multi-hop DM at the first relay.
    if (hdr_to == s_node_id) return;

    uint8_t flags = raw[12];
    uint8_t hop_limit = flags & MESH_FLAG_HOP_LIMIT_MASK;
    if (hop_limit == 0) return;
    hop_limit--;

    uint8_t relay_buf[256];
    memcpy(relay_buf, raw, raw_len);
    // Only the hop_limit bits change — hop_start/want_ack/via_mqtt and the
    // channel-hash/next_hop/relay_node bytes are relayed verbatim.
    relay_buf[12] = (uint8_t)((flags & ~MESH_FLAG_HOP_LIMIT_MASK) | hop_limit);

    // Random back-off 0-500ms to reduce simultaneous relay collisions
    uint32_t delay_ms = esp_random() % 500;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));

    // Racing mesh_manager_send_text() (a different task) and mesh_task()'s
    // own RX-poll loop otherwise — see mesh_radio_lock()'s doc comment.
    const catcall_radio_t *radio = purr_kernel_radio();
    if (radio && radio->send) {
        mesh_radio_lock();
        radio->send(relay_buf, raw_len);
        mesh_radio_unlock();
    }

    ESP_LOGI(TAG, "relay from=%08lX id=%lu hops_left=%u delay=%lums",
             (unsigned long)hdr_from, (unsigned long)hdr_id,
             (unsigned)hop_limit, (unsigned long)delay_ms);
}

bool mesh_router_decode(const uint8_t *raw, size_t raw_len,
                         uint32_t *from, uint32_t *to, uint32_t *pkt_id,
                         uint8_t *hop_limit, bool *want_ack, int *channel_idx, int *portnum,
                         uint8_t *payload, size_t *payload_len, size_t payload_max)
{
    if (raw_len <= MESH_HDR_LEN) {
        ESP_LOGW(TAG, "decode: raw_len=%u too short for %d-byte header",
                 (unsigned)raw_len, MESH_HDR_LEN);
        return false;
    }

    uint32_t hdr_to, hdr_from, hdr_id;
    memcpy(&hdr_to,   raw,     4);
    memcpy(&hdr_from, raw + 4, 4);
    memcpy(&hdr_id,   raw + 8, 4);
    uint8_t flags = raw[12];
    uint8_t hdr_channel_hash = raw[13];

    *from      = hdr_from;
    *to        = hdr_to;
    *pkt_id    = hdr_id;
    *hop_limit = flags & MESH_FLAG_HOP_LIMIT_MASK;
    *want_ack  = (flags & MESH_FLAG_WANT_ACK_MASK) != 0;

    uint8_t plain[256];
    size_t  plain_len;

    if (hdr_channel_hash == 0) {
        // PKI sentinel — checked before any channel-hash lookup, matching
        // real Meshtastic's Router::perhapsDecode() (`p->channel == 0`
        // triggers a PKI decrypt attempt first). We can only decrypt this
        // if we already have `from`'s public key on file (from a prior
        // NodeInfo) — same real limitation Meshtastic itself has; there's
        // no per-message key exchange to fall back on.
        const uint8_t *their_pub = find_pubkey(hdr_from);
        if (!their_pub) return false;   // no key yet — expected/normal, not an error

        if (raw_len <= MESH_HDR_LEN + MESH_PKI_OVERHEAD) {
            ESP_LOGW(TAG, "decode: from=%08lX id=%lu too short for PKI trailer",
                     (unsigned long)hdr_from, (unsigned long)hdr_id);
            return false;
        }
        size_t cipher_len = raw_len - MESH_HDR_LEN - MESH_PKI_OVERHEAD;
        if (cipher_len > sizeof(plain)) {
            ESP_LOGW(TAG, "decode: from=%08lX id=%lu cipher_len=%u exceeds plain buffer",
                     (unsigned long)hdr_from, (unsigned long)hdr_id, (unsigned)cipher_len);
            return false;
        }
        const uint8_t *cipher = raw + MESH_HDR_LEN;
        const uint8_t *tag    = cipher + cipher_len;              // MESH_PKI_TAG_LEN bytes
        uint32_t extra_nonce;
        memcpy(&extra_nonce, tag + MESH_PKI_TAG_LEN, 4);           // trailing 4 bytes, in the clear

        uint8_t shared_key[32];
        if (!mesh_radio_ecdh_shared(their_pub, shared_key)) {
            ESP_LOGW(TAG, "decode: from=%08lX id=%lu ECDH failed",
                     (unsigned long)hdr_from, (unsigned long)hdr_id);
            return false;
        }
        uint8_t nonce[MESH_PKI_NONCE_LEN];
        mesh_radio_build_pki_nonce(nonce, hdr_id, extra_nonce, hdr_from);
        if (!mesh_radio_aes_ccm_decrypt(cipher, plain, cipher_len, shared_key, nonce, tag)) {
            // A genuine auth failure (wrong key, corrupted packet) — not
            // logged as loudly as other paths since PKI auth failures can
            // also just mean "not really addressed to us" noise on a busy
            // channel, same spirit as the channel-hash-mismatch silence.
            return false;
        }
        plain_len = cipher_len;
        *channel_idx = -1;   // PKI has no channel — sentinel for callers
    } else {
        // Try every channel we know rather than assuming the single
        // hardcoded default — a packet on a channel we don't have
        // configured is expected/normal (someone else's private room),
        // not an error, so this isn't logged as a warning the way the
        // failure paths below are.
        int ch_idx = mesh_radio_channel_find_by_hash(hdr_channel_hash);
        if (ch_idx < 0) return false;
        *channel_idx = ch_idx;
        const mesh_channel_t *ch = mesh_radio_channel_at(ch_idx);

        const uint8_t *enc = raw + MESH_HDR_LEN;
        size_t enc_len = raw_len - MESH_HDR_LEN;
        if (enc_len > sizeof(plain)) {
            ESP_LOGW(TAG, "decode: from=%08lX id=%lu enc_len=%u exceeds plain buffer",
                     (unsigned long)hdr_from, (unsigned long)hdr_id, (unsigned)enc_len);
            return false;
        }
        uint8_t iv[16];
        mesh_radio_build_iv(iv, hdr_id, hdr_from);
        if (!mesh_radio_aes_ctr(enc, plain, enc_len, ch->psk, iv)) {
            ESP_LOGW(TAG, "decode: from=%08lX id=%lu AES-CTR call itself failed",
                     (unsigned long)hdr_from, (unsigned long)hdr_id);
            return false;
        }
        plain_len = enc_len;
    }

    meshtastic_Data d = meshtastic_Data_init_zero;
    pb_istream_t ds = pb_istream_from_buffer(plain, plain_len);
    if (!pb_decode(&ds, meshtastic_Data_fields, &d)) {
        ESP_LOGW(TAG, "decode: from=%08lX id=%lu channel=%u plain_len=%u inner Data pb_decode failed: %s",
                 (unsigned long)hdr_from, (unsigned long)hdr_id, (unsigned)hdr_channel_hash,
                 (unsigned)plain_len, PB_GET_ERROR(&ds));
        return false;
    }

    *portnum = (int)d.portnum;
    size_t n = d.payload.size < payload_max ? d.payload.size : payload_max;
    memcpy(payload, d.payload.bytes, n);
    *payload_len = n;
    return true;
}

#endif  // CONFIG_PURR_FEATURE_MESHTASTIC
