#include "purr_users.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "purr_pbkdf2.h"

/* ---------------------------------------------------------------- small helpers */

static int has_bad_char(const char *s)
{
    if (s[0] == '\0') {
        return 1;
    }
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '\t' || s[i] == '\n' || s[i] == '\r') {
            return 1;
        }
    }
    return 0;
}

static void hex_encode(const uint8_t *bytes, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0xF];
    }
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *s, size_t len, uint8_t *out, size_t n)
{
    if (len != n * 2) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        int hi = hex_val(s[i * 2]), lo = hex_val(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return 0;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

/* ---------------------------------------------------------------- accounts */

void purr_user_list_init(purr_user_list_t *l)
{
    memset(l, 0, sizeof(*l));
}

uint8_t purr_user_next_uid(const purr_user_list_t *l)
{
    for (int uid = 1; uid <= 255; uid++) {
        int taken = 0;
        for (int i = 0; i < l->count; i++) {
            if (l->users[i].uid == (uint8_t)uid) {
                taken = 1;
                break;
            }
        }
        if (!taken) {
            return (uint8_t)uid;
        }
    }
    return 0;
}

purr_user_t *purr_user_list_find(purr_user_list_t *l, const char *name)
{
    for (int i = 0; i < l->count; i++) {
        if (strcmp(l->users[i].name, name) == 0) {
            return &l->users[i];
        }
    }
    return NULL;
}

purr_user_t *purr_user_list_find_uid(purr_user_list_t *l, uint8_t uid)
{
    for (int i = 0; i < l->count; i++) {
        if (l->users[i].uid == uid) {
            return &l->users[i];
        }
    }
    return NULL;
}

int purr_user_list_add(purr_user_list_t *l, const char *name, uint8_t uid, purr_user_role_t role)
{
    if (has_bad_char(name) || uid == 0) {
        return PURR_USER_INVALID;
    }
    if (purr_user_list_find(l, name) != NULL) {
        return PURR_USER_EXISTS;
    }
    if (l->count >= PURR_USER_MAX) {
        return PURR_USER_FULL;
    }
    purr_user_t *u = &l->users[l->count];
    memset(u, 0, sizeof(*u));
    strncpy(u->name, name, sizeof(u->name) - 1);
    u->uid = uid;
    u->role = role;
    l->count++;
    return PURR_USER_ADDED;
}

int purr_user_list_remove(purr_user_list_t *l, const char *name)
{
    for (int i = 0; i < l->count; i++) {
        if (strcmp(l->users[i].name, name) == 0) {
            for (int j = i; j < l->count - 1; j++) {
                l->users[j] = l->users[j + 1];
            }
            l->count--;
            return 1;
        }
    }
    return 0;
}

int purr_user_is_last_admin(const purr_user_list_t *l, const char *name)
{
    int admins = 0;
    int this_is_admin = 0;
    for (int i = 0; i < l->count; i++) {
        if (l->users[i].role == PURR_ROLE_USER_ADMIN) {
            admins++;
            if (strcmp(l->users[i].name, name) == 0) {
                this_is_admin = 1;
            }
        }
    }
    return this_is_admin && admins <= 1;
}

int purr_user_list_format(const purr_user_list_t *l, char *buf, size_t bufsize)
{
    size_t pos = 0;
    for (int i = 0; i < l->count; i++) {
        const purr_user_t *u = &l->users[i];
        char line[PURR_USER_NAME_LEN + 48];
        int n = snprintf(line, sizeof(line), "%s\t%u\t%u\t%u\t%u\n", u->name, u->uid,
                         (unsigned)u->role, (unsigned)u->fail_count, (unsigned)u->next_allowed_time);
        if (n < 0 || pos + (size_t)n > bufsize) {
            return -1;
        }
        memcpy(buf + pos, line, (size_t)n);
        pos += (size_t)n;
    }
    return (int)pos;
}

/* Splits one tab-separated line (already isolated by the caller) into up to `max` fields,
 * writing into `line` in place. Returns the field count, or -1 if there are more than max. */
static int split_fields(char *line, char **fields, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        fields[n++] = p;
        char *tab = strchr(p, '\t');
        if (tab == NULL) {
            return n;
        }
        *tab = '\0';
        p = tab + 1;
    }
    return strchr(p, '\t') ? -1 : n;   /* a field left over: more columns than expected */
}

void purr_user_list_parse(purr_user_list_t *l, const char *buf, size_t len)
{
    purr_user_list_init(l);
    size_t i = 0;
    while (i < len && l->count < PURR_USER_MAX) {
        size_t start = i;
        while (i < len && buf[i] != '\n') {
            i++;
        }
        size_t line_len = i - start;
        if (i < len) {
            i++;
        }
        if (line_len == 0) {
            continue;
        }
        char line[PURR_USER_NAME_LEN + 48];
        if (line_len >= sizeof(line)) {
            continue;
        }
        memcpy(line, buf + start, line_len);
        line[line_len] = '\0';

        char *fields[5];
        if (split_fields(line, fields, 5) != 5) {
            continue;
        }
        char *end;
        long uid = strtol(fields[1], &end, 10);
        if (*end != '\0' || uid < 1 || uid > 255) {
            continue;
        }
        long role = strtol(fields[2], &end, 10);
        if (*end != '\0' || (role != PURR_ROLE_USER_STANDARD && role != PURR_ROLE_USER_ADMIN)) {
            continue;
        }
        long fail_count = strtol(fields[3], &end, 10);
        if (*end != '\0' || fail_count < 0) {
            continue;
        }
        long next_at = strtol(fields[4], &end, 10);
        if (*end != '\0' || next_at < 0) {
            continue;
        }
        if (purr_user_list_add(l, fields[0], (uint8_t)uid, (purr_user_role_t)role) != PURR_USER_ADDED) {
            continue;
        }
        purr_user_t *u = &l->users[l->count - 1];
        u->fail_count = (uint32_t)fail_count;
        u->next_allowed_time = (uint32_t)next_at;
    }
}

/* ---------------------------------------------------------------- passwords */

void purr_shadow_list_init(purr_shadow_list_t *l)
{
    memset(l, 0, sizeof(*l));
}

const purr_shadow_entry_t *purr_shadow_find(const purr_shadow_list_t *l, const char *name)
{
    for (int i = 0; i < l->count; i++) {
        if (strcmp(l->entries[i].name, name) == 0) {
            return &l->entries[i];
        }
    }
    return NULL;
}

static purr_shadow_entry_t *shadow_find_mut(purr_shadow_list_t *l, const char *name)
{
    for (int i = 0; i < l->count; i++) {
        if (strcmp(l->entries[i].name, name) == 0) {
            return &l->entries[i];
        }
    }
    return NULL;
}

void purr_shadow_set(purr_shadow_list_t *l, const char *name, const char *password,
                     const uint8_t salt[PURR_SALT_LEN], uint32_t iterations)
{
    purr_shadow_entry_t *e = shadow_find_mut(l, name);
    if (e == NULL) {
        if (l->count >= PURR_USER_MAX) {
            return;                              /* silently refused: the account list is
                                                    * already capped at the same size */
        }
        e = &l->entries[l->count++];
        memset(e, 0, sizeof(*e));
        strncpy(e->name, name, sizeof(e->name) - 1);
    }
    memcpy(e->salt, salt, PURR_SALT_LEN);
    e->iterations = iterations;
    purr_pbkdf2_hmac_sha256((const uint8_t *)password, strlen(password), salt, PURR_SALT_LEN,
                            iterations, e->hash);
}

int purr_shadow_list_remove(purr_shadow_list_t *l, const char *name)
{
    for (int i = 0; i < l->count; i++) {
        if (strcmp(l->entries[i].name, name) == 0) {
            for (int j = i; j < l->count - 1; j++) {
                l->entries[j] = l->entries[j + 1];
            }
            l->count--;
            return 1;
        }
    }
    return 0;
}

int purr_shadow_check(const purr_shadow_list_t *l, const char *name, const char *password)
{
    const purr_shadow_entry_t *e = purr_shadow_find(l, name);
    if (e == NULL) {
        return 0;
    }
    uint8_t computed[PURR_HASH_LEN];
    purr_pbkdf2_hmac_sha256((const uint8_t *)password, strlen(password), e->salt, PURR_SALT_LEN,
                            e->iterations, computed);
    uint8_t diff = 0;
    for (int i = 0; i < PURR_HASH_LEN; i++) {
        diff = (uint8_t)(diff | (computed[i] ^ e->hash[i]));   /* constant-time compare */
    }
    return diff == 0;
}

int purr_shadow_list_format(const purr_shadow_list_t *l, char *buf, size_t bufsize)
{
    size_t pos = 0;
    for (int i = 0; i < l->count; i++) {
        const purr_shadow_entry_t *e = &l->entries[i];
        char salt_hex[PURR_SALT_LEN * 2 + 1], hash_hex[PURR_HASH_LEN * 2 + 1];
        hex_encode(e->salt, PURR_SALT_LEN, salt_hex);
        salt_hex[PURR_SALT_LEN * 2] = '\0';
        hex_encode(e->hash, PURR_HASH_LEN, hash_hex);
        hash_hex[PURR_HASH_LEN * 2] = '\0';

        char line[PURR_USER_NAME_LEN + 128];
        int n = snprintf(line, sizeof(line), "%s\t%s\t%u\t%s\n", e->name, salt_hex,
                         (unsigned)e->iterations, hash_hex);
        if (n < 0 || pos + (size_t)n > bufsize) {
            return -1;
        }
        memcpy(buf + pos, line, (size_t)n);
        pos += (size_t)n;
    }
    return (int)pos;
}

void purr_shadow_list_parse(purr_shadow_list_t *l, const char *buf, size_t len)
{
    purr_shadow_list_init(l);
    size_t i = 0;
    while (i < len && l->count < PURR_USER_MAX) {
        size_t start = i;
        while (i < len && buf[i] != '\n') {
            i++;
        }
        size_t line_len = i - start;
        if (i < len) {
            i++;
        }
        if (line_len == 0) {
            continue;
        }
        char line[PURR_USER_NAME_LEN + 128];
        if (line_len >= sizeof(line)) {
            continue;
        }
        memcpy(line, buf + start, line_len);
        line[line_len] = '\0';

        char *fields[4];
        if (split_fields(line, fields, 4) != 4) {
            continue;
        }
        if (has_bad_char(fields[0])) {
            continue;
        }
        purr_shadow_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        strncpy(entry.name, fields[0], sizeof(entry.name) - 1);
        if (!hex_decode(fields[1], strlen(fields[1]), entry.salt, PURR_SALT_LEN)) {
            continue;
        }
        char *end;
        long iterations = strtol(fields[2], &end, 10);
        if (*end != '\0' || iterations <= 0) {
            continue;
        }
        if (!hex_decode(fields[3], strlen(fields[3]), entry.hash, PURR_HASH_LEN)) {
            continue;
        }
        entry.iterations = (uint32_t)iterations;
        l->entries[l->count++] = entry;
    }
}

uint32_t purr_login_delay_seconds(uint32_t fail_count)
{
    if (fail_count == 0) {
        return 0;
    }
    uint32_t delay = 1u << (fail_count - 1 > 6 ? 6 : fail_count - 1);   /* 1,2,4,...,64 before the cap */
    return delay > 60 ? 60 : delay;
}
