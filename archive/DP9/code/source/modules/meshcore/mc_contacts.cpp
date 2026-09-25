#include "mc_contacts.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"

#define MC_NVS_NS "purr_meshcore"

static mc_contact_t s_contacts[MC_MAX_CONTACTS];
static int s_contact_count = 0;
static volatile bool s_contacts_dirty = false;

static mc_channel_t s_channels[MC_MAX_CHANNELS];
static int s_channel_count = 0;
static volatile bool s_channels_dirty = false;

// ── contacts ────────────────────────────────────────────────────────────

void mc_contacts_load(void) {
    nvs_handle_t h;
    if (nvs_open(MC_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    int32_t count = 0;
    nvs_get_i32(h, "c_count", &count);
    if (count > 0) {
        if (count > MC_MAX_CONTACTS) count = MC_MAX_CONTACTS;
        size_t blob_len = sizeof(mc_contact_t) * (size_t)count;
        if (nvs_get_blob(h, "contacts", s_contacts, &blob_len) == ESP_OK) {
            s_contact_count = count;
            for (int i = 0; i < s_contact_count; i++) {
                s_contacts[i].last_seen_ms = 0;  // stale the moment it's loaded
            }
        }
    }
    nvs_close(h);
}

static void save_contacts(void) {
    nvs_handle_t h;
    if (nvs_open(MC_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "contacts", s_contacts, sizeof(mc_contact_t) * (size_t)s_contact_count);
    nvs_set_i32(h, "c_count", s_contact_count);
    nvs_commit(h);
    nvs_close(h);
}

void mc_contacts_flush_if_dirty(void) {
    if (!s_contacts_dirty) return;
    s_contacts_dirty = false;
    save_contacts();
}

int mc_contacts_count(void) {
    return s_contact_count;
}

const mc_contact_t* mc_contacts_at(int idx) {
    if (idx < 0 || idx >= s_contact_count || !s_contacts[idx].used) return nullptr;
    return &s_contacts[idx];
}

int mc_contacts_find_by_hash(const uint8_t* hash, uint8_t hash_len, int* out_indices, int max_matches) {
    int n = 0;
    for (int i = 0; i < s_contact_count && n < max_matches; i++) {
        if (s_contacts[i].used && memcmp(s_contacts[i].pub_key, hash, hash_len) == 0) {
            out_indices[n++] = i;
        }
    }
    return n;
}

int mc_contacts_upsert(const uint8_t* pub_key, const char* name, uint32_t timestamp) {
    for (int i = 0; i < s_contact_count; i++) {
        if (s_contacts[i].used && memcmp(s_contacts[i].pub_key, pub_key, PUB_KEY_SIZE) == 0) {
            if (timestamp > s_contacts[i].last_advert_timestamp) {
                if (name && name[0]) {
                    strncpy(s_contacts[i].name, name, MC_CONTACT_NAME_LEN - 1);
                    s_contacts[i].name[MC_CONTACT_NAME_LEN - 1] = 0;
                }
                s_contacts[i].last_advert_timestamp = timestamp;
                s_contacts_dirty = true;
            }
            return i;
        }
    }
    if (s_contact_count >= MC_MAX_CONTACTS) return -1;  // table full

    int idx = s_contact_count++;
    memset(&s_contacts[idx], 0, sizeof(mc_contact_t));
    s_contacts[idx].used = true;
    memcpy(s_contacts[idx].pub_key, pub_key, PUB_KEY_SIZE);
    if (name && name[0]) {
        strncpy(s_contacts[idx].name, name, MC_CONTACT_NAME_LEN - 1);
    }
    s_contacts[idx].last_advert_timestamp = timestamp;
    s_contacts_dirty = true;
    return idx;
}

void mc_contacts_set_path(int idx, const uint8_t* path, uint8_t path_len) {
    if (idx < 0 || idx >= s_contact_count || !s_contacts[idx].used) return;
    uint8_t byte_len = (path_len & 63) * ((path_len >> 6) + 1);
    if (byte_len > MAX_PATH_SIZE) return;
    memcpy(s_contacts[idx].path, path, byte_len);
    s_contacts[idx].path_len = path_len;
    s_contacts[idx].has_path = true;
    s_contacts_dirty = true;
}

void mc_contacts_forget(int idx) {
    if (idx < 0 || idx >= s_contact_count) return;
    memmove(&s_contacts[idx], &s_contacts[idx + 1],
            sizeof(mc_contact_t) * (size_t)(s_contact_count - idx - 1));
    s_contact_count--;
    s_contacts_dirty = true;
}

// ── channels ────────────────────────────────────────────────────────────

void mc_channels_load(void) {
    nvs_handle_t h;
    if (nvs_open(MC_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    int32_t count = 0;
    nvs_get_i32(h, "ch_count", &count);
    if (count > 0) {
        if (count > MC_MAX_CHANNELS) count = MC_MAX_CHANNELS;
        size_t blob_len = sizeof(mc_channel_t) * (size_t)count;
        if (nvs_get_blob(h, "channels", s_channels, &blob_len) == ESP_OK) {
            s_channel_count = count;
        }
    }
    nvs_close(h);
}

static void save_channels(void) {
    nvs_handle_t h;
    if (nvs_open(MC_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "channels", s_channels, sizeof(mc_channel_t) * (size_t)s_channel_count);
    nvs_set_i32(h, "ch_count", s_channel_count);
    nvs_commit(h);
    nvs_close(h);
}

void mc_channels_flush_if_dirty(void) {
    if (!s_channels_dirty) return;
    s_channels_dirty = false;
    save_channels();
}

int mc_channels_count(void) {
    return s_channel_count;
}

const mc_channel_t* mc_channels_at(int idx) {
    if (idx < 0 || idx >= s_channel_count || !s_channels[idx].used) return nullptr;
    return &s_channels[idx];
}

int mc_channels_find_by_hash(const uint8_t* hash, uint8_t hash_len, mesh::GroupChannel out[], int max_matches) {
    int n = 0;
    for (int i = 0; i < s_channel_count && n < max_matches; i++) {
        if (s_channels[i].used && memcmp(s_channels[i].hash, hash, hash_len) == 0) {
            memcpy(out[n].hash, s_channels[i].hash, PATH_HASH_SIZE);
            memcpy(out[n].secret, s_channels[i].secret, PUB_KEY_SIZE);
            n++;
        }
    }
    return n;
}

int mc_channels_add(const char* name, const uint8_t* secret) {
    if (s_channel_count >= MC_MAX_CHANNELS) return -1;

    int idx = s_channel_count++;
    memset(&s_channels[idx], 0, sizeof(mc_channel_t));
    s_channels[idx].used = true;
    strncpy(s_channels[idx].name, name, MC_CHANNEL_NAME_LEN - 1);
    memcpy(s_channels[idx].secret, secret, PUB_KEY_SIZE);
    // Confirmed against upstream's real BaseChatMesh::addChannel()
    // (src/helpers/BaseChatMesh.cpp): channel hash is SHA-256(secret),
    // truncated to PATH_HASH_SIZE — NOT a raw prefix of the secret bytes.
    // Getting this wrong would silently break channel message matching on
    // real hardware, so this was checked against the actual source rather
    // than assumed.
    mesh::Utils::sha256(s_channels[idx].hash, PATH_HASH_SIZE, secret, PUB_KEY_SIZE);
    s_channels_dirty = true;
    return idx;
}

void mc_channels_remove(int idx) {
    if (idx < 0 || idx >= s_channel_count) return;
    memmove(&s_channels[idx], &s_channels[idx + 1],
            sizeof(mc_channel_t) * (size_t)(s_channel_count - idx - 1));
    s_channel_count--;
    s_channels_dirty = true;
}
