#include "mc_identity.h"
#include "nvs_flash.h"
#include "nvs.h"

#define MC_NVS_NS "purr_meshcore"

static mesh::LocalIdentity s_identity;
static bool s_loaded = false;

mesh::LocalIdentity& mc_identity_get(PurrRNG& rng) {
    if (s_loaded) return s_identity;

    nvs_handle_t h;
    bool have_saved = false;
    if (nvs_open(MC_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        // LocalIdentity's buffer readFrom() re-derives pub_key if only
        // prv_key is present (PRV_KEY_SIZE-length blob) — see
        // vendor/Identity.cpp:124-133 — so a PRV_KEY_SIZE-only blob is a
        // valid, smaller persisted form; we always write the full
        // prv+pub pair (see below) but accept either on read.
        uint8_t buf[PRV_KEY_SIZE + PUB_KEY_SIZE];
        size_t len = sizeof(buf);
        if (nvs_get_blob(h, "identity", buf, &len) == ESP_OK && (len == PRV_KEY_SIZE || len == PRV_KEY_SIZE + PUB_KEY_SIZE)) {
            s_identity.readFrom(buf, len);
            have_saved = true;
        }
        nvs_close(h);
    }

    if (!have_saved) {
        s_identity = mesh::LocalIdentity(&rng);

        uint8_t buf[PRV_KEY_SIZE + PUB_KEY_SIZE];
        size_t len = s_identity.writeTo(buf, sizeof(buf));
        if (nvs_open(MC_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_blob(h, "identity", buf, len);
            nvs_commit(h);
            nvs_close(h);
        }
    }

    s_loaded = true;
    return s_identity;
}
