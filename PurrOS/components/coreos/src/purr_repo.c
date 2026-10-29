/*
 * purr_repo.c - the repository index, address policy and version logic (purr_repo.h).
 */
#include "purr_repo.h"

#include <stdio.h>
#include <string.h>

#include "purr_util.h"

/* ------------------------------------------------------------------ small helpers */

static void set_err(char *err, size_t cap, const char *msg)
{
    if (err != NULL && cap > 0) {
        snprintf(err, cap, "%s", msg);
    }
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        if (lower((unsigned char)*a) != lower((unsigned char)*b)) {
            return 0;
        }
    }
    return *a == *b;
}

static int is_alnum(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static int name_ok(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= PURR_REPO_NAME_LEN) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (!is_alnum((unsigned char)s[i]) && s[i] != '-' && s[i] != '_') {
            return 0;
        }
    }
    return 1;
}

static int version_ok(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= PURR_REPO_VERSION_LEN || s[0] < '0' || s[0] > '9') {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (!is_alnum((unsigned char)s[i]) && s[i] != '.' && s[i] != '-' && s[i] != '+') {
            return 0;
        }
    }
    return 1;
}

/* A path from an index or a join: plain characters only, nothing that climbs or redirects. */
static int rel_ok(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= PURR_REPO_URL_LEN || s[0] == '/' || s[0] == '.') {
        return 0;
    }
    if (strstr(s, "..") != NULL || strstr(s, "//") != NULL) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!is_alnum((unsigned char)c) && c != '.' && c != '_' && c != '-' && c != '/') {
            return 0;
        }
    }
    return 1;
}

/* A destination inside an asset folder: relative, plain characters, no climbing, not hidden. */
static int dest_ok(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= PURR_REPO_DEST_LEN || s[0] == '/' || s[0] == '.') {
        return 0;
    }
    if (strstr(s, "..") != NULL || strstr(s, "//") != NULL || strstr(s, "/.") != NULL) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!is_alnum((unsigned char)c) && c != '.' && c != '_' && c != '-' && c != '/') {
            return 0;
        }
    }
    return s[n - 1] != '/';
}

int purr_repo_hex_to_sha256(const char *hex, uint8_t out[32])
{
    if (strlen(hex) != 64) {
        return -1;
    }
    for (int i = 0; i < 32; i++) {
        int v[2];
        for (int k = 0; k < 2; k++) {
            char c = hex[i * 2 + k];
            if (c >= '0' && c <= '9') v[k] = c - '0';
            else if (c >= 'a' && c <= 'f') v[k] = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v[k] = c - 'A' + 10;
            else return -1;
        }
        out[i] = (uint8_t)((v[0] << 4) | v[1]);
    }
    return 0;
}

/* ------------------------------------------------------------------ a small JSON reader
 *
 * Just enough for an index: objects, arrays, strings, integers, booleans, null. Anything else is a
 * malformed document. Depth is bounded, nothing is allocated, and unknown fields are skipped so the
 * format can grow. */

typedef struct {
    const char *p, *end;
    int depth;
} jp_t;

#define JP_MAX_DEPTH 8

static void jp_ws(jp_t *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) {
        j->p++;
    }
}

static int jp_peek(jp_t *j)
{
    jp_ws(j);
    return j->p < j->end ? (unsigned char)*j->p : -1;
}

/* Reads a string into out. Returns its length; -1 if malformed; -2 if it did not fit (consumed anyway). */
static int jp_string(jp_t *j, char *out, size_t cap)
{
    if (jp_peek(j) != '"') {
        return -1;
    }
    j->p++;
    size_t n = 0;
    int too_long = 0;
    while (j->p < j->end && *j->p != '"') {
        unsigned char c = (unsigned char)*j->p++;
        if (c < 0x20) {
            return -1;
        }
        if (c == '\\') {
            if (j->p >= j->end) {
                return -1;
            }
            char e = *j->p++;
            switch (e) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u':                                   /* four hex digits; non-ASCII becomes '?' */
                if (j->end - j->p < 4) {
                    return -1;
                }
                j->p += 4;
                c = '?';
                break;
            default: return -1;
            }
        }
        if (out != NULL) {                              /* with no buffer the string is only being skipped */
            if (n + 1 < cap) {
                out[n] = (char)c;
            } else {
                too_long = 1;
            }
        }
        n++;
    }
    if (j->p >= j->end) {
        return -1;
    }
    j->p++;                                             /* closing quote */
    if (out != NULL && cap > 0) {
        out[too_long ? cap - 1 : n] = '\0';
    }
    return too_long ? -2 : (int)n;
}

