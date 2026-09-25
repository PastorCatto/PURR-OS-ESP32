#pragma once
// Contact (peer) table and group-channel table for MeshCore. Mesh has no
// built-in persistence for either — confirmed by reading Mesh.h/.cpp
// directly (searchPeersByHash()/getPeerSharedSecret()/searchChannelsByHash()
// all default to "not found", entirely the integrator's design) — same
// shape as mesh_router.c's own node table for Meshtastic, built fresh
// here rather than reused (wire-incompatible protocols, no shared struct).
//
// NVS-persisted with the same dirty-flag + dedicated persist-task pattern
// as mesh_router.c: runtime mutators only set a flag, a low-priority
// internal-RAM-stack task (owned by meshcore_module.cpp) is the only thing
// that actually touches flash — required because NVS writes must never
// happen from a task whose own stack lives in PSRAM (ESP32-S3 cache-disable
// assert), see mesh_radio.h's documented crash class.

#include <stdint.h>
#include <stddef.h>
#include <MeshCore.h>
#include <Mesh.h>  // GroupChannel is declared here, not Dispatcher.h

#define MC_MAX_CONTACTS  16
#define MC_MAX_CHANNELS  8
#define MC_CONTACT_NAME_LEN 32
#define MC_CHANNEL_NAME_LEN 32

struct mc_contact_t {
    bool     used;
    uint8_t  pub_key[PUB_KEY_SIZE];
    uint8_t  path[MAX_PATH_SIZE];
    uint8_t  path_len;      // MeshCore path_len encoding (0 = no known path yet)
    bool     has_path;
    char     name[MC_CONTACT_NAME_LEN];
    uint32_t last_advert_timestamp;
    uint32_t last_seen_ms;
};

struct mc_channel_t {
    bool     used;
    char     name[MC_CHANNEL_NAME_LEN];
    uint8_t  hash[PATH_HASH_SIZE];
    uint8_t  secret[PUB_KEY_SIZE];
};

// Loads persisted contacts/channels from NVS. Must be called synchronously
// during mc_manager_init(), before the mesh task starts (see file header).
void mc_contacts_load(void);
void mc_channels_load(void);

// Only the persist task (internal-RAM stack) should call these.
void mc_contacts_flush_if_dirty(void);
void mc_channels_flush_if_dirty(void);

int mc_contacts_count(void);
const mc_contact_t* mc_contacts_at(int idx);

// Finds every contact whose pub_key's leading hash_len bytes match 'hash'.
// Fills out_indices (caller-owned, up to max_matches) and returns the count
// — mirrors Mesh::onRecvPacket()'s "try each matching contact until one
// decrypts successfully" pattern (mesh_router.c uses the identical idea
// for Meshtastic node lookups).
int mc_contacts_find_by_hash(const uint8_t* hash, uint8_t hash_len, int* out_indices, int max_matches);

// Inserts a new contact or updates an existing one's name/timestamp.
// Returns the contact's index, or -1 if the table is full and no matching
// entry existed to update.
int mc_contacts_upsert(const uint8_t* pub_key, const char* name, uint32_t timestamp);

void mc_contacts_set_path(int idx, const uint8_t* path, uint8_t path_len);

// Removes a contact — "Forget" in MSN's Manage screen. Mirrors
// mesh_router_node_forget()'s shift-down-and-mark-dirty shape.
void mc_contacts_forget(int idx);

// Group channels — 'secret' is the pre-shared symmetric key, derived from
// a passphrase via SHA-256 the same way meshchat.c derives Meshtastic
// channel PSKs (mesh_channel_t's own pattern), not part of the MeshCore
// wire format itself.
int mc_channels_count(void);
const mc_channel_t* mc_channels_at(int idx);
int mc_channels_find_by_hash(const uint8_t* hash, uint8_t hash_len, mesh::GroupChannel out[], int max_matches);
int mc_channels_add(const char* name, const uint8_t* secret);

// Removes a channel — "Forget" in MSN's Manage screen. Mirrors
// mesh_radio_remove_channel()'s shift-down-and-mark-dirty shape (no
// "index 0 is protected" rule here — MeshCore has no fixed default
// channel the way Meshtastic's LongFast is).
void mc_channels_remove(int idx);
