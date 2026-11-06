#include "purr_move.h"

#include <string.h>

#include "purr_repo.h"
#include "purr_repo_trust.h"

static int copy_field(char *dst, size_t cap, const char *v, size_t vl, int printable_only)
{
    if (vl == 0 || vl >= cap) {
        return -1;
    }
    for (size_t i = 0; i < vl; i++) {
        unsigned char c = (unsigned char)v[i];
        if (c < 32 || c > 126 || (!printable_only && c == ' ')) {
            return -1;
        }
    }
    memcpy(dst, v, vl);
    dst[vl] = '\0';
    return 0;
}

static int parse_u32(const char *v, size_t vl, uint32_t *out)
{
    if (vl == 0 || vl > 10) {
        return -1;
    }
    uint64_t n = 0;
    for (size_t i = 0; i < vl; i++) {
        if (v[i] < '0' || v[i] > '9') {
            return -1;
        }
        n = n * 10 + (uint64_t)(v[i] - '0');
    }
    if (n > 0xFFFFFFFFu) {
        return -1;
    }
    *out = (uint32_t)n;
    return 0;
}

int purr_move_parse(const char *text, size_t len, purr_move_t *out)
{
    memset(out, 0, sizeof(*out));
    if (text == NULL || len == 0 || len > PURR_MOVE_MAX_TEXT) {
        return -1;
    }
    int have_fmt = 0, have_seq = 0, have_key = 0, have_from = 0, have_rec = 0, have_mod = 0;
    const char *p = text, *end = text + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        const char *line_end = le;
        if (line_end > p && line_end[-1] == '\r') {
            line_end--;
        }
        const char *eq = memchr(p, '=', (size_t)(line_end - p));
        if (eq != NULL && eq > p) {
            size_t kl = (size_t)(eq - p), vl = (size_t)(line_end - eq - 1);
            const char *v = eq + 1;
            uint32_t n;
            if (kl == 5 && memcmp(p, "moved", 5) == 0) {
                if (parse_u32(v, vl, &n) != 0 || n != 1) {
                    return -1;
                }
                have_fmt = 1;
            } else if (kl == 3 && memcmp(p, "seq", 3) == 0) {
                if (parse_u32(v, vl, &out->seq) != 0) {
                    return -1;
                }
                have_seq = 1;
            } else if (kl == 6 && memcmp(p, "key_id", 6) == 0) {
                if (parse_u32(v, vl, &n) != 0 || n == 0 || n > 255) {
                    return -1;
                }
                out->key_id = (uint8_t)n;
                have_key = 1;
            } else if (kl == 4 && memcmp(p, "from", 4) == 0) {
                if (copy_field(out->from, sizeof(out->from), v, vl, 0) != 0) {
                    return -1;
                }
                have_from = 1;
            } else if (kl == 8 && memcmp(p, "recovery", 8) == 0) {
                if (copy_field(out->recovery, sizeof(out->recovery), v, vl, 0) != 0) {
                    return -1;
                }
                have_rec = 1;
            } else if (kl == 7 && memcmp(p, "modules", 7) == 0) {
                if (copy_field(out->modules, sizeof(out->modules), v, vl, 0) != 0) {
                    return -1;
                }
                have_mod = 1;
            } else if (kl == 7 && memcmp(p, "message", 7) == 0) {
                if (vl > 0 && copy_field(out->message, sizeof(out->message), v, vl, 1) != 0) {
                    return -1;
                }
            }
        }
        p = nl ? nl + 1 : end;
    }
    return have_fmt && have_seq && have_key && have_from && have_rec && have_mod ? 0 : -1;
}

static int ends_with(const char *s, const char *suffix)
{
    size_t sl = strlen(s), xl = strlen(suffix);
    return sl >= xl && strcmp(s + sl - xl, suffix) == 0;
}

purr_move_result_t purr_move_check(const purr_crypto_t *crypto, const purr_keybag_t *bag, const char *text, size_t len, const uint8_t sig[PURR_SIG_LEN],
                                   const char *asked, uint32_t stored_seq, purr_move_t *out)
{
    if (purr_move_parse(text, len, out) != 0) {
        return PURR_MOVE_BAD_FORMAT;
    }
    const purr_key_t *k = purr_keybag_find(bag, out->key_id);
    if (k == NULL) {
        return PURR_MOVE_NO_KEY;
    }
    if (k->role != PURR_ROLE_BOOT) {
        return PURR_MOVE_WRONG_ROLE;
    }
    if (purr_keybag_is_revoked(bag, out->key_id)) {
        return PURR_MOVE_REVOKED;
    }
    if (purr_repo_check_signed(crypto, k->pub, text, len, sig) != 0) {
        return PURR_MOVE_BAD_SIGNATURE;
    }
    if (asked == NULL || strcmp(out->from, asked) != 0) {
        return PURR_MOVE_WRONG_FROM;
    }
    if (out->seq <= stored_seq) {
        return PURR_MOVE_OLD_SEQ;
    }
    char why[64];
    if (!purr_repo_url_allowed(PURR_REPO_STORE, out->recovery, why, sizeof(why)) || !purr_repo_url_allowed(PURR_REPO_STORE, out->modules, why, sizeof(why)) ||
        !ends_with(out->recovery, "/recovery.manifest") || !ends_with(out->modules, "/modules.manifest")) {
        return PURR_MOVE_BAD_URL;
    }
    return PURR_MOVE_OK;
}

int purr_move_is_default(const purr_move_t *m, const char *default_recovery, const char *default_modules)
{
    return strcmp(m->recovery, default_recovery) == 0 && strcmp(m->modules, default_modules) == 0;
}

const char *purr_move_result_name(purr_move_result_t r)
{
    switch (r) {
    case PURR_MOVE_OK: return "ok";
    case PURR_MOVE_BAD_FORMAT: return "not a well-formed move file";
    case PURR_MOVE_NO_KEY: return "signed with a key this device does not have";
    case PURR_MOVE_WRONG_ROLE: return "signed with a key that may not sign a move";
    case PURR_MOVE_REVOKED: return "signed with a revoked key";
    case PURR_MOVE_BAD_SIGNATURE: return "the signature does not verify";
    case PURR_MOVE_WRONG_FROM: return "meant for another address";
    case PURR_MOVE_OLD_SEQ: return "not newer than the move already followed";
    case PURR_MOVE_BAD_URL: return "the new address is not acceptable";
    }
    return "?";
}