static int jp_int(jp_t *j, long long *out)
{
    jp_ws(j);
    const char *s = j->p;
    int neg = 0;
    if (j->p < j->end && *j->p == '-') {
        neg = 1;
        j->p++;
    }
    long long v = 0;
    int digits = 0;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
        if (v < 1000000000000LL) {
            v = v * 10 + (*j->p - '0');
        }
        j->p++;
        digits++;
    }
    if (digits == 0) {
        j->p = s;
        return -1;
    }
    /* a fraction or exponent is read and dropped: an index carries whole numbers */
    if (j->p < j->end && *j->p == '.') {
        j->p++;
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
    }
    if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
        j->p++;
        if (j->p < j->end && (*j->p == '+' || *j->p == '-')) j->p++;
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
    }
    *out = neg ? -v : v;
    return 0;
}

static int jp_lit(jp_t *j, const char *word)
{
    size_t n = strlen(word);
    if ((size_t)(j->end - j->p) >= n && memcmp(j->p, word, n) == 0) {
        j->p += n;
        return 0;
    }
    return -1;
}

static int jp_skip(jp_t *j);

static int jp_skip_container(jp_t *j, char open, char close)
{
    if (++j->depth > JP_MAX_DEPTH) {
        return -1;
    }
    j->p++;                                             /* the opening bracket */
    if (jp_peek(j) == close) {
        j->p++;
        j->depth--;
        return 0;
    }
    for (;;) {
        if (open == '{') {
            if (jp_string(j, NULL, 0) < 0 || jp_peek(j) != ':') {
                return -1;
            }
            j->p++;
        }
        if (jp_skip(j) != 0) {
            return -1;
        }
        int c = jp_peek(j);
        if (c == ',') {
            j->p++;
            continue;
        }
        if (c == close) {
            j->p++;
            j->depth--;
            return 0;
        }
        return -1;
    }
}

static int jp_skip(jp_t *j)
{
    int c = jp_peek(j);
    long long dummy;
    switch (c) {
    case '"': return jp_string(j, NULL, 0) == -1 ? -1 : 0;
    case '{': return jp_skip_container(j, '{', '}');
    case '[': return jp_skip_container(j, '[', ']');
    case 't': return jp_lit(j, "true");
    case 'f': return jp_lit(j, "false");
    case 'n': return jp_lit(j, "null");
    default: return jp_int(j, &dummy);
    }
}

/* ------------------------------------------------------------------ the index */

