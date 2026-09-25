// user_mgr.c — PURR OS multi-user accounts. See user_mgr.h for the full
// design (credential model, why SHA-256 over a slow KDF, the local/remote
// account-type split, session-not-persisted-across-reboot).

#include "user_mgr.h"
#include "purr_module.h"
#include "esp_random.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG = "user_mgr";
// Bumped from "purr_users" — user_record_t grew an is_admin field (remote-
// login work, see user_mgr_is_admin()'s own doc comment). Per this file's
// own policy just below ("a shape change just means a fresh NVS
// namespace"): old records are sizeof(user_record_t) bytes and load_all()
// requires an exact length match, so they'd otherwise fail that check and
// get silently skipped as "corrupt" rather than cleanly reset. Bumping the
// namespace makes that a clean, obvious bootstrap-fresh instead.
#define NVS_NS       "purr_users_v2"
#define NVS_KEY_COUNT  "count"
#define NVS_KEY_ACTIVE "active"
#define NVS_KEY_OOBE   "oobe_done"

// On-disk (and in-memory) record. Packed for a stable, minimal NVS blob
// size — this is local device state, not something read by another
// version of this code or another device, so no version byte: a shape
// change just means a fresh NVS namespace, same as any other purr_* store
// in this codebase when its layout changes.
typedef struct __attribute__((packed)) {
    char    name[USER_MGR_USERNAME_MAX];
    uint8_t type;           // user_account_type_t
    uint8_t has_password;   // 0/1 — LOCAL only; always 0 for REMOTE
    uint8_t salt[16];
    uint8_t hash[32];       // SHA-256(salt || password)
    uint8_t is_admin;       // 0/1 — see user_mgr_is_admin()'s own doc comment
    // Which server this account lives on — REMOTE only, all-zero/unused
    // for LOCAL. Needed so a later boot/relock can actually attempt
    // reconnecting a remote identity (pairing_verify_user(mac, name))
    // instead of just remembering the name with nowhere to send it — see
    // user_mgr_get_remote_mac()/user_mgr_create_remote()'s own comments.
    uint8_t mac[6];
    // Opt-in only, REMOTE accounts only (unused/0 for LOCAL) — allows a
    // "Continue offline" fallback when this identity's server can't be
    // reached (systemui_login.c's begin_remote_reconnect() failure
    // path). Off by default: caching enough state to act "logged in"
    // without the server reachable is a real trust tradeoff, not
    // something to default on. See user_mgr_set_offline_access()'s own
    // doc comment.
    uint8_t offline_access;
} user_record_t;

static user_record_t s_users[USER_MGR_MAX_USERS];
static int           s_user_count = 0;

static char s_current_user[USER_MGR_USERNAME_MAX] = "";   // "" = not logged in
static char s_default_buf[USER_MGR_USERNAME_MAX]  = "";   // scratch for user_mgr_default_username()

// ── Username validation ──────────────────────────────────────────────────────

