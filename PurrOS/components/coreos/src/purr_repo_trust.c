/*
 * purr_repo_trust.c - see purr_repo_trust.h.
 */
#include "purr_repo_trust.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "purr_util.h"

static const char PREFIX[] = "PURRREPO";        /* the endorsement is over "PURRREPO\0" + SHA-256(pub): nine bytes with the terminator */

static void set_err(char *err, size_t cap, const char *msg)
{
    if (err != NULL && cap > 0) {
        snprintf(err, cap, "%s", msg);
    }
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *s, size_t n, uint8_t *out, size_t outlen)
{
    if (n != outlen * 2) {
        return -1;
    }
    for (size_t i = 0; i < outlen; i++) {
        int a = hexval((unsigned char)s[i * 2]), b = hexval((unsigned char)s[i * 2 + 1]);
        if (a < 0 || b < 0) {
            return -1;
        }
        out[i] = (uint8_t)((a << 4) | b);
    }
    return 0;
}

static void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = d[in[i] >> 4];
        out[i * 2 + 1] = d[in[i] & 15];
    }
    out[n * 2] = '\0';
}

void purr_repo_fingerprint(const uint8_t pub[PURR_PUBKEY_LEN], char id[PURR_RT_ID_HEX + 1])
{
    uint8_t h[32];
    purr_sha256(pub, PURR_PUBKEY_LEN, h);
    hex_encode(h, PURR_RT_ID_HEX / 2, id);
}

int purr_repo_sig_decode(const char *text, size_t len, uint8_t sig[PURR_SIG_LEN])
{
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' || text[len - 1] == ' ' || text[len - 1] == '\t')) {
        len--;
    }
    while (len > 0 && (*text == '\n' || *text == '\r' || *text == ' ' || *text == '\t')) {
        text++;
        len--;
    }
    return hex_decode(text, len, sig, PURR_SIG_LEN);
}

int purr_repo_check_signed(const purr_crypto_t *crypto, const uint8_t pub[PURR_PUBKEY_LEN], const void *data, size_t len, const uint8_t sig[PURR_SIG_LEN])
{
    uint8_t h[32];
    purr_sha256(data, len, h);
    return crypto != NULL && crypto->verify_p256(pub, h, sig) ? 0 : -1;
}

/* ------------------------------------------------------------------ repo.json: a deliberately small reader (flat strings, one nested object each for key and endorsement) */

typedef struct {
    const char *p, *end;
} rd_t;

static void ws(rd_t *r)
{
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r')) {
        r->p++;
    }
}

static int peek(rd_t *r)
{
    ws(r);
    return r->p < r->end ? (unsigned char)*r->p : -1;
}

/* A string into out (cap bytes). Returns its length, -1 if malformed, -2 if it did not fit (consumed). out may be NULL to skip. */
static int str(rd_t *r, char *out, size_t cap)
{
    if (peek(r) != '"') {
        return -1;
    }
    r->p++;
    size_t n = 0;
    int over = 0;
    while (r->p < r->end && *r->p != '"') {
        unsigned char c = (unsigned char)*r->p++;
        if (c < 0x20) {
            return -1;
        }
        if (c == '\\') {
            if (r->p >= r->end) return -1;
            char e = *r->p++;
            if (e == 'n') c = '\n';
            else if (e == 't') c = '\t';
            else if (e == 'r') c = '\r';
            else if (e == '"' || e == '\\' || e == '/') c = (unsigned char)e;
            else if (e == 'u') {
                if (r->end - r->p < 4) return -1;
                r->p += 4;
                c = '?';
            } else return -1;
        }
        if (out != NULL) {
            if (n + 1 < cap) out[n] = (char)c;
            else over = 1;
        }
        n++;
    }
    if (r->p >= r->end) {
        return -1;
    }
    r->p++;
    if (out != NULL && cap > 0) {
        out[over ? cap - 1 : n] = '\0';
    }
    return over ? -2 : (int)n;
}

static int num(rd_t *r, long long *out)
{
    ws(r);
    const char *s = r->p;
    long long v = 0;
    int d = 0, neg = 0;
    if (r->p < r->end && *r->p == '-') {
        neg = 1;
        r->p++;
    }
    while (r->p < r->end && *r->p >= '0' && *r->p <= '9') {
        if (v < 1000000000000LL) v = v * 10 + (*r->p - '0');
        r->p++;
        d++;
    }
    if (!d) {
        r->p = s;
        return -1;
    }
    *out = neg ? -v : v;
    return 0;
}