/* Reads one app object. Returns 0 if parsed (*valid says whether the entry is usable), -1 if the JSON is bad. */
static int parse_app(jp_t *j, purr_repo_app_t *a, int *valid)
{
    char sha_hex[72] = "";
    int ok = 1, have_name = 0, have_ver = 0, have_sha = 0, have_url = 0, have_size = 0;
    memset(a, 0, sizeof(*a));
    snprintf(a->chip, sizeof(a->chip), "any");
    if (jp_peek(j) != '{') {
        return -1;
    }
    if (++j->depth > JP_MAX_DEPTH) {
        return -1;
    }
    j->p++;
    if (jp_peek(j) == '}') {
        j->p++;
        j->depth--;
        *valid = 0;
        return 0;
    }
    for (;;) {
        char key[24];
        int kl = jp_string(j, key, sizeof(key));
        if (kl == -1 || jp_peek(j) != ':') {
            return -1;
        }
        j->p++;
        if (kl == -2) {                                  /* a key too long to be one of ours */
            if (jp_skip(j) != 0) return -1;
        } else {
            int r = 0;
            long long n = 0;
            if (strcmp(key, "name") == 0)         { r = jp_string(j, a->name, sizeof(a->name)); have_name = r >= 0; }
            else if (strcmp(key, "title") == 0)   r = jp_string(j, a->title, sizeof(a->title));
            else if (strcmp(key, "version") == 0) { r = jp_string(j, a->version, sizeof(a->version)); have_ver = r >= 0; }
            else if (strcmp(key, "desc") == 0)    r = jp_string(j, a->desc, sizeof(a->desc));
            else if (strcmp(key, "url") == 0)     { r = jp_string(j, a->url, sizeof(a->url)); have_url = r >= 0; }
            else if (strcmp(key, "chip") == 0)    r = jp_string(j, a->chip, sizeof(a->chip));
            else if (strcmp(key, "sha256") == 0)  { r = jp_string(j, sha_hex, sizeof(sha_hex)); have_sha = r >= 0; }
            else if (strcmp(key, "size") == 0)    { if (jp_int(j, &n) != 0) return -1; a->size = (n > 0 && n < 0x7fffffff) ? (uint32_t)n : 0; have_size = 1; }
            else if (strcmp(key, "updated") == 0) { if (jp_int(j, &n) != 0) return -1; a->updated = n > 0 ? (uint32_t)n : 0; }
            else if (strcmp(key, "abi") == 0)     { if (jp_int(j, &n) != 0) return -1; a->abi = (n > 0 && n < 256) ? (uint8_t)n : 0; }
            else if (strcmp(key, "key_id") == 0)  { if (jp_int(j, &n) != 0) return -1; a->key_id = (n > 0 && n < 256) ? (uint8_t)n : 0; }
            else if (strcmp(key, "type") == 0) {
                char v[8];
                r = jp_string(j, v, sizeof(v));
                if (r >= 0) {
                    if (strcmp(v, "app") == 0) a->type = PURR_REPO_ITEM_APP;
                    else if (strcmp(v, "asset") == 0) a->type = PURR_REPO_ITEM_ASSET;
                    else if (strcmp(v, "extra") == 0) a->type = PURR_REPO_ITEM_EXTRA;
                    else ok = 0;                          /* a kind of item this OS does not know */
                } else if (r == -2) ok = 0;
            }
            else if (strcmp(key, "target") == 0) {
                char v[8];
                r = jp_string(j, v, sizeof(v));
                if (r >= 0) {
                    if (strcmp(v, "flash") == 0) a->target = PURR_REPO_TARGET_FLASH;
                    else if (strcmp(v, "sd") == 0) a->target = PURR_REPO_TARGET_SD;
                    else if (strcmp(v, "any") == 0) a->target = PURR_REPO_TARGET_ANY;
                    else ok = 0;
                } else if (r == -2) ok = 0;
            }
            else if (strcmp(key, "dest") == 0)    r = jp_string(j, a->dest, sizeof(a->dest));
            else if (strcmp(key, "min_os") == 0)  r = jp_string(j, a->min_os, sizeof(a->min_os));
            else if (strcmp(key, "category") == 0) r = jp_string(j, a->category, sizeof(a->category));
            else if (strcmp(key, "perms") == 0) {          /* an array of names, kept as one comma separated line (cut off if it is long) */
                if (jp_peek(j) != '[') return -1;
                j->p++;
                size_t pn = 0;
                if (jp_peek(j) != ']') {
                    for (;;) {
                        char one[24];
                        int pl = jp_string(j, one, sizeof(one));
                        if (pl == -1) return -1;
                        if (pl >= 0 && pn + 1 + (size_t)pl < sizeof(a->perms)) {
                            if (pn > 0) a->perms[pn++] = ',';
                            memcpy(a->perms + pn, one, (size_t)pl);
                            pn += (size_t)pl;
                            a->perms[pn] = '\0';
                        }
                        int c = jp_peek(j);
                        if (c == ',') { j->p++; continue; }
                        if (c == ']') break;
                        return -1;
                    }
                }
                j->p++;
            }
            else if (strcmp(key, "needs_sd") == 0) { if (jp_int(j, &n) != 0) return -1; a->needs_sd = n != 0; }
            else if (strcmp(key, "signed") == 0) {
                int c = jp_peek(j);
                if (c == 't') { if (jp_lit(j, "true") != 0) return -1; a->signed_claim = 1; }
                else if (c == 'f') { if (jp_lit(j, "false") != 0) return -1; a->signed_claim = 0; }
                else return -1;
            } else {
                if (jp_skip(j) != 0) return -1;
            }
            if (r == -1) return -1;                      /* malformed JSON */
            if (r == -2) ok = 0;                         /* a field too long: the entry is unusable */
        }
        int c = jp_peek(j);
        if (c == ',') { j->p++; continue; }
        if (c == '}') { j->p++; j->depth--; break; }
        return -1;
    }
    if (!have_name || !have_ver || !have_sha || !have_url || !have_size) ok = 0;
    if (ok && (!name_ok(a->name) || !version_ok(a->version) || !rel_ok(a->url) ||
               a->size == 0 || a->size > PURR_REPO_MAX_PACKAGE ||
               purr_repo_hex_to_sha256(sha_hex, a->sha256) != 0)) {
        ok = 0;
    }
    if (ok && a->type != PURR_REPO_ITEM_APP && !dest_ok(a->dest)) {
        ok = 0;                                          /* an asset or extra must say where it goes, and plainly */
    }
    if (ok && a->type == PURR_REPO_ITEM_APP && a->dest[0] != '\0') {
        ok = 0;
    }
    if (ok && a->title[0] == '\0') {
        snprintf(a->title, sizeof(a->title), "%s", a->name);
    }
    *valid = ok;
    return 0;
}