bool user_mgr_valid_username(const char *username) {
    if (!username) return false;
    size_t len = strlen(username);
    if (len < 1 || len >= USER_MGR_USERNAME_MAX) return false;
    if (!(username[0] >= 'a' && username[0] <= 'z')) return false;
    for (size_t i = 1; i < len; i++) {
        char c = username[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

// ── Credential hashing ───────────────────────────────────────────────────────

static void hash_password(const uint8_t salt[16], const char *password, uint8_t out[32]) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);   // 0 = SHA-256, not SHA-224
    mbedtls_sha256_update(&ctx, salt, 16);
    if (password && password[0]) {
        mbedtls_sha256_update(&ctx, (const uint8_t *)password, strlen(password));
    }
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

// Public wrapper — same construction, exposed so remote-login code (the
// paired-device client side, source/modules/pairing/) computes the
// identical hash a real LOCAL account on this SAME firmware would, instead
// of a second hand-rolled copy of "SHA-256(salt || password)" drifting
// out of sync with this one over time.
void user_mgr_hash_password(const uint8_t salt[16], const char *password, uint8_t out[32]) {
    hash_password(salt, password, out);
}

// Same cost regardless of where the first differing byte falls, so a wrong
// guess and a right one are not distinguishable by timing.
static bool constant_time_eq(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

// ── NVS persistence ───────────────────────────────────────────────────────────
// Whole-table rewrite on any mutation rather than delta updates — at
// USER_MGR_MAX_USERS (8) records of ~82 bytes each, this is a handful of
// small NVS writes, not worth the bug surface of incremental add/remove/
// compact logic against flash. Trailing slots beyond `count` are simply
// never read back, not explicitly erased — stale bytes there are inert.

static void save_all(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_COUNT, (uint8_t)s_user_count);
    char key[12];   // "u" + up to 10 digits (int worst case) + NUL — see this file's
                    // own -Werror=format-truncation note; USER_MGR_MAX_USERS keeps i
                    // single-digit in practice, this just satisfies GCC's conservative bound
    for (int i = 0; i < s_user_count; i++) {
        snprintf(key, sizeof(key), "u%d", i);
        nvs_set_blob(h, key, &s_users[i], sizeof(user_record_t));
    }
    nvs_commit(h);
    nvs_close(h);
}

static void load_all(void) {
    s_user_count = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;   // namespace absent — first boot

    uint8_t count = 0;
    if (nvs_get_u8(h, NVS_KEY_COUNT, &count) != ESP_OK) { nvs_close(h); return; }
    if (count > USER_MGR_MAX_USERS) count = USER_MGR_MAX_USERS;

    char key[12];   // "u" + up to 10 digits (int worst case) + NUL — see this file's
                    // own -Werror=format-truncation note; USER_MGR_MAX_USERS keeps i
                    // single-digit in practice, this just satisfies GCC's conservative bound
    for (int i = 0; i < count; i++) {
        snprintf(key, sizeof(key), "u%d", i);
        size_t len = sizeof(user_record_t);
        if (nvs_get_blob(h, key, &s_users[s_user_count], &len) == ESP_OK &&
            len == sizeof(user_record_t)) {
            s_user_count++;
        } else {
            ESP_LOGW(TAG, "skipping unreadable/corrupt record at index %d", i);
        }
    }
    nvs_close(h);
}

static void save_active(const char *username) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_ACTIVE, username);
    nvs_commit(h);
    nvs_close(h);
}

// Clears the persisted "active" username — see user_mgr_logout()'s own
// comment on why this matters specifically for a REMOTE account: an empty
// string makes user_mgr_default_username()'s user_mgr_exists() check fail
// (nothing named "" exists), falling through to USER_MGR_BOOTSTRAP_USER
// instead of nvs_erase_key()'s NOT_FOUND-handling dance for the same
// effect — simpler, and reuses a check that already exists.
static void clear_active(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_ACTIVE, "");
    nvs_commit(h);
    nvs_close(h);
}

// ── Lookup ────────────────────────────────────────────────────────────────────

static int find_index(const char *username) {
    if (!username) return -1;
    for (int i = 0; i < s_user_count; i++) {
        if (strncmp(s_users[i].name, username, USER_MGR_USERNAME_MAX) == 0) return i;
    }
    return -1;
}

// ── Public API ────────────────────────────────────────────────────────────────

int user_mgr_count(void) { return s_user_count; }

bool user_mgr_at(int idx, char *name_out, size_t name_out_sz) {
    if (idx < 0 || idx >= s_user_count || !name_out || name_out_sz == 0) return false;
    snprintf(name_out, name_out_sz, "%s", s_users[idx].name);
    return true;
}

bool user_mgr_exists(const char *username) { return find_index(username) >= 0; }

user_account_type_t user_mgr_account_type(const char *username) {
    int idx = find_index(username);
    return idx >= 0 ? (user_account_type_t)s_users[idx].type : USER_ACCOUNT_LOCAL;
}

// False for an unknown username — never treats "doesn't exist" as admin.
bool user_mgr_is_admin(const char *username) {
    int idx = find_index(username);
    return idx >= 0 && s_users[idx].is_admin != 0;
}

bool user_mgr_set_admin(const char *username, bool is_admin) {
    int idx = find_index(username);
    if (idx < 0) return false;
    s_users[idx].is_admin = is_admin ? 1 : 0;
    save_all();
    ESP_LOGI(TAG, "'%s' admin = %d", username, (int)s_users[idx].is_admin);
    return true;
}

