#pragma once
// NVS-backed persistence for MeshCore's LocalIdentity (Ed25519 keypair).
// Mirrors mesh_radio.c's ensure_identity() pattern exactly: generated once,
// synchronously, on first boot with no persisted identity; every later
// boot loads the same keypair. Must be called from mc_manager_init()
// (internal-RAM module-loader task), never from a PSRAM-backed task — see
// mc_contacts.h's file header for why.

#include <Identity.h>
#include "mc_radio_adapter.h"

// Returns the persisted identity, generating and saving a new one on first
// use. 'rng' is only consulted if no identity exists yet.
mesh::LocalIdentity& mc_identity_get(PurrRNG& rng);