static int skip(rd_t *r, int depth);

static int skip_container(rd_t *r, char open, char close, int depth)
{
    if (depth > 6) return -1;
    r->p++;
    if (peek(r) == close) {
        r->p++;
        return 0;
    }
    for (;;) {
        if (open == '{') {
            if (str(r, NULL, 0) < 0 || peek(r) != ':') return -1;
            r->p++;
        }
        if (skip(r, depth + 1) != 0) return -1;
        int c = peek(r);
        if (c == ',') { r->p++; continue; }
        if (c == close) { r->p++; return 0; }
        return -1;
    }
}

static int skip(rd_t *r, int depth)
{
    int c = peek(r);
    long long d;
    if (c == '"') return str(r, NULL, 0) == -1 ? -1 : 0;
    if (c == '{') return skip_container(r, '{', '}', depth);
    if (c == '[') return skip_container(r, '[', ']', depth);
    if (c == 't' && r->end - r->p >= 4 && memcmp(r->p, "true", 4) == 0) { r->p += 4; return 0; }
    if (c == 'f' && r->end - r->p >= 5 && memcmp(r->p, "false", 5) == 0) { r->p += 5; return 0; }
    if (c == 'n' && r->end - r->p >= 4 && memcmp(r->p, "null", 4) == 0) { r->p += 4; return 0; }
    return num(r, &d);
}

/* Reads a nested object of string/integer members into a callback-free fixed set: used for "key" and "endorsement". */
static int read_key_obj(rd_t *r, char *alg, size_t alg_cap, char *pub_hex, size_t pub_cap)
{
    if (peek(r) != '{') return -1;
    r->p++;
    if (peek(r) == '}') { r->p++; return 0; }
    for (;;) {
        char k[8];
        int kl = str(r, k, sizeof(k));
        if (kl == -1 || peek(r) != ':') return -1;
        r->p++;
        int rc = 0;
        if (kl >= 0 && strcmp(k, "alg") == 0) rc = str(r, alg, alg_cap);
        else if (kl >= 0 && strcmp(k, "pub") == 0) rc = str(r, pub_hex, pub_cap);
        else if (skip(r, 2) != 0) return -1;
        if (rc == -1) return -1;
        int c = peek(r);
        if (c == ',') { r->p++; continue; }
        if (c == '}') { r->p++; return 0; }
        return -1;
    }
}

