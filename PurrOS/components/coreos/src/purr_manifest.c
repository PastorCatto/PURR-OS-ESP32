#include "purr_manifest.h"

#include <stdlib.h>
#include <string.h>

/* This runs in CoreOS and the recovery loader (both real ESP-IDF apps), not the boot
 * package, so plain libc is fine here, unlike purr_menu and purr_wifi. */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_sha256(const char *s, size_t len, uint8_t out[32])
{
    if (len != 64) {
        return 0;
    }
    for (int i = 0; i < 32; i++) {
        int hi = hex_val(s[i * 2]);
        int lo = hex_val(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return 0;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

static void set_field(char *dst, size_t dstsize, const char *val, size_t len)
{
    if (len >= dstsize) {
        len = dstsize - 1;
    }
    memcpy(dst, val, len);
    dst[len] = '\0';
}

/* A stanza being accumulated, plus whether it looked like a component at all. */
typedef struct {
    purr_manifest_entry_t e;
    int have_component, have_version, have_file, have_size, have_sha256;
} draft_t;

static void apply_kv(purr_manifest_t *m, draft_t *d, const char *key, size_t klen,
                     const char *val, size_t vlen)
{
    if (klen == 7 && strncmp(key, "release", 7) == 0) {
        set_field(m->release, sizeof(m->release), val, vlen);
    } else if (klen == 8 && strncmp(key, "released", 8) == 0) {
        set_field(m->released, sizeof(m->released), val, vlen);
    } else if (klen == 9 && strncmp(key, "component", 9) == 0) {
        set_field(d->e.component, sizeof(d->e.component), val, vlen);
        d->have_component = 1;
    } else if (klen == 4 && strncmp(key, "type", 4) == 0) {
        set_field(d->e.type, sizeof(d->e.type), val, vlen);
    } else if (klen == 7 && strncmp(key, "version", 7) == 0) {
        set_field(d->e.version, sizeof(d->e.version), val, vlen);
        d->have_version = 1;
    } else if (klen == 4 && strncmp(key, "chip", 4) == 0) {
        set_field(d->e.chip, sizeof(d->e.chip), val, vlen);
    } else if (klen == 5 && strncmp(key, "board", 5) == 0) {
        set_field(d->e.board, sizeof(d->e.board), val, vlen);
    } else if (klen == 4 && strncmp(key, "file", 4) == 0) {
        set_field(d->e.file, sizeof(d->e.file), val, vlen);
        d->have_file = 1;
    } else if (klen == 4 && strncmp(key, "size", 4) == 0) {
        char tmp[16];
        set_field(tmp, sizeof(tmp), val, vlen);
        char *end = NULL;
        unsigned long n = strtoul(tmp, &end, 10);
        if (end != tmp && *end == '\0') {
            d->e.size = (uint32_t)n;
            d->have_size = 1;
        }
    } else if (klen == 6 && strncmp(key, "sha256", 6) == 0) {
        d->have_sha256 = parse_sha256(val, vlen, d->e.sha256);
    } else if (klen == 3 && strncmp(key, "key", 3) == 0) {
        set_field(d->e.key_role, sizeof(d->e.key_role), val, vlen);
    } else if (klen == 14 && strncmp(key, "min_bootloader", 14) == 0) {
        set_field(d->e.min_bootloader, sizeof(d->e.min_bootloader), val, vlen);
    } else if (klen == 10 && strncmp(key, "min_coreos", 10) == 0) {
        set_field(d->e.min_coreos, sizeof(d->e.min_coreos), val, vlen);
    } else if (klen == 12 && strncmp(key, "payload_size", 12) == 0) {
        char tmp[16];
        set_field(tmp, sizeof(tmp), val, vlen);
        char *end = NULL;
        unsigned long n = strtoul(tmp, &end, 10);
        if (end != tmp && *end == '\0') {
            d->e.payload_size = (uint32_t)n;
        }
    } else if (klen == 14 && strncmp(key, "payload_sha256", 14) == 0) {
        d->e.have_payload_sha256 = parse_sha256(val, vlen, d->e.payload_sha256);
    }
    /* Anything else is a newer field this reader does not know about yet: ignored. */
}

static void commit(purr_manifest_t *m, draft_t *d)
{
    if (!d->have_component) {
        memset(d, 0, sizeof(*d));
        return;                                    /* the header stanza, or a blank run: not an entry */
    }
    if (!(d->have_version && d->have_file && d->have_size && d->have_sha256) || d->e.size == 0) {
        m->dropped++;
        memset(d, 0, sizeof(*d));
        return;
    }
    if (d->e.chip[0] == '\0') {
        strcpy(d->e.chip, "any");
    }
    if (d->e.board[0] == '\0') {
        strcpy(d->e.board, "any");
    }
    if (m->count < PURR_MANIFEST_MAX_ENTRIES) {
        m->entries[m->count++] = d->e;
    } else {
        m->truncated = 1;
    }
    memset(d, 0, sizeof(*d));
}

int purr_manifest_parse(purr_manifest_t *m, const char *text, size_t len)
{
    memset(m, 0, sizeof(*m));
    draft_t d;
    memset(&d, 0, sizeof(d));

    size_t i = 0;
    while (i <= len) {
        size_t start = i;
        while (i < len && text[i] != '\n') {
            i++;
        }
        size_t line_len = i - start;
        if (line_len > 0 && text[start + line_len - 1] == '\r') {
            line_len--;
        }
        i++;                                        /* past the newline, or past the end */

        /* Trim leading spaces and tabs. */
        const char *p = text + start;
        size_t n = line_len;
        while (n > 0 && (*p == ' ' || *p == '\t')) {
            p++;
            n--;
        }

        if (n == 0) {
            commit(m, &d);
        } else if (p[0] != '#') {
            const char *eq = memchr(p, '=', n);
            if (eq != NULL) {
                size_t klen = (size_t)(eq - p);
                apply_kv(m, &d, p, klen, eq + 1, n - klen - 1);
            }
            /* A line with no '=' is not a key/value pair: ignored, not an error. */
        }
        if (i > len) {
            break;
        }
    }
    commit(m, &d);                                  /* the file need not end with a blank line */
    return m->count;
}

static int matches(const char *field, const char *want)
{
    return want == NULL || strcmp(field, "any") == 0 || strcmp(field, want) == 0;
}

const purr_manifest_entry_t *purr_manifest_find(const purr_manifest_t *m, const char *component,
                                                const char *chip, const char *board)
{
    for (int i = 0; i < m->count; i++) {
        const purr_manifest_entry_t *e = &m->entries[i];
        if (strcmp(e->component, component) == 0 && matches(e->chip, chip) && matches(e->board, board)) {
            return e;
        }
    }
    return NULL;
}
