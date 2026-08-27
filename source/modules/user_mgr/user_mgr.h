#pragma once
// user_mgr.h — PURR OS multi-user accounts (public API)
//
// Core plumbing only — accounts, credential storage/verification, and
// session state. Deliberately does NOT touch any UI: no lock screen, no
// login prompt, no auto-login policy. That belongs to whichever UI backend
// decides to gate on it later (systemUI's existing lock screen — see
// purr_systemui_is_locked() in systemui_android.c/systemui_ios.c — is the
// natural home for that, but wiring it in is a separate, deliberately
// deferred pass).
//
// ── Accounts ─────────────────────────────────────────────────────────────────
// Unix-style: named accounts, each with an optional password. No password
// set means the account is a no-prompt/auto-login identity — verification
// against it always succeeds, matching how the bootstrap default below
// works. This mirrors a real Unix account with an empty/locked shadow entry
// more than it mirrors "no security" — the DISTINCTION (has a password vs.
// doesn't) is the whole point, not an oversight.
//
// Bootstrap: user_mgr_init() creates a single account named "milkaholic"
// with no password the first time it ever runs (user_mgr_count() == 0) —
// so the system always has at least one identity, and a fresh device never
// needs configuring before use.
//
// ── Credentials ──────────────────────────────────────────────────────────────
// Salted SHA-256 (mbedtls, already a dependency elsewhere in this tree —
// see ota_mgr.c's own checksum verification), NOT a slow KDF
// (bcrypt/scrypt/PBKDF2-many-rounds): those exist to make offline
// dictionary attacks on a stolen credential DATABASE expensive, which is
// the wrong threat model here — a device this size is attacked by physical
// possession, not a leaked hash file, and a deliberately slow hash would
// make every legitimate login noticeably sluggish on this hardware for a
// threat that mostly doesn't apply. Salt is 16 random bytes from
// esp_fill_random() (hardware RNG, no new dependency); verification uses a
// constant-time compare so a legitimate check and a wrong guess take
// observably the same time.
//
// ── Account type: local vs. remote ──────────────────────────────────────────
// user_account_type_t exists so a future remote-account-server integration
// (explicitly planned, client side first — see this file's own history) is
// additive: a REMOTE account's record carries no local password hash at
// all, and user_mgr_verify() dispatches on type. Today that dispatch is a
// dead end — no remote-verification transport exists yet, so a REMOTE
// account (nothing creates one yet either) always fails to verify — but the
// account SHAPE will not need to change when that transport is built.
//
// ── Session ──────────────────────────────────────────────────────────────────
// NOT persisted across reboot, by design: every boot starts logged out,
// the same way a phone starts at its lock screen after a hard reboot
// regardless of how long you were logged in before. Whether that means an
// immediate visible prompt or a silent auto-login is entirely the UI
// layer's call (see this file's own header note) — user_mgr only tracks
// "who, if anyone, is currently authenticated this boot."

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USER_MGR_MAX_USERS      8    // more than enough for a shared/family device
#define USER_MGR_USERNAME_MAX   32   // matches purr_module_header_t::name's own bound
#define USER_MGR_BOOTSTRAP_USER "milkaholic"

typedef enum {
    USER_ACCOUNT_LOCAL  = 0,   // credentials stored + verified on-device (today's only working path)
    USER_ACCOUNT_REMOTE = 1,   // verified against a remote account server — see this
                                // header's own doc comment. Not implemented.
} user_account_type_t;

int  user_mgr_init(void);
void user_mgr_deinit(void);

// Unix-style rules: 1-31 chars, lowercase [a-z0-9_], must start with a
// letter. Rejects anything else outright — callers do not need their own
// pre-validation.
bool user_mgr_valid_username(const char *username);

int  user_mgr_count(void);
// Enumerates by index (0..user_mgr_count()-1). Copies up to name_out_sz-1
// bytes, NUL-terminated. False if idx is out of range.
bool user_mgr_at(int idx, char *name_out, size_t name_out_sz);

bool user_mgr_exists(const char *username);
user_account_type_t user_mgr_account_type(const char *username);   // USER_ACCOUNT_LOCAL if not found

// Creates a LOCAL account. `password` may be NULL or "" for no password
// (auto-login identity). Fails (false) if the username is invalid, already
// exists, or USER_MGR_MAX_USERS is already reached.
bool user_mgr_create(const char *username, const char *password);

// Removes an account. Logs the caller out first if it was the current
// session's user — an authenticated session for an account that no longer
// exists is not a state anything downstream should have to handle.
bool user_mgr_remove(const char *username);

// Changes an existing LOCAL account's password. NULL/"" clears it (makes
// the account no-password / auto-login). False for a REMOTE account —
// there is no local credential to change.
bool user_mgr_set_password(const char *username, const char *password);

bool user_mgr_has_password(const char *username);

// LOCAL: verifies via the salted-hash compare described above; a
// no-password account verifies true regardless of what `password` is.
// REMOTE: always false — see this header's own doc comment on why.
// Unknown username: always false.
bool user_mgr_verify(const char *username, const char *password);

// The identity to seed a login flow with — the sole account if there is
// exactly one, otherwise the persisted last-logged-in username, falling
// back to USER_MGR_BOOTSTRAP_USER if neither applies. Does not imply an
// authenticated session; purely a UI convenience so a caller is never
// left picking a default with no signal at all.
const char *user_mgr_default_username(void);

// ── Session (this boot only — see header note) ──────────────────────────────

bool        user_mgr_is_logged_in(void);
const char *user_mgr_current_user(void);   // "" if not logged in

// Marks `username` as the current session. Does NOT itself verify a
// password — call user_mgr_verify() first and only call this on success.
// Persists `username` as the next boot's default (see
// user_mgr_default_username()). False if the account does not exist.
bool user_mgr_set_logged_in(const char *username);

void user_mgr_logout(void);

// ── First-run setup (OOBE) ───────────────────────────────────────────────────
// A separate, explicit completion marker — NOT inferred from "an account
// other than the bootstrap default exists." A real out-of-box flow can be
// skipped or can finish with the person deliberately keeping the
// no-password default, and neither of those should make it come back on
// the next boot demanding setup again. Persisted in the same NVS namespace
// as everything else here, but as its own key: creating/renaming accounts
// and "the setup flow has run" are independent facts.
bool user_mgr_oobe_completed(void);
void user_mgr_set_oobe_completed(void);

#ifdef __cplusplus
}
#endif
