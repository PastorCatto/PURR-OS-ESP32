/*
 * purr_repo_trust.h - who a repository is, and whether to trust it (Repositories/SPEC.md sections 3.1, 4 and 5).
 *
 * A repository publishes a release file (repo.json) with its public key. This reads it, checks the repository's id and its own signature,
 * checks an endorsement against the device's key bag, and keeps the list of repositories an administrator has trusted (/etc/repos.conf).
 * It also answers the loader's question: does a trusted repository's signed index stand for this package?
 *
 * Pure C with no allocation and no ESP-IDF, host-tested (test_repo_trust.c). The P-256 check goes through purr_crypto_t, as for images.
 */
#ifndef PURR_REPO_TRUST_H
#define PURR_REPO_TRUST_H

#include <stddef.h>
#include <stdint.h>

#include "purr_keybag.h"
#include "purr_repo.h"
#include "purr_verify.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_RT_ID_HEX     16            /* a repository id: 16 hex digits */
#define PURR_REPOS_MAX     6

typedef enum {
    PURR_TIER_UNTRUSTED = 0,             /* a bad signature, a changed key, or not confirmed: developer mode only */
    PURR_TIER_COMMUNITY = 1,             /* an administrator added it and confirmed its fingerprint */
    PURR_TIER_OFFICIAL  = 2,             /* its key is endorsed by a key in the key bag */
    PURR_TIER_YOURS     = 3,             /* a developer server on the local network (the SDK kind) */
} purr_repo_tier_t;

typedef struct {
    int format;
    char id[PURR_RT_ID_HEX + 1];
    char name[40];
    char maintainer[40];
    char url[96];
    char description[96];
    char min_os[16];
    uint8_t pub[PURR_PUBKEY_LEN];
    uint8_t has_key;
    uint8_t has_endorsement;
    uint8_t end_key_id;
    uint8_t end_sig[PURR_SIG_LEN];
    uint8_t flat;                        /* "layout": "flat" (a release's assets) or "tree" */
    uint32_t updated;
} purr_repo_info_t;

/* Parses a repo.json. 0, or -1 with the reason in err. A key or signature that is not the right length is refused. */
int purr_repo_info_parse(const char *json, size_t len, purr_repo_info_t *out, char *err, size_t errcap);

/* The id of a key: the first 16 hex digits (lowercase) of SHA-256 of its 64 bytes. */
void purr_repo_fingerprint(const uint8_t pub[PURR_PUBKEY_LEN], char id[PURR_RT_ID_HEX + 1]);

/* A detached signature file: 128 hex digits, surrounding whitespace allowed. 0 or -1. */
int purr_repo_sig_decode(const char *text, size_t len, uint8_t sig[PURR_SIG_LEN]);

/* 0 if sig is a valid signature of exactly these bytes by pub (SHA-256, then P-256). */
int purr_repo_check_signed(const purr_crypto_t *crypto, const uint8_t pub[PURR_PUBKEY_LEN], const void *data, size_t len, const uint8_t sig[PURR_SIG_LEN]);

/* The release file as a whole: the id must be the key's fingerprint, and repo.json must carry the signature of its own key. 0 or -1 with err. */
int purr_repo_info_check(const purr_crypto_t *crypto, const purr_repo_info_t *info, const void *json, size_t len, const uint8_t sig[PURR_SIG_LEN],
                         char *err, size_t errcap);

/* COMMUNITY if the release file has no endorsement; OFFICIAL if it has one and a key in the bag with that id verifies it; UNTRUSTED if it has one that does not verify. */
purr_repo_tier_t purr_repo_endorsement_tier(const purr_crypto_t *crypto, const purr_keybag_t *bag, const purr_repo_info_t *info);

/* 1 if the (already signature-checked) index lists an app named `name` whose package hash is `sha256`. */
int purr_repo_vouches(const purr_repo_index_t *idx, const char *name, const uint8_t sha256[32]);

const char *purr_repo_tier_name(purr_repo_tier_t t);

/* ---- the list of trusted repositories: /etc/repos.conf -------------------------------------------------- */

typedef struct {
    char id[PURR_RT_ID_HEX + 1];
    char name[40];
    char url[96];
    uint8_t pub[PURR_PUBKEY_LEN];
    uint8_t tier;                        /* purr_repo_tier_t, as confirmed */
    uint8_t flat;
    uint32_t added;                      /* unix seconds, 0 if the clock was not set */
} purr_repo_entry_t;

typedef struct {
    purr_repo_entry_t e[PURR_REPOS_MAX];
    int count;
    int dropped;                         /* stanzas that were malformed */
} purr_repos_t;

/* The file is stanzas of key=value lines (id, name, url, pub, tier, layout, added) separated by blank lines; # starts a comment. A stanza whose id is not its
 * key's fingerprint, or that has no usable key, is dropped: the file is read as untrusted input. */
int purr_repos_parse(const char *text, size_t len, purr_repos_t *out);
int purr_repos_format(const purr_repos_t *r, char *out, size_t cap);          /* bytes written, or -1 if it did not fit */

const purr_repo_entry_t *purr_repos_find(const purr_repos_t *r, const char *id);

typedef enum {
    PURR_RA_NEW = 0,                     /* a repository that is not in the list */
    PURR_RA_KNOWN,                       /* in the list, same key */
    PURR_RA_REKEYED,                     /* the address is in the list under a different key: the repository changed its key */
} purr_repo_add_class_t;

/* What fetching this release file means for the list. */
purr_repo_add_class_t purr_repos_classify(const purr_repos_t *r, const purr_repo_info_t *info, const char *url);

/* Adds a confirmed repository (or refreshes a known one). -1 if the list is full. An entry for the same address under another key is removed first
 * (the administrator confirmed the new fingerprint). */
int purr_repos_add(purr_repos_t *r, const purr_repo_info_t *info, const char *url, purr_repo_tier_t tier, uint32_t now);
int purr_repos_remove(purr_repos_t *r, const char *id);

#ifdef __cplusplus
}
#endif

#endif /* PURR_REPO_TRUST_H */
