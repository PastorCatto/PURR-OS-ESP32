// mesh_radio.c — Meshtastic LONG_FAST radio preset + AES-128-CTR crypto.
// Ported from PURR-OS-0.11/CoreOS/system/kernel/modules/purr_mesh.cpp — the
// only real change is that this talks to the radio through
// purr_kernel_radio()'s catcall_radio_t instead of a bespoke lora_manager
// singleton, so it's radio-chip-agnostic (works with sx1262 or sx1276).

#include "sdkconfig.h"

// No external caller besides meshtastic_module.c — see mesh_router.c's
// matching comment. Empty translation unit when the feature is off.
#ifdef CONFIG_PURR_FEATURE_MESHTASTIC

#include "mesh_radio.h"
#include "../../kernel/core/purr_kernel.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
// mesh_radio_ecdh_shared()/generate_curve25519_keypair() below need direct
// X/Z-coordinate access on an mbedtls_ecp_point (there's no public "raw
// X25519 point" accessor in this mbedtls version — its fields are wrapped
// in MBEDTLS_PRIVATE() by default). Must be defined before any mbedtls
// header is included.
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include <mbedtls/aes.h>
#include <mbedtls/ccm.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/sha256.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <string.h>
#include <stdio.h>

uint32_t mesh_radio_freq_for_region(const char *region)
{
    if (region && strcmp(region, "EU") == 0) return 869525000UL;
    if (region && strcmp(region, "JP") == 0) return 923875000UL;
    if (region && strcmp(region, "CN") == 0) return 470000000UL;
    if (region && strcmp(region, "AU") == 0) return 916875000UL;
    return 906875000UL;  // US default (also ANZ, TW, KR approximate)
}

// See mesh_radio_lock()'s doc comment in mesh_radio.h for why this exists.
static SemaphoreHandle_t s_radio_mutex = NULL;

void mesh_radio_lock(void)
{
    if (s_radio_mutex) xSemaphoreTake(s_radio_mutex, portMAX_DELAY);
}

void mesh_radio_unlock(void)
{
    if (s_radio_mutex) xSemaphoreGive(s_radio_mutex);
}

// ── Channels ─────────────────────────────────────────────────────────────────

#define MESH_NVS_NS "purr_mesh"

// Default channel PSK (Meshtastic "1" shorthand — the real 16-byte key used
// as-is). Meshtastic's own Channels.h calls this out explicitly: "16 bytes
// of random PSK for our _public_ default channel that all devices power up
// on (AES128)" — this was previously doubled into a fake 32-byte key and
// run through AES-256, which silently produced a completely different
// keystream from every real Meshtastic node (same bug on both encode and
// decode, so two PURR nodes could still talk to each other — just never to
// anything real).
static const uint8_t DEFAULT_PSK16[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01
};

static mesh_channel_t s_channels[MESH_MAX_CHANNELS];
static int            s_channel_count = 0;

uint8_t mesh_radio_channel_hash(const char *name, const uint8_t psk[16])
{
    uint8_t code = 0;
    for (size_t i = 0; name[i]; i++) code ^= (uint8_t)name[i];
    uint8_t psk_code = 0;
    for (size_t i = 0; i < 16; i++) psk_code ^= psk[i];
    return code ^ psk_code;
}

int mesh_radio_channel_count(void) { return s_channel_count; }

const mesh_channel_t *mesh_radio_channel_at(int idx)
{
    if (idx < 0 || idx >= s_channel_count) return NULL;
    return &s_channels[idx];
}

int mesh_radio_channel_find_by_hash(uint8_t hash)
{
    for (int i = 0; i < s_channel_count; i++) {
        if (s_channels[i].active && s_channels[i].hash == hash) return i;
    }
    return -1;
}

// mesh_radio_add_channel()/_remove_channel() are called from MeshChat's "Add
// Room"/"Forget Room" buttons — that app's own task is PSRAM-backed (see
// app_manager.c's launch_native(), only settings/fileman get a safe static
// internal-RAM stack), so neither can call save_custom_channels() directly
// (same PSRAM-stack-vs-NVS crash class as mesh_router.c's node table).
// Both just mark this dirty instead; the actual write happens on
// mesh_persist_task()'s internal-RAM stack (meshtastic_module.c).
static volatile bool s_channels_dirty = false;

