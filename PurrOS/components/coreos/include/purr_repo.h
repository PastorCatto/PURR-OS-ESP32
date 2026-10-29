/*
 * purr_repo.h - a repository of apps: what the app manager can install from (plan8.md).
 *
 * A repository is an index (what exists) and packages (the .cat files). The same client code serves a
 * local SDK server that hands out pre-compiled developer builds, and the official app store that will
 * come later; what differs is the policy for each KIND, enforced here and not left to the caller:
 *
 *   SDK    local network only (private, loopback or link-local addresses), http or https, signed or
 *          unsigned packages (unsigned only ever land in a user's devapps folder, with developer mode on)
 *   STORE  https only, any public host, signed packages only, and a signed index (index.json.sig)
 *
 * This file is pure C with no allocation and no ESP-IDF, so it is host-tested (test_repo.c). Fetching
 * the bytes, and installing through purr_appmgr_add(), are the device's job (main/purr_repo_dev.c).
 *
 * The index is advisory. A package is never trusted because the index says so: its size and sha256 are
 * checked against the index, and then purr_appmgr_add() verifies it as it verifies any app.
 */
#ifndef PURR_REPO_H
#define PURR_REPO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_REPO_FORMAT       1
#define PURR_REPO_FORMAT_STORE 2        /* the signed index of a store repository (Repositories/SPEC.md) */

/* The official repository: the release JumpingJaguar3 of the project's repository. A PLACEHOLDER until that release is published (JumpingJaguar1 holds the
 * recovery images and JumpingJaguar2 the drivers and modules). Its layout is flat: repo.json, index.json, their .sig files and the packages are all assets. */
#define PURR_REPO_OFFICIAL_URL "https://github.com/PastorCatto/PURR-OS-ESP32/releases/download/JumpingJaguar3"
#define PURR_REPO_MAX_APPS     32
#define PURR_REPO_NAME_LEN     32       /* app name, as in the app manager */
#define PURR_REPO_VERSION_LEN  16
#define PURR_REPO_TITLE_LEN    32
#define PURR_REPO_DESC_LEN     80
#define PURR_REPO_URL_LEN      96       /* an app's package path, relative to the repository */
#define PURR_REPO_DEST_LEN     64       /* where an asset or extra is written, relative to its folder */
#define PURR_REPO_ID_LEN       24
#define PURR_REPO_MAX_PACKAGE  (512u * 1024u)   /* the app manager's scratch buffer */
#define PURR_REPO_MAX_INDEX    (24u * 1024u)    /* an index larger than this is refused */

typedef enum {
    PURR_REPO_SDK = 0,
    PURR_REPO_STORE = 1,
} purr_repo_kind_t;

typedef struct {
    char name[PURR_REPO_NAME_LEN];
    char title[PURR_REPO_TITLE_LEN];
    char version[PURR_REPO_VERSION_LEN];
    char desc[PURR_REPO_DESC_LEN];
    char url[PURR_REPO_URL_LEN];        /* relative: apps/clock/1.0.0/package.cat */
    char chip[12];                      /* "any", "esp32s3", ... */
    uint32_t size;                      /* bytes in the package */
    uint32_t updated;                   /* unix seconds, 0 if unknown */
    uint8_t sha256[32];
    uint8_t abi;                        /* app ABI the package was built for, 0 if unknown */
    uint8_t key_id;                     /* the key it claims to be signed with; 0 if none */
    uint8_t signed_claim;               /* the repository says it is signed (the package decides) */
    /* Format 2 (Repositories/SPEC.md): */
    uint8_t type;                       /* PURR_REPO_ITEM_* */
    uint8_t target;                     /* PURR_REPO_TARGET_* */
    uint8_t needs_sd;                   /* cannot work without an SD card */
    uint8_t vouched;                    /* the index is signed by a trusted repository, which stands for the package (format 2, store kind) */
    char dest[PURR_REPO_DEST_LEN];      /* an asset or extra: its path under the destination folder */
    char min_os[12];                    /* the oldest OS it works on, "" for any */
} purr_repo_app_t;