bool user_mgr_create(const char *username, const char *password) {
    if (!user_mgr_valid_username(username)) return false;
    if (user_mgr_exists(username)) return false;
    if (s_user_count >= USER_MGR_MAX_USERS) {
        ESP_LOGW(TAG, "cannot create '%s' — USER_MGR_MAX_USERS (%d) reached",
                 username, USER_MGR_MAX_USERS);
        return false;
    }

    user_record_t *u = &s_users[s_user_count];
    memset(u, 0, sizeof(*u));
    snprintf(u->name, sizeof(u->name), "%s", username);
    u->type = USER_ACCOUNT_LOCAL;
    if (password && password[0]) {
        esp_fill_random(u->salt, sizeof(u->salt));
        hash_password(u->salt, password, u->hash);
        u->has_password = 1;
    } else {
        u->has_password = 0;   // salt/hash stay zeroed — unused for a no-password account
    }
    s_user_count++;
    save_all();
    ESP_LOGI(TAG, "created user '%s' (%s)", username, u->has_password ? "password set" : "no password");
    return true;
}

bool user_mgr_remove(const char *username) {
    int idx = find_index(username);
    if (idx < 0) return false;

    if (strncmp(s_current_user, username, sizeof(s_current_user)) == 0) {
        user_mgr_logout();
    }

    // Compact: shift everything after idx down one slot.
    for (int i = idx; i < s_user_count - 1; i++) {
        s_users[i] = s_users[i + 1];
    }
    s_user_count--;
    save_all();
    ESP_LOGI(TAG, "removed user '%s'", username);
    return true;
}

bool user_mgr_set_password(const char *username, const char *password) {
    int idx = find_index(username);
    if (idx < 0) return false;
    if (s_users[idx].type != USER_ACCOUNT_LOCAL) {
        ESP_LOGW(TAG, "'%s' is a remote account — no local credential to change", username);
        return false;
    }

    user_record_t *u = &s_users[idx];
    if (password && password[0]) {
        esp_fill_random(u->salt, sizeof(u->salt));
        hash_password(u->salt, password, u->hash);
        u->has_password = 1;
    } else {
        memset(u->salt, 0, sizeof(u->salt));
        memset(u->hash, 0, sizeof(u->hash));
        u->has_password = 0;
    }
    save_all();
    ESP_LOGI(TAG, "password %s for '%s'", u->has_password ? "set" : "cleared", username);
    return true;
}

bool user_mgr_has_password(const char *username) {
    int idx = find_index(username);
    return idx >= 0 && s_users[idx].type == USER_ACCOUNT_LOCAL && s_users[idx].has_password;
}

// False for an unknown username OR a REMOTE account (which has no local
// salt/hash at all — see user_record_t's own has_password comment). The
// caller (pairing_module.c's Phase B access-request handler) treats
// either case the same way: nothing to hand back.
bool user_mgr_get_salt(const char *username, uint8_t out[16]) {
    int idx = find_index(username);
    if (idx < 0 || s_users[idx].type != USER_ACCOUNT_LOCAL || !s_users[idx].has_password) return false;
    memcpy(out, s_users[idx].salt, 16);
    return true;
}

// Constant-time compare against the stored hash — same helper
// user_mgr_verify() itself uses internally, just taking an already-hashed
// candidate instead of a plaintext password. This is what lets a remote
// login (source/modules/pairing/) check a password without user_mgr ever
// seeing the plaintext: the caller hashes client-side (user_mgr_hash_
// password(), using the salt from user_mgr_get_salt()) and only the
// resulting digest crosses back into this module.
bool user_mgr_verify_hash(const char *username, const uint8_t hash[32]) {
    int idx = find_index(username);
    if (idx < 0) return false;
    const user_record_t *u = &s_users[idx];
    if (u->type != USER_ACCOUNT_LOCAL || !u->has_password) return false;
    return constant_time_eq(hash, u->hash, 32);
}

