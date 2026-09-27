#pragma once
// mesh_radio.h — Meshtastic LONG_FAST radio preset + AES-128-CTR crypto.
// Internal to the meshtastic module — not a public API, see meshtastic.h.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// LONG_FAST preset — matches Meshtastic's default channel modulation exactly.
#define MESH_SF         11
#define MESH_BW_HZ      250000
#define MESH_CR         5           // 4/5
#define MESH_SYNC_WORD  0x2B
#define MESH_TX_DBM     20
#define MESH_HOP_LIMIT  3
#define MESH_BROADCAST  0xFFFFFFFFUL

// LONG_FAST channel 0 frequency by region ("US" default, "EU", "JP", "CN", "AU").
uint32_t mesh_radio_freq_for_region(const char *region);

// One-time setup: creates the radio mutex, populates the channel table
// (channel 0 fixed LongFast + any saved custom channels loaded from NVS),
// and loads/generates this node's PKI identity keypair. Must be called
// from mesh_manager_init() — i.e. on the kernel module loader's own task,
// *before* mesh_task() is created — never from mesh_task() itself or
// anything it calls. mesh_task() runs on a PSRAM-backed stack, and every
// one of these steps touches NVS/flash; ESP32-S3 asserts
// esp_task_stack_is_sane_cache_disabled() the instant a flash op needs to
// disable the cache while the calling task's own stack lives in PSRAM
// (confirmed live as a boot-loop crash before this split existed).
void mesh_radio_init(void);

// Retunes the already-registered radio catcall to the LONG_FAST preset at
// the given frequency — safe to call from mesh_task(), touches only the
// radio catcall (set_modulation/set_sync_word/set_frequency/set_power),
// no NVS/flash access. Does NOT call catcall_radio_t.init() itself — the
// radio driver's own module registration already did that with its
// default config. Returns false if no radio catcall is registered.
bool mesh_radio_apply_preset(uint32_t freq_hz);

// IV layout: [packet_id as uint64_t LE (8 bytes)][from_node LE (4 bytes)][0x00 * 4]
// Matches Meshtastic's CryptoEngine::initNonce() byte-for-byte.
void mesh_radio_build_iv(uint8_t iv[16], uint32_t pkt_id, uint32_t from_node);

bool mesh_radio_aes_ctr(const uint8_t *in, uint8_t *out, size_t len,
                         const uint8_t key16[16], const uint8_t iv[16]);

// ── Channels ─────────────────────────────────────────────────────────────────
// Real Meshtastic supports up to 8 channels (index 0 = primary), each with
// its own name+PSK, selected on the wire by a single hash byte
// (PacketHeader.channel — see mesh_router.c's top comment). Channel 0 is
// always "LongFast" (name="LongFast", the real default 16-byte PSK) —
// fixed at mesh_radio_apply_preset()'s one-time setup, matching this
// project's whole prior single-channel behavior exactly, so every existing
// caller that never thinks about channels keeps working unchanged.
#define MESH_MAX_CHANNELS 8

typedef struct {
    char    name[12];
    uint8_t psk[16];
    uint8_t hash;
    bool    active;
} mesh_channel_t;

// xorHash(name) ^ xorHash(psk) — Channels::generateHash()'s exact algorithm
// (byte-XOR over each buffer, then XOR the two results together). Confirmed
// against real Meshtastic source; this is the same formula that was
// previously only ever hand-computed once for the LongFast default (0x08).
uint8_t mesh_radio_channel_hash(const char *name, const uint8_t psk[16]);

// Adds a channel (name copied/truncated to 11 chars + NUL, psk copied
// verbatim). Returns the new index, or -1 if all 8 slots are full or the
// computed hash collides with an existing active channel (two channels
// with the same on-air hash would be indistinguishable on decode).
// Persists to NVS immediately (namespace "purr_mesh") so it survives reboot.
int mesh_radio_add_channel(const char *name, const uint8_t psk[16]);

// Removes a channel (shift-down + re-save to NVS) — used by MeshChat's
// "Forget" action. Refuses idx==0: the fixed LongFast primary can't be
// removed, every device speaks it out of the box. No-op if idx is out of
// range.
void mesh_radio_remove_channel(int idx);

// Both mesh_radio_add_channel()/_remove_channel() above are called from
// MeshChat's own PSRAM-backed app task, so neither writes to NVS directly —
// they just mark the channel table dirty. This performs the actual
// deferred write, and MUST be called periodically from a task with an
// internal-RAM stack (see meshtastic_module.c's mesh_persist_task()).
// No-op if nothing changed since the last call.
void mesh_radio_flush_channels_if_dirty(void);

int                   mesh_radio_channel_count(void);
const mesh_channel_t *mesh_radio_channel_at(int idx);