int purr_repo_index_parse(const char *json, size_t len, purr_repo_index_t *out,
                          const purr_repo_kind_t *kind, char *err, size_t errcap)
{
    memset(out, 0, sizeof(*out));
    if (json == NULL || len == 0 || len > PURR_REPO_MAX_INDEX) {
        set_err(err, errcap, len > PURR_REPO_MAX_INDEX ? "the index is too large" : "the index is empty");
        return -1;
    }
    jp_t j = {json, json + len, 0};
    int have_kind = 0, have_format = 0;
    if (jp_peek(&j) != '{') {
        set_err(err, errcap, "the index is not a JSON object");
        return -1;
    }
    j.p++;
    j.depth = 1;
    if (jp_peek(&j) != '}') {
        for (;;) {
            char key[16];
            int kl = jp_string(&j, key, sizeof(key));
            if (kl == -1 || jp_peek(&j) != ':') {
                set_err(err, errcap, "the index is malformed");
                return -1;
            }
            j.p++;
            long long n;
            if (kl >= 0 && strcmp(key, "format") == 0) {
                if (jp_int(&j, &n) != 0) { set_err(err, errcap, "the index is malformed"); return -1; }
                out->format = (int)n;
                have_format = 1;
            } else if (kl >= 0 && strcmp(key, "repo") == 0) {
                if (jp_peek(&j) != '{') { set_err(err, errcap, "the index is malformed"); return -1; }
                j.p++;
                j.depth++;
                if (jp_peek(&j) != '}') {
                    for (;;) {
                        char rk[12], val[40];
                        int rl = jp_string(&j, rk, sizeof(rk));
                        if (rl == -1 || jp_peek(&j) != ':') { set_err(err, errcap, "the index is malformed"); return -1; }
                        j.p++;
                        if (rl >= 0 && strcmp(rk, "id") == 0) {
                            if (jp_string(&j, out->id, sizeof(out->id)) == -1) { set_err(err, errcap, "the index is malformed"); return -1; }
                        } else if (rl >= 0 && strcmp(rk, "name") == 0) {
                            if (jp_string(&j, out->name, sizeof(out->name)) == -1) { set_err(err, errcap, "the index is malformed"); return -1; }
                        } else if (rl >= 0 && strcmp(rk, "kind") == 0) {
                            int vl = jp_string(&j, val, sizeof(val));
                            if (vl == -1) { set_err(err, errcap, "the index is malformed"); return -1; }
                            if (strcmp(val, "sdk") == 0) { out->kind = PURR_REPO_SDK; have_kind = 1; }
                            else if (strcmp(val, "store") == 0) { out->kind = PURR_REPO_STORE; have_kind = 1; }
                            else { set_err(err, errcap, "unknown repository kind"); return -1; }
                        } else if (jp_skip(&j) != 0) {
                            set_err(err, errcap, "the index is malformed");
                            return -1;
                        }
                        int c = jp_peek(&j);
                        if (c == ',') { j.p++; continue; }
                        if (c == '}') break;
                        set_err(err, errcap, "the index is malformed");
                        return -1;
                    }
                }
                j.p++;                                   /* the closing brace of "repo" */
                j.depth--;
            } else if (kl >= 0 && strcmp(key, "apps") == 0) {
                if (jp_peek(&j) != '[') { set_err(err, errcap, "the index is malformed"); return -1; }
                j.p++;
                j.depth++;
                if (jp_peek(&j) != ']') {
                    for (;;) {
                        purr_repo_app_t a;
                        int valid = 0;
                        if (parse_app(&j, &a, &valid) != 0) { set_err(err, errcap, "the index is malformed"); return -1; }
                        if (valid && purr_repo_index_find(out, a.name) != NULL) {
                            valid = 0;                    /* a repeated name: the first one stands */
                        }
                        if (valid && out->count >= PURR_REPO_MAX_APPS) {
                            valid = 0;
                        }
                        if (valid) {
                            out->apps[out->count++] = a;
                        } else {
                            out->dropped++;
                        }
                        int c = jp_peek(&j);
                        if (c == ',') { j.p++; continue; }
                        if (c == ']') break;
                        set_err(err, errcap, "the index is malformed");
                        return -1;
                    }
                }
                j.p++;                                   /* the closing bracket of "apps" */
                j.depth--;
            } else if (jp_skip(&j) != 0) {
                set_err(err, errcap, "the index is malformed");
                return -1;
            }
            int c = jp_peek(&j);
            if (c == ',') { j.p++; continue; }
            if (c == '}') break;
            set_err(err, errcap, "the index is malformed");
            return -1;
        }
    }
    if (!have_format || (out->format != PURR_REPO_FORMAT && out->format != PURR_REPO_FORMAT_STORE)) {
        set_err(err, errcap, "unsupported index format");
        return -1;
    }
    if (kind != NULL) {
        /* An index may not claim to be more trusted than where it was fetched from asked for. */
        if ((!have_kind && *kind == PURR_REPO_STORE) || (have_kind && out->kind != *kind)) {
            set_err(err, errcap, "the repository is not the kind that was expected");
            return -1;
        }
        out->kind = *kind;
        if (out->format == PURR_REPO_FORMAT_STORE && *kind != PURR_REPO_STORE) {
            set_err(err, errcap, "only a store repository uses format 2");
            return -1;
        }
        for (int i = 0; i < out->count; i++) {            /* a signed format-2 index stands for its packages */
            out->apps[i].vouched = out->format == PURR_REPO_FORMAT_STORE && *kind == PURR_REPO_STORE;
        }
        int kept = 0;
        for (int i = 0; i < out->count; i++) {
            if (purr_repo_app_allowed(*kind, &out->apps[i], NULL, 0)) {
                out->apps[kept++] = out->apps[i];
            } else {
                out->dropped++;
            }
        }
        out->count = kept;
    }
    return 0;
}