int purr_repo_info_parse(const char *json, size_t len, purr_repo_info_t *out, char *err, size_t errcap)
{
    memset(out, 0, sizeof(*out));
    if (json == NULL || len == 0 || len > 4096) {
        set_err(err, errcap, len > 4096 ? "the release file is too large" : "the release file is empty");
        return -1;
    }
    rd_t r = {json, json + len};
    char pub_hex[160] = "", alg[12] = "", end_sig_hex[160] = "";
    int have_end = 0, bad = 0;
    long long n;
    if (peek(&r) != '{') {
        set_err(err, errcap, "the release file is not a JSON object");
        return -1;
    }
    r.p++;
    if (peek(&r) != '}') {
        for (;;) {
            char k[16];
            int kl = str(&r, k, sizeof(k));
            if (kl == -1 || peek(&r) != ':') {
                set_err(err, errcap, "the release file is malformed");
                return -1;
            }
            r.p++;
            int rc = 0;
            if (kl < 0) { if (skip(&r, 1) != 0) rc = -1; }
            else if (strcmp(k, "format") == 0)      { if (num(&r, &n) != 0) rc = -1; else out->format = (int)n; }
            else if (strcmp(k, "id") == 0)          rc = str(&r, out->id, sizeof(out->id));
            else if (strcmp(k, "name") == 0)        rc = str(&r, out->name, sizeof(out->name));
            else if (strcmp(k, "maintainer") == 0)  rc = str(&r, out->maintainer, sizeof(out->maintainer));
            else if (strcmp(k, "url") == 0)         rc = str(&r, out->url, sizeof(out->url));
            else if (strcmp(k, "description") == 0) rc = str(&r, out->description, sizeof(out->description));
            else if (strcmp(k, "min_os") == 0)      rc = str(&r, out->min_os, sizeof(out->min_os));
            else if (strcmp(k, "updated") == 0)     { if (num(&r, &n) != 0) rc = -1; else out->updated = n > 0 ? (uint32_t)n : 0; }
            else if (strcmp(k, "layout") == 0) {
                char v[8];
                rc = str(&r, v, sizeof(v));
                if (rc >= 0) out->flat = strcmp(v, "tree") != 0;
            }
            else if (strcmp(k, "key") == 0)         { if (read_key_obj(&r, alg, sizeof(alg), pub_hex, sizeof(pub_hex)) != 0) rc = -1; }
            else if (strcmp(k, "endorsement") == 0) {
                have_end = 1;
                if (peek(&r) != '{') { rc = -1; }
                else {
                    r.p++;
                    if (peek(&r) != '}') {
                        for (;;) {
                            char ek[8];
                            int el = str(&r, ek, sizeof(ek));
                            if (el == -1 || peek(&r) != ':') { rc = -1; break; }
                            r.p++;
                            if (el >= 0 && strcmp(ek, "key_id") == 0) {
                                if (num(&r, &n) != 0) { rc = -1; break; }
                                out->end_key_id = (n > 0 && n < 256) ? (uint8_t)n : 0;
                            } else if (el >= 0 && strcmp(ek, "sig") == 0) {
                                if (str(&r, end_sig_hex, sizeof(end_sig_hex)) == -1) { rc = -1; break; }
                            } else if (skip(&r, 2) != 0) { rc = -1; break; }
                            int c = peek(&r);
                            if (c == ',') { r.p++; continue; }
                            if (c == '}') break;
                            rc = -1;
                            break;
                        }
                    }
                    if (rc == 0) r.p++;
                }
            }
            else if (skip(&r, 1) != 0) rc = -1;
            if (rc == -1) {
                set_err(err, errcap, "the release file is malformed");
                return -1;
            }
            if (rc == -2) bad = 1;                    /* a field too long to keep */
            int c = peek(&r);
            if (c == ',') { r.p++; continue; }
            if (c == '}') break;
            set_err(err, errcap, "the release file is malformed");
            return -1;
        }
    }
    if (bad) {
        set_err(err, errcap, "a field in the release file is too long");
        return -1;
    }
    if (out->format != PURR_REPO_FORMAT_STORE) {
        set_err(err, errcap, "unsupported release file format");
        return -1;
    }
    if (strcmp(alg, "p256") != 0 || hex_decode(pub_hex, strlen(pub_hex), out->pub, PURR_PUBKEY_LEN) != 0) {
        set_err(err, errcap, "the release file has no usable key");
        return -1;
    }
    out->has_key = 1;
    if (have_end) {
        if (out->end_key_id == 0 || hex_decode(end_sig_hex, strlen(end_sig_hex), out->end_sig, PURR_SIG_LEN) != 0) {
            set_err(err, errcap, "the endorsement is malformed");
            return -1;
        }
        out->has_endorsement = 1;
    }
    if (out->name[0] == '\0') {
        set_err(err, errcap, "the repository has no name");
        return -1;
    }
    return 0;
}

int purr_repo_info_check(const purr_crypto_t *crypto, const purr_repo_info_t *info, const void *json, size_t len, const uint8_t sig[PURR_SIG_LEN],
                         char *err, size_t errcap)
{
    char fp[PURR_RT_ID_HEX + 1];
    if (!info->has_key) {
        set_err(err, errcap, "no key");
        return -1;
    }
    purr_repo_fingerprint(info->pub, fp);
    if (strcmp(fp, info->id) != 0) {
        set_err(err, errcap, "the repository's id is not the fingerprint of its key");
        return -1;
    }
    if (purr_repo_check_signed(crypto, info->pub, json, len, sig) != 0) {
        set_err(err, errcap, "the release file is not signed by its own key");
        return -1;
    }
    return 0;
}