// Finds the (first) active channel whose hash matches a received packet's
// channel byte, or -1 if none of our known channels match — used by
// mesh_router_decode() to pick which PSK to try.
int mesh_radio_channel_find_by_hash(uint8_t hash);

// ── PKI (direct-message end-to-end encryption) ────────────────────────────────
// Modern Meshtastic clients (v2.5+, confirmed against the official iOS app)
// always encrypt direct messages with the recipient's Curve25519 public
// key, never the channel PSK — a DM to a node with no known public key
// fails locally on the sender's end before any radio transmission happens
// at all. Wire format, nonce construction, and cipher choice (AES-256-CCM,
// not the channel path's plain CTR) confirmed against real Meshtastic's
// CryptoEngine.cpp/aes-ccm.cpp; see mesh_router.c's PKI encode/decode
// comments for the exact byte layout.
#define MESH_PKI_TAG_LEN    8
#define MESH_PKI_NONCE_LEN  13
#define MESH_PKI_OVERHEAD   12   // 8-byte tag + 4-byte extraNonce trailer

// This node's persistent Curve25519 identity keypair — generated once,
// ever, on first access, and persisted to NVS. Never regenerated: every
// peer that has already heard our public key would otherwise start
// treating us as a stranger with a "changed key" warning on every reboot.
void mesh_radio_identity_pubkey(uint8_t pub_out[32]);

// PKI's nonce differs from mesh_radio_build_iv()'s channel-path IV in two
// ways: it's 13 bytes not 16 (CCM's N = 15-L, L=2), and the extraNonce
// overwrites bytes [4:8) — NOT appended after packetId's full 8 bytes, and
// NOT replacing from_node. Confirmed byte-for-byte against real
// CryptoEngine::initNonce() (github.com/meshtastic/firmware): it first
// does `memcpy(nonce, &packetId, 8)` (packetId is a uint64_t, but the wire
// packet id is only 32 bits zero-extended, so bytes [4:8) start as zero),
// `memcpy(nonce+8, &fromNode, 4)`, THEN — critically — a *third* memcpy at
// `nonce + sizeof(uint32_t)` i.e. offset 4 (not offset 8) writes extraNonce
// there, clobbering packetId's zero upper half. Net layout: [0:4)=packetId,
// [4:8)=extraNonce, [8:12)=fromNode, [12:16)=0. An earlier version of this
// function put extraNonce at [8:12) and dropped from_node entirely — wrong
// on both counts, confirmed live as the cause of every inbound PKI message
// failing AES-CCM tag verification despite ECDH succeeding.
void mesh_radio_build_pki_nonce(uint8_t nonce[MESH_PKI_NONCE_LEN], uint32_t pkt_id, uint32_t extra_nonce, uint32_t from_node);

// Raw X25519 scalar multiplication (our private key * their public key,
// RFC 7748) followed by SHA-256 of the raw shared secret — matches real
// Meshtastic's CryptoEngine::setDHPublicKey()+hash() exactly. Returns
// false on a crypto failure (should not happen in practice).
bool mesh_radio_ecdh_shared(const uint8_t their_pub[32], uint8_t shared_out[32]);

// AES-256-CCM, tag=8 bytes, nonce=13 bytes (matching MESH_PKI_TAG_LEN/
// NONCE_LEN above), no associated data. `tag_out`/`tag_in` are always
// exactly MESH_PKI_TAG_LEN bytes.
bool mesh_radio_aes_ccm_encrypt(const uint8_t *in, uint8_t *out, size_t len,
                                 const uint8_t key32[32], const uint8_t nonce[MESH_PKI_NONCE_LEN],
                                 uint8_t tag_out[MESH_PKI_TAG_LEN]);
bool mesh_radio_aes_ccm_decrypt(const uint8_t *in, uint8_t *out, size_t len,
                                 const uint8_t key32[32], const uint8_t nonce[MESH_PKI_NONCE_LEN],
                                 const uint8_t tag_in[MESH_PKI_TAG_LEN]);

// Guards every catcall_radio_t call (send/receive/data_available/rssi/snr)
// against concurrent access. RadioLib has no locking of its own, and this
// module has two independent callers of the radio: mesh_task()'s own RX-
// poll loop (every 10ms, meshtastic_module.c) and anything that sends —
// mesh_manager_send_text() (called directly from a UI button press, i.e. a
// different task entirely) and mesh_router_relay()'s own send (called from
// mesh_task(), but still needs the same lock since it's racing the *other*
// caller). An interleaved send/receive corrupts RadioLib's internal mode-
// tracking state mid-sequence — confirmed live as the root cause of
// intermittent crashes specifically when sending a message while RX
// polling is concurrently running (which it always is). The mutex is
// created once inside mesh_radio_apply_preset() (called once at mesh_task
// startup, before s_ready unblocks any other caller) — every other radio
// catcall call site in this module must be wrapped in these.
void mesh_radio_lock(void);
void mesh_radio_unlock(void);
