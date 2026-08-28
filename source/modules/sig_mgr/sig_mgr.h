#pragma once
// sig_mgr.h — PURR OS artifact signing/verification (apps + drivers)
//
// Hardens the honor-system "-- purr-sig: <value>" comment tag that
// app_manager.c/catstrap.py have used until now (see scan_purr_sig()'s own
// doc comment in app_manager.c) — that tag is a self-declared string with
// no cryptography behind it, catches nothing, and only ever covered
// .hiss/.kitten. This replaces it with real Ed25519 signature
// verification (source/lib/lib_ed25519, already vendored — used here for
// the FIRST time), covering every loadable artifact type: apps of every
// tier AND driver .purr blobs, neither of which had any trust concept
// before this.
//
// ── Signature attachment ─────────────────────────────────────────────────────
// A companion file next to the artifact, not an embedded tag — works
// identically for a compiled .claw/.paws/.purr blob and a plain-text
// .meow/.hiss/.kitten script, and needs no artifact format change:
//   <path>.sig   64 raw bytes  — Ed25519 signature over SHA-256(file content)
//   <path>.pub   32 raw bytes  — Ed25519 public key (USER tier only — see below)
//
// ── The four tiers ────────────────────────────────────────────────────────────
//   OFFICIAL  — .sig verifies against the baked-in official public key
//               (this project's release-signing key; the matching private
//               key never lives on a device).
//   DEV       — same, against the baked-in dev-signed public key (a second,
//               separate root for nightly/development builds — deliberately
//               a DIFFERENT keypair from official, not a lower-security
//               copy of it, so one can be revoked/rotated without the other).
//   USER      — .sig verifies against a .pub file found ALONGSIDE the
//               artifact (same directory — typically the SD card). No
//               enrollment, no NVS pre-registration: sign your own build
//               with your own key, drop both files on the card next to
//               it, done. Deliberately the lowest form of "signed" —
//               proves the file matches what that specific key signed and
//               has not changed since, nothing more. This is what makes
//               "test my own driver" low-friction rather than needing a
//               device-side trust ceremony first.
//   UNSIGNED  — no .sig, or a .sig that matches none of the above.
//
// TAMPERED is reported as a distinct value from UNSIGNED, not folded into
// it — see sig_tier_t below. A file with no signature at all and a file
// whose signature exists but no longer matches what it was signed against
// are different situations (the second implies the artifact changed AFTER
// someone signed it) and callers should be able to react differently.
//
// ── Device identity key ──────────────────────────────────────────────────────
// Separate from the USER tier above, not a replacement for it: a keypair
// this device generates for itself on first use (esp_fill_random() seed,
// NVS-persisted, plaintext — same physical-possession threat model this
// codebase already applies to user_mgr's stored credentials; flash
// encryption is a separate, not-currently-enabled IDF feature). Exposed so
// something on-device can sign with it later if a feature needs to; not
// wired into the USER-tier verification path itself, which only ever
// checks a co-located .pub regardless of whether it happens to be this
// device's own.
//
// ── What is NOT verified here ────────────────────────────────────────────────
// orlp/ed25519 (lib_ed25519) is a mature, widely-deployed, RFC 8032-
// compliant implementation, and the signing side of this (catstrap/
// purrstrap tooling, Python's `cryptography` package) produces standard
// 32-byte public keys and 64-byte signatures — the same sizes this file's
// own cross-check confirmed. Real interoperability between the two was
// NOT confirmed by an executed on-device (or even host-compiled) test in
// this pass — this machine has no complete host C toolchain (esp-clang
// ships without a usable sysroot for either GNU or MSVC target modes,
// and no separate MSVC install exists to supply one). The first real
// flash of this feature should include a boot-time self-check (sign a
// known value with the device's own key, verify it right back) so this
// gets confirmed for real rather than left as a documented assumption.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SIG_TIER_UNSIGNED = 0,   // no .sig, or .sig matches no known/co-located key
    SIG_TIER_TAMPERED,       // .sig + co-located .pub form a self-consistent
                              // pair, but the signature does NOT verify
                              // against the artifact's current content —
                              // the strongest signal something changed
                              // after it was signed. See this header's own
                              // doc comment.
    SIG_TIER_USER,           // verifies against a co-located .pub
    SIG_TIER_DEV,             // verifies against the baked-in dev key
    SIG_TIER_OFFICIAL,       // verifies against the baked-in official key
} sig_tier_t;

const char *sig_tier_name(sig_tier_t tier);

int  sig_mgr_init(void);
void sig_mgr_deinit(void);

// Classifies the artifact at `path` by looking for "<path>.sig" (and,
// for the USER-tier path, "<path>.pub") in the same directory. Reads and
// SHA-256-hashes `path` itself to do so — safe to call on any file,
// including one on SD, but not free (a full read of the artifact); call
// once per load, not per frame.
sig_tier_t sig_mgr_classify(const char *path);

// Same classification, but for a caller that already has the artifact's
// full content in memory (app_manager.c's .meow/.hiss/.kitten launch path
// does — see its own DMA-contention comments on why re-reading from SD a
// second time just for this is worth avoiding) — hashes `data`/`len`
// directly instead of opening `path`. `path` is still needed, only to
// locate "<path>.sig"/"<path>.pub"; it is never itself read.
sig_tier_t sig_mgr_classify_buffer(const uint8_t *data, size_t len, const char *path);

// This device's own identity key — see this header's doc comment on why
// it is separate from USER-tier verification. `out` receives the 32-byte
// raw public key. False if sig_mgr has not initialised yet.
bool sig_mgr_device_pubkey(uint8_t out[32]);

// Signs `len` bytes of `data` with the device's own identity key,
// producing a 64-byte raw Ed25519 signature in `out_sig`. False if
// sig_mgr has not initialised yet.
bool sig_mgr_sign_with_device_key(const uint8_t *data, size_t len, uint8_t out_sig[64]);

#ifdef __cplusplus
}
#endif