purr_repo_tier_t purr_repo_endorsement_tier(const purr_crypto_t *crypto, const purr_keybag_t *bag, const purr_repo_info_t *info)
{
    if (!info->has_endorsement) {
        return PURR_TIER_COMMUNITY;
    }
    const purr_key_t *k = bag != NULL ? purr_keybag_find(bag, info->end_key_id) : NULL;
    if (k == NULL || crypto == NULL) {
        return PURR_TIER_UNTRUSTED;
    }
    uint8_t msg[sizeof(PREFIX) + 32];                /* "PURRREPO" + its terminating NUL + the hash */
    memcpy(msg, PREFIX, sizeof(PREFIX));
    uint8_t h[32];
    purr_sha256(info->pub, PURR_PUBKEY_LEN, h);
    memcpy(msg + sizeof(PREFIX), h, 32);
    uint8_t digest[32];
    purr_sha256(msg, sizeof(msg), digest);
    return crypto->verify_p256(k->pub, digest, info->end_sig) ? PURR_TIER_OFFICIAL : PURR_TIER_UNTRUSTED;
}

int purr_repo_vouches(const purr_repo_index_t *idx, const char *name, const uint8_t sha256[32])
{
    for (int i = 0; i < idx->count; i++) {
        const purr_repo_app_t *a = &idx->apps[i];
        if (a->type == PURR_REPO_ITEM_APP && strcmp(a->name, name) == 0 && memcmp(a->sha256, sha256, 32) == 0) {
            return 1;
        }
    }
    return 0;
}

const char *purr_repo_tier_name(purr_repo_tier_t t)
{
    switch (t) {
    case PURR_TIER_OFFICIAL:  return "official";
    case PURR_TIER_COMMUNITY: return "community";
    case PURR_TIER_YOURS:     return "yours";
    default:                  return "untrusted";
    }
}

/* ------------------------------------------------------------------ /etc/repos.conf */

static int tier_from(const char *s)
{
    if (strcmp(s, "official") == 0) return PURR_TIER_OFFICIAL;
    if (strcmp(s, "community") == 0) return PURR_TIER_COMMUNITY;
    if (strcmp(s, "yours") == 0) return PURR_TIER_YOURS;
    return -1;
}

static void stanza_done(purr_repos_t *out, purr_repo_entry_t *e, int have_pub, int have_tier)
{
    char fp[PURR_RT_ID_HEX + 1];
    if (e->id[0] == '\0' && !have_pub) {
        return;                                       /* an empty stanza */
    }
    if (!have_pub || !have_tier || e->url[0] == '\0') {
        out->dropped++;
        return;
    }
    purr_repo_fingerprint(e->pub, fp);
    if (strcmp(fp, e->id) != 0 || out->count >= PURR_REPOS_MAX || purr_repos_find(out, e->id) != NULL) {
        out->dropped++;
        return;
    }
    out->e[out->count++] = *e;
}

int purr_repos_parse(const char *text, size_t len, purr_repos_t *out)
{
    memset(out, 0, sizeof(*out));
    purr_repo_entry_t e;
    memset(&e, 0, sizeof(e));
    int have_pub = 0, have_tier = 0;
    size_t i = 0;
    while (i <= len) {
        size_t s = i;
        while (i < len && text[i] != '\n') i++;
        size_t ll = i - s;
        i++;
        if (ll > 0 && text[s + ll - 1] == '\r') ll--;
        if (ll == 0) {                                 /* a blank line ends a stanza */
            stanza_done(out, &e, have_pub, have_tier);
            memset(&e, 0, sizeof(e));
            have_pub = have_tier = 0;
            continue;
        }
        if (text[s] == '#') continue;
        const char *eq = memchr(text + s, '=', ll);
        if (eq == NULL) continue;
        size_t kl = (size_t)(eq - (text + s)), vl = ll - kl - 1;
        const char *v = eq + 1;
        char val[160];
        if (vl >= sizeof(val)) continue;
        memcpy(val, v, vl);
        val[vl] = '\0';
        char key[12];
        if (kl >= sizeof(key)) continue;
        memcpy(key, text + s, kl);
        key[kl] = '\0';
        if (strcmp(key, "id") == 0 && vl == PURR_RT_ID_HEX) memcpy(e.id, val, PURR_RT_ID_HEX + 1);
        else if (strcmp(key, "name") == 0) snprintf(e.name, sizeof(e.name), "%s", val);
        else if (strcmp(key, "url") == 0) snprintf(e.url, sizeof(e.url), "%s", val);
        else if (strcmp(key, "pub") == 0) have_pub = hex_decode(val, vl, e.pub, PURR_PUBKEY_LEN) == 0;
        else if (strcmp(key, "tier") == 0) { int t = tier_from(val); if (t >= 0) { e.tier = (uint8_t)t; have_tier = 1; } }
        else if (strcmp(key, "layout") == 0) e.flat = strcmp(val, "tree") != 0;
        else if (strcmp(key, "added") == 0) e.added = (uint32_t)strtoul(val, NULL, 10);
    }
    stanza_done(out, &e, have_pub, have_tier);
    return out->count;
}