// Creates (or refreshes) a REMOTE-type record — no password path at all,
// matching user_account_type_t's own doc comment ("a REMOTE account's
// record carries no local password hash"). Called client-side once a
// pairing_module.c login round-trip (Phase B/C of the remote-login work)
// has already been proven against the actual server — this function
// itself performs no verification, same division of responsibility
// user_mgr_set_logged_in() already has ("does NOT itself verify a
// password — call user_mgr_verify() first").
//
// Unlike user_mgr_create(), re-creating an existing name is not an error:
// logging into the same remote account again (a normal, expected event —
// see pairing.h's Phase C reconnect flow) just confirms the existing
// record (and syncs is_admin to whatever the server most recently
// reported — a promotion/demotion on the server's own account should take
// effect on the client's next successful login, not get stuck at
// whatever it was the first time) rather than failing on "already
// exists". Refuses to touch a name that already exists as a LOCAL
// account, though — silently converting a real local account into a
// remote stand-in would be a genuine account-identity mix-up, not a
// no-op.
//
// `mac` is stored (and refreshed on every re-confirm, same as is_admin
// above) so a later boot/relock can actually attempt reconnecting this
// identity — see user_mgr_get_remote_mac()'s own comment. A username is
// assumed to belong to exactly one server at a time; a second server
// authenticating the same username just overwrites the stored mac with
// wherever the most recent successful login actually came from.
bool user_mgr_create_remote(const char *username, bool is_admin, const uint8_t mac[6]) {
    if (!user_mgr_valid_username(username) || !mac) return false;

    int idx = find_index(username);
    if (idx >= 0) {
        if (s_users[idx].type != USER_ACCOUNT_REMOTE) return false;
        bool dirty = false;
        if (s_users[idx].is_admin != (is_admin ? 1 : 0)) {
            s_users[idx].is_admin = is_admin ? 1 : 0;
            dirty = true;
        }
        if (memcmp(s_users[idx].mac, mac, 6) != 0) {
            memcpy(s_users[idx].mac, mac, 6);
            dirty = true;
        }
        if (dirty) save_all();
        return true;
    }

    if (s_user_count >= USER_MGR_MAX_USERS) {
        ESP_LOGW(TAG, "cannot create remote '%s' — USER_MGR_MAX_USERS (%d) reached",
                 username, USER_MGR_MAX_USERS);
        return false;
    }

    user_record_t *u = &s_users[s_user_count];
    memset(u, 0, sizeof(*u));
    snprintf(u->name, sizeof(u->name), "%s", username);
    u->type = USER_ACCOUNT_REMOTE;
    u->has_password = 0;   // salt/hash stay zeroed — REMOTE never has a local credential
    u->is_admin = is_admin ? 1 : 0;
    memcpy(u->mac, mac, 6);
    s_user_count++;
    save_all();
    ESP_LOGI(TAG, "created remote user '%s' (admin=%d)", username, (int)u->is_admin);
    return true;
}

// False if unknown or not USER_ACCOUNT_REMOTE (a LOCAL account's mac[]
// field is unused/zeroed — returning it as if meaningful would be a real
// bug, not just an unhelpful answer).
bool user_mgr_get_remote_mac(const char *username, uint8_t out_mac[6]) {
    int idx = find_index(username);
    if (idx < 0 || s_users[idx].type != USER_ACCOUNT_REMOTE || !out_mac) return false;
    memcpy(out_mac, s_users[idx].mac, 6);
    return true;
}

// False (no-op) for a LOCAL account or an unknown username — offline
// access is a REMOTE-account concept only, see user_record_t's own
// comment on why.
bool user_mgr_set_offline_access(const char *username, bool enabled) {
    int idx = find_index(username);
    if (idx < 0 || s_users[idx].type != USER_ACCOUNT_REMOTE) return false;
    uint8_t v = enabled ? 1 : 0;
    if (s_users[idx].offline_access != v) {
        s_users[idx].offline_access = v;
        save_all();
    }
    ESP_LOGI(TAG, "'%s' offline_access = %d", username, (int)v);
    return true;
}

// False for an unknown username or a LOCAL account (never offline-
// capable — it's already fully local, "offline" isn't a meaningful
// state for it) — same "unknown never reads as enabled" caution user_
// mgr_is_admin() already documents for its own equivalent flag.
bool user_mgr_get_offline_access(const char *username) {
    int idx = find_index(username);
    return idx >= 0 && s_users[idx].type == USER_ACCOUNT_REMOTE && s_users[idx].offline_access != 0;
}

bool user_mgr_verify(const char *username, const char *password) {
    int idx = find_index(username);
    if (idx < 0) return false;
    const user_record_t *u = &s_users[idx];

    if (u->type == USER_ACCOUNT_REMOTE) {
        // REMOTE accounts are real now (user_mgr_create_remote(), created
        // by pairing_module.c's Phase C on a successful login) — but this
        // function still isn't how one gets verified. A REMOTE account
        // never has a password (no salt/hash at all — see user_record_t's
        // own has_password comment), and its actual proof-of-identity is
        // pairing.h's own challenge-response, already complete by the time
        // user_mgr_create_remote()/user_mgr_set_logged_in() get called.
        // This path stays a deliberate dead end so the dispatch shape is
        // right for whatever calls user_mgr_verify() generically without
        // knowing the account's type in advance.
        ESP_LOGW(TAG, "'%s' is a remote account — verify via pairing_verify_user(), not this", username);
        return false;
    }

    if (!u->has_password) return true;   // auto-login identity
    if (!password) password = "";

    uint8_t candidate[32];
    hash_password(u->salt, password, candidate);
    return constant_time_eq(candidate, u->hash, sizeof(candidate));
}