const purr_repo_app_t *purr_repo_index_find(const purr_repo_index_t *idx, const char *name)
{
    for (int i = 0; i < idx->count; i++) {
        if (strcmp(idx->apps[i].name, name) == 0) {
            return &idx->apps[i];
        }
    }
    return NULL;
}

int purr_repo_app_allowed(purr_repo_kind_t kind, const purr_repo_app_t *app, char *why, size_t whycap)
{
    if (kind == PURR_REPO_STORE && !app->signed_claim && !app->vouched) {
        set_err(why, whycap, "a store only offers signed apps");
        return 0;
    }
    return 1;
}

const char *purr_repo_pkg_result_name(purr_repo_pkg_result_t r)
{
    switch (r) {
    case PURR_REPO_PKG_OK:       return "ok";
    case PURR_REPO_PKG_BAD_SIZE: return "wrong size";
    case PURR_REPO_PKG_BAD_HASH: return "wrong hash";
    }
    return "?";
}

purr_repo_pkg_result_t purr_repo_check_package(const purr_repo_app_t *app, const uint8_t *data, size_t len)
{
    if (len != app->size) {
        return PURR_REPO_PKG_BAD_SIZE;
    }
    uint8_t digest[32];
    purr_sha256(data, len, digest);
    return memcmp(digest, app->sha256, 32) == 0 ? PURR_REPO_PKG_OK : PURR_REPO_PKG_BAD_HASH;
}