static void save_custom_channels(void)
{
    nvs_handle_t h;
    if (nvs_open(MESH_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    // Channel 0 (LongFast) is never persisted — it's re-derived identically
    // every boot in mesh_radio_apply_preset(), only slots 1..count-1 (the
    // user-added ones) need saving.
    int custom_count = s_channel_count - 1;
    nvs_set_blob(h, "channels", &s_channels[1], sizeof(mesh_channel_t) * (size_t)custom_count);
    nvs_set_i32(h, "count", custom_count);
    nvs_commit(h);
    nvs_close(h);
}

void mesh_radio_flush_channels_if_dirty(void)
{
    if (!s_channels_dirty) return;
    s_channels_dirty = false;
    save_custom_channels();
}

static void load_custom_channels(void)
{
    nvs_handle_t h;
    if (nvs_open(MESH_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    int32_t custom_count = 0;
    nvs_get_i32(h, "count", &custom_count);
    if (custom_count > 0) {
        if (custom_count > MESH_MAX_CHANNELS - 1) custom_count = MESH_MAX_CHANNELS - 1;
        size_t blob_len = sizeof(mesh_channel_t) * (size_t)custom_count;
        if (nvs_get_blob(h, "channels", &s_channels[1], &blob_len) == ESP_OK) {
            s_channel_count += custom_count;
        }
    }
    nvs_close(h);
}

int mesh_radio_add_channel(const char *name, const uint8_t psk[16])
{
    if (s_channel_count >= MESH_MAX_CHANNELS) return -1;
    uint8_t hash = mesh_radio_channel_hash(name, psk);
    if (mesh_radio_channel_find_by_hash(hash) >= 0) return -1;  // hash collision

    mesh_channel_t *c = &s_channels[s_channel_count];
    memset(c, 0, sizeof(*c));
    strncpy(c->name, name, sizeof(c->name) - 1);
    memcpy(c->psk, psk, 16);
    c->hash   = hash;
    c->active = true;
    int idx = s_channel_count++;
    s_channels_dirty = true;
    return idx;
}

void mesh_radio_remove_channel(int idx)
{
    if (idx <= 0 || idx >= s_channel_count) return;   // idx==0 (LongFast) refused, matches doc comment
    memmove(&s_channels[idx], &s_channels[idx + 1],
            sizeof(mesh_channel_t) * (size_t)(s_channel_count - idx - 1));
    s_channel_count--;
    s_channels_dirty = true;
}

// Defined further down (needs generate_curve25519_keypair(), which needs
// to come after this point's simpler NVS helpers) — forward-declared here
// since mesh_radio_init() below calls it before its own definition.
static void ensure_identity(void);

// All NVS/flash access for this module happens here — called once from
// mesh_manager_init(), which runs on the kernel module loader's own task
// (an internal-RAM stack), *before* mesh_task() is created on its
// PSRAM-backed stack. This split exists because of a real, confirmed-live
// crash: ESP32-S3 asserts esp_task_stack_is_sane_cache_disabled() the
// moment a flash op (any NVS call) needs to disable the cache while the
// calling task's own stack lives in PSRAM (PSRAM access also needs that
// same cache) — mesh_task()'s whole call graph was deliberately audited to
// be NVS-free for exactly this reason (see mesh_manager_init()'s own
// comment), and the identity-keypair/channel-table NVS calls below used to
// live inside mesh_radio_apply_preset() (called from mesh_task() itself)
// until this was caught by a live boot-loop.
void mesh_radio_init(void)
{
    if (!s_radio_mutex) s_radio_mutex = xSemaphoreCreateMutex();

    if (s_channel_count == 0) {
        mesh_channel_t *primary = &s_channels[0];
        snprintf(primary->name, sizeof(primary->name), "LongFast");
        memcpy(primary->psk, DEFAULT_PSK16, 16);
        primary->hash   = mesh_radio_channel_hash(primary->name, primary->psk);
        primary->active = true;
        s_channel_count = 1;
        load_custom_channels();
    }

    ensure_identity();
}

bool mesh_radio_apply_preset(uint32_t freq_hz)
{
    const catcall_radio_t *radio = purr_kernel_radio();
    if (!radio) return false;

    if (radio->set_modulation) radio->set_modulation(MESH_SF, MESH_BW_HZ, MESH_CR);
    if (radio->set_sync_word)  radio->set_sync_word(MESH_SYNC_WORD);
    if (radio->set_frequency)  radio->set_frequency(freq_hz);
    if (radio->set_power)      radio->set_power(MESH_TX_DBM);
    return true;
}

void mesh_radio_build_iv(uint8_t iv[16], uint32_t pkt_id, uint32_t from_node)
{
    memset(iv, 0, 16);
    uint64_t id64 = pkt_id;
    memcpy(iv, &id64, 8);
    memcpy(iv + 8, &from_node, 4);
}

bool mesh_radio_aes_ctr(const uint8_t *in, uint8_t *out, size_t len,
                         const uint8_t key16[16], const uint8_t iv[16])
{
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    // AES-128, not AES-256 — see mesh_radio_default_psk()'s comment. The
    // channel's actual key length (16 bytes) is what selects the cipher in
    // real Meshtastic firmware (CryptoEngine::encryptAESCtr: 16 bytes ==
    // AES128, else AES256); this project only ever uses the 16-byte default
    // channel key today, so AES-128 is simply the correct, only case.
    if (mbedtls_aes_setkey_enc(&ctx, key16, 128) != 0) {
        mbedtls_aes_free(&ctx);
        return false;
    }
    uint8_t counter[16], stream[16];
    memcpy(counter, iv, 16);
    size_t nc_off = 0;
    int rc = mbedtls_aes_crypt_ctr(&ctx, len, &nc_off, counter, stream,
                                    (const uint8_t *)in, out);
    mbedtls_aes_free(&ctx);
    return rc == 0;
}

// ── PKI (direct-message end-to-end encryption) ─────────────────────────────────
// See mesh_radio.h's doc comment for the "why" — this section is the "how",
// matching real Meshtastic's CryptoEngine.cpp/aes-ccm.cpp byte-for-byte.

static uint8_t s_identity_priv[32];
static uint8_t s_identity_pub[32];
static bool    s_identity_loaded = false;

// RFC 7748's X25519 u-coordinate encoding is little-endian; mbedtls's
// mbedtls_ecp_point/mbedtls_mpi are curve-agnostic and for Curve25519
// specifically operate directly on that same little-endian convention once
// loaded via mbedtls_mpi_read/write_binary_le — no extra byte-swapping or
// TLS-style point-format framing needed at this level.
static bool generate_curve25519_keypair(uint8_t priv_out[32], uint8_t pub_out[32])
{
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point Q;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    static const char *pers = "purr_mesh_pki_keygen";
    bool ok = false;
    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                               (const unsigned char *)pers, strlen(pers)) != 0) goto done;
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) goto done;
    if (mbedtls_ecp_gen_keypair(&grp, &d, &Q, mbedtls_ctr_drbg_random, &ctr_drbg) != 0) goto done;
    if (mbedtls_mpi_write_binary_le(&d, priv_out, 32) != 0) goto done;
    if (mbedtls_mpi_write_binary_le(&Q.X, pub_out, 32) != 0) goto done;
    ok = true;

done:
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

// Generated once, ever — see mesh_radio.h's doc comment for why regenerating
// on every boot would break trust with every peer that's already heard our
// old public key.
static void ensure_identity(void)
{
    if (s_identity_loaded) return;

    nvs_handle_t h;
    bool have_keys = false;
    if (nvs_open(MESH_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t priv_len = 32, pub_len = 32;
        if (nvs_get_blob(h, "id_priv", s_identity_priv, &priv_len) == ESP_OK && priv_len == 32 &&
            nvs_get_blob(h, "id_pub",  s_identity_pub,  &pub_len)  == ESP_OK && pub_len  == 32) {
            have_keys = true;
        }
        nvs_close(h);
    }

    if (!have_keys && generate_curve25519_keypair(s_identity_priv, s_identity_pub)) {
        if (nvs_open(MESH_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_blob(h, "id_priv", s_identity_priv, 32);
            nvs_set_blob(h, "id_pub",  s_identity_pub,  32);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    s_identity_loaded = true;
}

void mesh_radio_identity_pubkey(uint8_t pub_out[32])
{
    ensure_identity();
    memcpy(pub_out, s_identity_pub, 32);
}

void mesh_radio_build_pki_nonce(uint8_t nonce[MESH_PKI_NONCE_LEN], uint32_t pkt_id, uint32_t extra_nonce, uint32_t from_node)
{
    memset(nonce, 0, MESH_PKI_NONCE_LEN);
    // See mesh_radio.h's doc comment for exactly why this specific layout
    // (not the more "obvious" [packetId 8 bytes][extraNonce 4 bytes]) is
    // what real Meshtastic's initNonce() actually produces.
    memcpy(nonce,     &pkt_id,      4);   // [0:4)  packetId
    memcpy(nonce + 4, &extra_nonce, 4);   // [4:8)  extraNonce
    memcpy(nonce + 8, &from_node,   4);   // [8:12) fromNode
    // byte 12 stays 0 — matches CryptoEngine::initNonce()'s 16-byte buffer
    // truncated to the 13 bytes CCM actually uses.
}

bool mesh_radio_ecdh_shared(const uint8_t their_pub[32], uint8_t shared_out[32])
{
    ensure_identity();

    mbedtls_ecp_group grp;
    mbedtls_mpi d, shared;
    mbedtls_ecp_point Q;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&shared);
    mbedtls_ecp_point_init(&Q);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    bool ok = false;
    uint8_t raw_shared[32];
    // This mbedtls version's mbedtls_ecp_mul_restartable() (called inside
    // mbedtls_ecdh_compute_shared()) unconditionally rejects a NULL f_rng
    // with MBEDTLS_ERR_ECP_BAD_INPUT_DATA for every curve, including
    // Curve25519 — confirmed by reading ecp.c directly, contrary to older
    // assumptions that Montgomery curves don't need one. X25519 itself is
    // still fully deterministic (this RNG only feeds mbedtls's internal
    // scalar-blinding side-channel countermeasure, it doesn't change the
    // resulting shared secret) — a fresh seed per call is fine. Confirmed
    // live: every single ECDH call failed 100% of the time (both real
    // inbound PKI messages and 20/20 synthetic self-test trials) until this
    // was seeded.
    static const char *pers = "purr_mesh_ecdh";
    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                               (const unsigned char *)pers, strlen(pers)) != 0) goto done;
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) goto done;
    if (mbedtls_mpi_read_binary_le(&d, s_identity_priv, 32) != 0) goto done;
    // Curve25519 points in mbedtls are projective (X, Z) — Y is unused for
    // the Montgomery x25519 ladder. Z=1 marks this as an affine input point.
    if (mbedtls_mpi_read_binary_le(&Q.X, their_pub, 32) != 0) goto done;
    if (mbedtls_mpi_lset(&Q.Z, 1) != 0) goto done;
    if (mbedtls_ecdh_compute_shared(&grp, &shared, &Q, &d, mbedtls_ctr_drbg_random, &ctr_drbg) != 0) goto done;
    if (mbedtls_mpi_write_binary_le(&shared, raw_shared, 32) != 0) goto done;

    // Real Meshtastic hashes the raw ECDH output before using it as the
    // AES key (CryptoEngine::hash(), SHA-256) rather than using the raw
    // shared secret directly.
    if (mbedtls_sha256(raw_shared, 32, shared_out, 0) != 0) goto done;
    ok = true;

done:
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&shared);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

bool mesh_radio_aes_ccm_encrypt(const uint8_t *in, uint8_t *out, size_t len,
                                 const uint8_t key32[32], const uint8_t nonce[MESH_PKI_NONCE_LEN],
                                 uint8_t tag_out[MESH_PKI_TAG_LEN])
{
    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    if (mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key32, 256) != 0) {
        mbedtls_ccm_free(&ctx);
        return false;
    }
    int rc = mbedtls_ccm_encrypt_and_tag(&ctx, len, nonce, MESH_PKI_NONCE_LEN,
                                          NULL, 0, in, out, tag_out, MESH_PKI_TAG_LEN);
    mbedtls_ccm_free(&ctx);
    return rc == 0;
}

bool mesh_radio_aes_ccm_decrypt(const uint8_t *in, uint8_t *out, size_t len,
                                 const uint8_t key32[32], const uint8_t nonce[MESH_PKI_NONCE_LEN],
                                 const uint8_t tag_in[MESH_PKI_TAG_LEN])
{
    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    if (mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key32, 256) != 0) {
        mbedtls_ccm_free(&ctx);
        return false;
    }
    int rc = mbedtls_ccm_auth_decrypt(&ctx, len, nonce, MESH_PKI_NONCE_LEN,
                                       NULL, 0, in, out, tag_in, MESH_PKI_TAG_LEN);
    mbedtls_ccm_free(&ctx);
    return rc == 0;
}

#endif  // CONFIG_PURR_FEATURE_MESHTASTIC
