/* purr_perm.c - see purr_perm.h. */
#include "purr_perm.h"

#include <stdio.h>
#include <string.h>

static const struct {
    const char *name, *what;
} k_perm[PURR_PERM_COUNT] = {
    {"keep-awake", "keep the device awake"},
    {"wake-alarm", "wake the device on a schedule (uses battery)"},
    {"shared-storage", "see and change the files in your home folder"},
    {"system-files", "read the system's files (read-only)"},
    {"manage-devices", "find and set up hardware, and update drivers and the system"},
    {"install-apps", "install, update and remove apps and downloads"},
    {"messaging", "read and send your messages"},
    {"contacts", "read and change your contacts"},
    {"radio", "use the LoRa radio"},
};

const char *purr_perm_name(int perm)
{
    return perm >= 0 && perm < PURR_PERM_COUNT ? k_perm[perm].name : NULL;
}

const char *purr_perm_describe(int perm)
{
    return perm >= 0 && perm < PURR_PERM_COUNT ? k_perm[perm].what : NULL;
}

int purr_perm_by_name(const char *name)
{
    for (int i = 0; name != NULL && i < PURR_PERM_COUNT; i++) {
        if (strcmp(name, k_perm[i].name) == 0) {
            return i;
        }
    }
    return -1;
}

static int valid_app(const char *app)
{
    size_t n = app ? strlen(app) : 0;
    if (n == 0 || n >= PURR_PERM_APP_LEN) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (app[i] == ':' || app[i] == '=' || app[i] == '\n' || app[i] == '\r' || app[i] == ' ') {
            return 0;
        }
    }
    return 1;
}

static purr_perm_entry_t *find(const purr_perm_t *t, const char *app)
{
    for (int i = 0; app != NULL && i < t->count; i++) {
        if (strcmp(t->e[i].app, app) == 0) {
            return (purr_perm_entry_t *)&t->e[i];
        }
    }
    return NULL;
}

static purr_perm_entry_t *find_or_add(purr_perm_t *t, const char *app)
{
    purr_perm_entry_t *e = find(t, app);
    if (e != NULL || !valid_app(app) || t->count >= PURR_PERM_MAX_APPS) {
        return e;
    }
    e = &t->e[t->count++];
    memset(e, 0, sizeof(*e));
    snprintf(e->app, sizeof(e->app), "%s", app);
    return e;
}

int purr_perm_state(const purr_perm_t *t, const char *app, int perm)
{
    const purr_perm_entry_t *e = find(t, app);
    return e != NULL && perm >= 0 && perm < PURR_PERM_COUNT ? e->state[perm] : PURR_PERM_ASK;
}

int purr_perm_decide(const purr_perm_t *t, const char *app, int perm, int privileged)
{
    if (perm < 0 || perm >= PURR_PERM_COUNT) {
        return PURR_PERM_NO;                        /* an unknown permission fails closed */
    }
    if (privileged) {
        return PURR_PERM_YES;
    }
    switch (purr_perm_state(t, app, perm)) {
    case PURR_PERM_ALWAYS:
    case PURR_PERM_ONCE:  return PURR_PERM_YES;
    case PURR_PERM_NEVER: return PURR_PERM_NO;
    default:              return PURR_PERM_PROMPT;
    }
}

int purr_perm_set(purr_perm_t *t, const char *app, int perm, int state)
{
    if (perm < 0 || perm >= PURR_PERM_COUNT || state < PURR_PERM_ASK || state > PURR_PERM_NEVER) {
        return -1;
    }
    purr_perm_entry_t *e = find_or_add(t, app);
    if (e == NULL) {
        return -1;
    }
    e->state[perm] = (uint8_t)state;
    if (state == PURR_PERM_ALWAYS || state == PURR_PERM_ONCE) {
        e->denials[perm] = 0;
    }
    return 0;
}

int purr_perm_deny(purr_perm_t *t, const char *app, int perm)
{
    purr_perm_entry_t *e = perm >= 0 && perm < PURR_PERM_COUNT ? find_or_add(t, app) : NULL;
    if (e == NULL) {
        return 0;
    }
    if (e->denials[perm] < 250) {
        e->denials[perm]++;
    }
    return e->denials[perm];
}

int purr_perm_denials(const purr_perm_t *t, const char *app, int perm)
{
    const purr_perm_entry_t *e = find(t, app);
    return e != NULL && perm >= 0 && perm < PURR_PERM_COUNT ? e->denials[perm] : 0;
}

void purr_perm_app_stopped(purr_perm_t *t, const char *app)
{
    purr_perm_entry_t *e = find(t, app);
    for (int i = 0; e != NULL && i < PURR_PERM_COUNT; i++) {
        if (e->state[i] == PURR_PERM_ONCE) {
            e->state[i] = PURR_PERM_ASK;
        }
        e->denials[i] = 0;
    }
}

void purr_perm_forget(purr_perm_t *t, const char *app)
{
    purr_perm_entry_t *e = find(t, app);
    if (e != NULL) {
        *e = t->e[--t->count];
        memset(&t->e[t->count], 0, sizeof(t->e[0]));
    }
}

void purr_perm_reset(purr_perm_t *t, const char *app)
{
    purr_perm_entry_t *e = find(t, app);
    if (e != NULL) {
        memset(e->state, 0, sizeof(e->state));
        memset(e->denials, 0, sizeof(e->denials));
    }
}

size_t purr_perm_format(const purr_perm_t *t, char *out, size_t cap)
{
    size_t n = 0;
    if (cap == 0) {
        return 0;
    }
    out[0] = '\0';
    for (int i = 0; i < t->count; i++) {
        for (int p = 0; p < PURR_PERM_COUNT; p++) {
            int s = t->e[i].state[p];
            if (s != PURR_PERM_ALWAYS && s != PURR_PERM_NEVER) {
                continue;                           /* "ask" and "just once" are not kept */
            }
            int w = snprintf(out + n, cap - n, "%s:%s=%s\n", t->e[i].app, k_perm[p].name,
                             s == PURR_PERM_ALWAYS ? "always" : "never");
            if (w < 0 || (size_t)w >= cap - n) {
                out[n] = '\0';
                return n;                           /* whole lines only */
            }
            n += (size_t)w;
        }
    }
    return n;
}

void purr_perm_parse(purr_perm_t *t, const char *text)
{
    while (text != NULL && *text) {
        const char *nl = strchr(text, '\n');
        size_t len = nl ? (size_t)(nl - text) : strlen(text);
        char line[96];
        if (len > 0 && len < sizeof(line)) {
            memcpy(line, text, len);
            line[len] = '\0';
            if (len > 0 && line[len - 1] == '\r') {
                line[len - 1] = '\0';
            }
            char *colon = strchr(line, ':');
            char *eq = colon ? strchr(colon, '=') : NULL;
            if (colon != NULL && eq != NULL && line[0] != '#') {
                *colon = '\0';
                *eq = '\0';
                int perm = purr_perm_by_name(colon + 1);
                int state = strcmp(eq + 1, "always") == 0 ? PURR_PERM_ALWAYS
                          : strcmp(eq + 1, "never") == 0 ? PURR_PERM_NEVER : -1;
                if (perm >= 0 && state >= 0) {
                    purr_perm_set(t, line, perm, state);
                }
            }
        }
        text = nl ? nl + 1 : text + len;
    }
}