/* ------------------------------------------------------------------ addresses */

int purr_repo_ip_is_local(uint32_t ip)
{
    uint8_t a = (uint8_t)(ip >> 24), b = (uint8_t)(ip >> 16);
    return a == 10 || a == 127 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
           (a == 169 && b == 254);
}

/* A host made only of digits and dots must be a clean dotted quad ("127.1" and "2130706433" are
 * address spellings some resolvers accept and a policy check must not be fooled by). Returns 1 and the
 * address, 0 if the host is a name, -1 if it looks numeric but is not a clean quad. */
static int host_ipv4(const char *host, uint32_t *out)
{
    int numeric = 1;
    for (const char *p = host; *p; p++) {
        if (!((*p >= '0' && *p <= '9') || *p == '.')) {
            numeric = 0;
        }
    }
    if (!numeric || host[0] == '\0') {
        return 0;
    }
    uint32_t v = 0;
    int parts = 0;
    const char *p = host;
    while (*p) {
        if (parts == 4 || *p < '0' || *p > '9') {
            return -1;
        }
        const char *start = p;
        unsigned n = 0;
        while (*p >= '0' && *p <= '9') {
            n = n * 10 + (unsigned)(*p - '0');
            if (n > 255) {
                return -1;
            }
            p++;
        }
        if (p - start > 1 && *start == '0') {
            return -1;                                   /* leading zeros: octal in some resolvers */
        }
        v = (v << 8) | n;
        parts++;
        if (*p == '.') {
            p++;
            if (*p == '\0') {
                return -1;
            }
        }
    }
    if (parts != 4) {
        return -1;
    }
    *out = v;
    return 1;
}

int purr_repo_url_split(const char *url, char *scheme, size_t scheme_cap, char *host, size_t host_cap,
                        uint16_t *port, char *path, size_t path_cap)
{
    if (url == NULL) {
        return -1;
    }
    for (const char *p = url; *p; p++) {
        if ((unsigned char)*p <= ' ' || (unsigned char)*p >= 0x7f || *p == '\\') {
            return -1;
        }
    }
    const char *rest;
    char sch[8];
    if (strncmp(url, "http://", 7) == 0 || strncmp(url, "HTTP://", 7) == 0) {
        snprintf(sch, sizeof(sch), "http");
        rest = url + 7;
    } else if (strncmp(url, "https://", 8) == 0 || strncmp(url, "HTTPS://", 8) == 0) {
        snprintf(sch, sizeof(sch), "https");
        rest = url + 8;
    } else {
        return -1;
    }
    size_t alen = strcspn(rest, "/?#");
    if (alen == 0 || rest[alen] == '?' || rest[alen] == '#') {
        return -1;
    }
    char auth[96];
    if (alen >= sizeof(auth)) {
        return -1;
    }
    memcpy(auth, rest, alen);
    auth[alen] = '\0';
    if (strchr(auth, '@') != NULL || strchr(auth, '[') != NULL) {
        return -1;                                       /* userinfo tricks, IPv6 literals */
    }
    unsigned long pn = strcmp(sch, "https") == 0 ? 443 : 80;
    char *colon = strchr(auth, ':');
    if (colon != NULL) {
        *colon++ = '\0';
        if (*colon == '\0' || strlen(colon) > 5) {
            return -1;
        }
        pn = 0;
        for (const char *q = colon; *q; q++) {
            if (*q < '0' || *q > '9') {
                return -1;
            }
            pn = pn * 10 + (unsigned long)(*q - '0');
        }
        if (pn == 0 || pn > 65535) {
            return -1;
        }
    }
    if (auth[0] == '\0') {
        return -1;
    }
    for (const char *q = auth; *q; q++) {
        if (!is_alnum((unsigned char)*q) && *q != '.' && *q != '-') {
            return -1;
        }
    }
    const char *pth = rest[alen] == '/' ? rest + alen : "/";
    if (strlen(pth) >= path_cap || strlen(auth) >= host_cap || strlen(sch) >= scheme_cap) {
        return -1;
    }
    snprintf(scheme, scheme_cap, "%s", sch);
    for (char *q = scheme; *q; q++) *q = (char)lower((unsigned char)*q);
    snprintf(host, host_cap, "%s", auth);
    snprintf(path, path_cap, "%s", pth);
    *port = (uint16_t)pn;
    return 0;
}