int purr_repos_format(const purr_repos_t *r, char *out, size_t cap)
{
    size_t n = 0;
    int w = snprintf(out, cap, "# /etc/repos.conf - the repositories an administrator trusted (Repositories/SPEC.md).\n");
    if (w < 0 || (size_t)w >= cap) return -1;
    n = (size_t)w;
    for (int i = 0; i < r->count; i++) {
        const purr_repo_entry_t *e = &r->e[i];
        char pub[PURR_PUBKEY_LEN * 2 + 1];
        hex_encode(e->pub, PURR_PUBKEY_LEN, pub);
        w = snprintf(out + n, cap - n, "\nid=%s\nname=%s\nurl=%s\npub=%s\ntier=%s\nlayout=%s\nadded=%u\n", e->id, e->name, e->url, pub,
                     purr_repo_tier_name((purr_repo_tier_t)e->tier), e->flat ? "flat" : "tree", (unsigned)e->added);
        if (w < 0 || (size_t)w >= cap - n) return -1;
        n += (size_t)w;
    }
    return (int)n;
}

const purr_repo_entry_t *purr_repos_find(const purr_repos_t *r, const char *id)
{
    for (int i = 0; i < r->count; i++) {
        if (strcmp(r->e[i].id, id) == 0) {
            return &r->e[i];
        }
    }
    return NULL;
}

purr_repo_add_class_t purr_repos_classify(const purr_repos_t *r, const purr_repo_info_t *info, const char *url)
{
    if (purr_repos_find(r, info->id) != NULL) {
        return PURR_RA_KNOWN;
    }
    for (int i = 0; i < r->count; i++) {
        if (strcmp(r->e[i].url, url) == 0) {
            return PURR_RA_REKEYED;
        }
    }
    return PURR_RA_NEW;
}

int purr_repos_remove(purr_repos_t *r, const char *id)
{
    for (int i = 0; i < r->count; i++) {
        if (strcmp(r->e[i].id, id) == 0) {
            memmove(&r->e[i], &r->e[i + 1], (size_t)(r->count - i - 1) * sizeof(r->e[0]));
            r->count--;
            return 0;
        }
    }
    return -1;
}

int purr_repos_add(purr_repos_t *r, const purr_repo_info_t *info, const char *url, purr_repo_tier_t tier, uint32_t now)
{
    for (int i = 0; i < r->count; i++) {               /* the same address under another key: the new fingerprint replaces the old */
        if (strcmp(r->e[i].url, url) == 0 && strcmp(r->e[i].id, info->id) != 0) {
            purr_repos_remove(r, r->e[i].id);
            break;
        }
    }
    purr_repo_entry_t *slot = NULL;
    for (int i = 0; i < r->count; i++) {
        if (strcmp(r->e[i].id, info->id) == 0) {
            slot = &r->e[i];
        }
    }
    if (slot == NULL) {
        if (r->count >= PURR_REPOS_MAX) {
            return -1;
        }
        slot = &r->e[r->count++];
        memset(slot, 0, sizeof(*slot));
        slot->added = now;
    }
    snprintf(slot->id, sizeof(slot->id), "%s", info->id);
    snprintf(slot->name, sizeof(slot->name), "%s", info->name);
    snprintf(slot->url, sizeof(slot->url), "%s", url);
    memcpy(slot->pub, info->pub, PURR_PUBKEY_LEN);
    slot->tier = (uint8_t)tier;
    slot->flat = info->flat;
    return 0;
}