const char *user_mgr_default_username(void) {
    if (s_user_count == 1) {
        snprintf(s_default_buf, sizeof(s_default_buf), "%s", s_users[0].name);
        return s_default_buf;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_default_buf);
        esp_err_t err = nvs_get_str(h, NVS_KEY_ACTIVE, s_default_buf, &len);
        nvs_close(h);
        if (err == ESP_OK && user_mgr_exists(s_default_buf)) return s_default_buf;
    }

    snprintf(s_default_buf, sizeof(s_default_buf), "%s", USER_MGR_BOOTSTRAP_USER);
    return s_default_buf;
}

// ── Session ───────────────────────────────────────────────────────────────────

bool        user_mgr_is_logged_in(void) { return s_current_user[0] != '\0'; }
const char *user_mgr_current_user(void) { return s_current_user; }

bool user_mgr_set_logged_in(const char *username) {
    if (!user_mgr_exists(username)) return false;
    snprintf(s_current_user, sizeof(s_current_user), "%s", username);
    save_active(username);
    ESP_LOGI(TAG, "session: logged in as '%s'", username);
    return true;
}

void user_mgr_logout(void) {
    if (s_current_user[0]) {
        ESP_LOGI(TAG, "session: '%s' logged out", s_current_user);
        // A REMOTE account's persisted "active" marker (save_active(),
        // written unconditionally on every login) must NOT survive an
        // explicit logout — confirmed live as a real, reported bug:
        // without this, the next login screen's boot/relock seed
        // (user_mgr_default_username()) kept returning this same remote
        // username, and systemui_login.c's automatic reconnect
        // (begin_remote_reconnect()) fired right back at the server the
        // user just explicitly logged off from. A LOCAL account's logout
        // does NOT clear it — "remember who was last active across a
        // local multi-account device" is real, wanted behavior there,
        // untouched by this.
        if (user_mgr_account_type(s_current_user) == USER_ACCOUNT_REMOTE) {
            clear_active();
        }
    }
    s_current_user[0] = '\0';
}

// ── First-run setup (OOBE) ───────────────────────────────────────────────────

bool user_mgr_oobe_completed(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t done = 0;
    esp_err_t err = nvs_get_u8(h, NVS_KEY_OOBE, &done);
    nvs_close(h);
    return err == ESP_OK && done != 0;
}

void user_mgr_set_oobe_completed(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_OOBE, 1);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "OOBE marked complete");
}

// ── Module lifecycle ──────────────────────────────────────────────────────────

int user_mgr_init(void) {
    load_all();

    if (s_user_count == 0) {
        // Bootstrap default — see user_mgr.h's doc comment. Direct array
        // write rather than calling user_mgr_create(): that function would
        // work fine here too, but this makes the "this only ever happens
        // once, on a genuinely empty table" invariant visible at the call
        // site instead of relying on user_mgr_exists() returning false.
        user_record_t *u = &s_users[0];
        memset(u, 0, sizeof(*u));
        snprintf(u->name, sizeof(u->name), "%s", USER_MGR_BOOTSTRAP_USER);
        u->type = USER_ACCOUNT_LOCAL;
        u->has_password = 0;
        // Admin — whoever set up the device (the only account that can
        // possibly exist at this point) is the natural default admin. See
        // user_mgr_is_admin()'s own doc comment for what this actually
        // gates (remote-login dashboard-vs-desktop routing, currently the
        // only consumer).
        u->is_admin = 1;
        s_user_count = 1;
        save_all();
        ESP_LOGI(TAG, "no users configured — bootstrapped default admin user '%s' (no password)",
                 USER_MGR_BOOTSTRAP_USER);
    }

    s_current_user[0] = '\0';   // never persisted across reboot — see header
    ESP_LOGI(TAG, "init complete (%d user%s)", s_user_count, s_user_count == 1 ? "" : "s");
    return 0;
}

void user_mgr_deinit(void) {
    s_current_user[0] = '\0';
}

PURR_MODULE_REGISTER(user_mgr) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "user_mgr",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = user_mgr_init,
    .deinit            = user_mgr_deinit,
};