int purr_repo_url_allowed(purr_repo_kind_t kind, const char *url, char *why, size_t whycap)
{
    char scheme[8], host[96], path[96];
    uint16_t port;
    if (purr_repo_url_split(url, scheme, sizeof(scheme), host, sizeof(host), &port, path, sizeof(path)) != 0) {
        set_err(why, whycap, "not a valid http(s) address");
        return 0;
    }
    uint32_t ip = 0;
    int is_ip = host_ipv4(host, &ip);
    if (is_ip < 0) {
        set_err(why, whycap, "not a clean IPv4 address");
        return 0;
    }
    if (kind == PURR_REPO_STORE) {
        if (strcmp(scheme, "https") != 0) {
            set_err(why, whycap, "a store must use https");
            return 0;
        }
        return 1;
    }
    /* SDK: the local network only. A name has to be localhost or an mDNS .local name; either way the
     * caller checks the resolved address with purr_repo_ip_is_local() before it connects. */
    if (is_ip == 1) {
        if (!purr_repo_ip_is_local(ip)) {
            set_err(why, whycap, "an SDK server must be on the local network");
            return 0;
        }
        return 1;
    }
    size_t hl = strlen(host);
    if (ieq(host, "localhost") || (hl > 6 && ieq(host + hl - 6, ".local"))) {
        return 1;
    }
    set_err(why, whycap, "an SDK server must be an address on the local network, or a .local name");
    return 0;
}

int purr_repo_url_join(const char *base, const char *rel, char *out, size_t cap)
{
    char scheme[8], host[96], path[96];
    uint16_t port;
    if (purr_repo_url_split(base, scheme, sizeof(scheme), host, sizeof(host), &port, path, sizeof(path)) != 0 ||
        !rel_ok(rel)) {
        return -1;
    }
    /* the repository's own path prefix (if any) is kept; the protocol path follows it */
    size_t pl = strlen(path);
    while (pl > 1 && path[pl - 1] == '/') {
        path[--pl] = '\0';
    }
    int n = snprintf(out, cap, "%s://%s:%u%s%s/purr/v1/%s", scheme, host, (unsigned)port,
                     strcmp(path, "/") == 0 ? "" : path, "", rel);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* ------------------------------------------------------------------ versions */

static int parse_ver(const char *s, unsigned parts[6], int *pre)
{
    memset(parts, 0, 6 * sizeof(unsigned));
    *pre = 0;
    if (*s == 'v' || *s == 'V') {
        s++;
    }
    int i = 0;
    while (*s && i < 6) {
        if (*s < '0' || *s > '9') {
            break;
        }
        unsigned n = 0;
        while (*s >= '0' && *s <= '9') {
            if (n < 100000000u) n = n * 10 + (unsigned)(*s - '0');
            s++;
        }
        parts[i++] = n;
        if (*s == '.') {
            s++;
        } else {
            break;
        }
    }
    if (*s == '-') {
        *pre = 1;
    }
    return i;
}

int purr_repo_version_cmp(const char *a, const char *b)
{
    unsigned pa[6], pb[6];
    int prea, preb;
    parse_ver(a, pa, &prea);
    parse_ver(b, pb, &preb);
    for (int i = 0; i < 6; i++) {
        if (pa[i] != pb[i]) {
            return pa[i] < pb[i] ? -1 : 1;
        }
    }
    if (prea != preb) {
        return prea ? -1 : 1;
    }
    return 0;
}

int purr_repo_updates(const purr_repo_index_t *idx, const purr_repo_installed_t *installed, int n,
                      int *out, int cap)
{
    int found = 0;
    for (int i = 0; i < n && found < cap; i++) {
        for (int k = 0; k < idx->count; k++) {
            if (strcmp(idx->apps[k].name, installed[i].name) == 0) {
                if (purr_repo_version_cmp(idx->apps[k].version, installed[i].version) > 0) {
                    out[found++] = k;
                }
                break;
            }
        }
    }
    return found;
}