#define PURR_REPO_ITEM_APP    0
#define PURR_REPO_ITEM_ASSET  1
#define PURR_REPO_ITEM_EXTRA  2
#define PURR_REPO_TARGET_FLASH 0
#define PURR_REPO_TARGET_SD    1
#define PURR_REPO_TARGET_ANY   2

typedef struct {
    int format;
    purr_repo_kind_t kind;
    char id[PURR_REPO_ID_LEN];          /* the repository's identity, remembered once trusted */
    char name[PURR_REPO_TITLE_LEN];
    purr_repo_app_t apps[PURR_REPO_MAX_APPS];
    int count;
    int dropped;                        /* entries that were malformed or broke the kind's policy */
} purr_repo_index_t;

/* Parses an index.json. Returns 0 and fills *out, or -1 and says why in err. A malformed app entry is
 * dropped (counted in out->dropped), never fatal; a malformed document is. If `kind` is given, an
 * index that declares another kind is refused, and the kind's policy is applied to every entry. */
int purr_repo_index_parse(const char *json, size_t len, purr_repo_index_t *out,
                          const purr_repo_kind_t *kind, char *err, size_t errcap);

const purr_repo_app_t *purr_repo_index_find(const purr_repo_index_t *idx, const char *name);

/* ---- the address policy -------------------------------------------------------------------------- */

/* 1 if the IPv4 address (host byte order) is private, loopback or link-local. */
int purr_repo_ip_is_local(uint32_t ipv4);

/* Whether a repository of this kind may be contacted at this base URL (http://host[:port][/path]).
 * Checks the text only; a host NAME must also be checked once resolved (see purr_repo_ip_is_local()). */
int purr_repo_url_allowed(purr_repo_kind_t kind, const char *url, char *why, size_t whycap);

/* Splits a URL. host, path and scheme are NUL-terminated copies; the port defaults to 80 or 443.
 * Returns 0, or -1 if it is not a well-formed http(s) URL (no userinfo, no spaces, no IPv6 literal). */
int purr_repo_url_split(const char *url, char *scheme, size_t scheme_cap, char *host, size_t host_cap,
                        uint16_t *port, char *path, size_t path_cap);

/* The URL of a file in a repository: <base>/purr/v1/<rel>, where base is the repository's address
 * (http://10.0.2.2:47801) and rel is "index.json" or an app's url from the index. Refuses anything that is
 * not a plain relative path (no "..", no scheme, no query). Returns 0 or -1. */
int purr_repo_url_join(const char *base, const char *rel, char *out, size_t cap);

/* ---- policy for entries and downloads -------------------------------------------------------------- */

/* Whether this kind may offer this entry at all (a store never offers unsigned apps). */
int purr_repo_app_allowed(purr_repo_kind_t kind, const purr_repo_app_t *app, char *why, size_t whycap);

typedef enum {
    PURR_REPO_PKG_OK = 0,
    PURR_REPO_PKG_BAD_SIZE,
    PURR_REPO_PKG_BAD_HASH,
} purr_repo_pkg_result_t;

/* The downloaded bytes against the index entry: exact size, then sha256. */
purr_repo_pkg_result_t purr_repo_check_package(const purr_repo_app_t *app, const uint8_t *data, size_t len);

/* ---- versions and updates -------------------------------------------------------------------------- */

/* Compares dotted versions numerically (1.10.0 > 1.9.0); a missing part counts as 0; "-suffix" marks a
 * pre-release, which sorts before the same version without it. Returns <0, 0 or >0. */
int purr_repo_version_cmp(const char *a, const char *b);

typedef struct {
    char name[PURR_REPO_NAME_LEN];
    char version[PURR_REPO_VERSION_LEN];
} purr_repo_installed_t;

/* For each installed app, the index entry that is newer, if any. Writes the entries' indexes into
 * out[] (up to cap) and returns how many. */
int purr_repo_updates(const purr_repo_index_t *idx, const purr_repo_installed_t *installed, int n,
                      int *out, int cap);

/* Decodes 64 hex digits into 32 bytes. Returns 0, or -1 if it is not exactly that. */
int purr_repo_hex_to_sha256(const char *hex, uint8_t out[32]);

const char *purr_repo_pkg_result_name(purr_repo_pkg_result_t r);

#ifdef __cplusplus
}
#endif

#endif /* PURR_REPO_H */
