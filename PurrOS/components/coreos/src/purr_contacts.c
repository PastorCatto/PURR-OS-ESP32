/* purr_contacts.c - see purr_contacts.h. */
#include "purr_contacts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static void clean(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    while (src && src[i] && i + 1 < cap) {
        char c = src[i];
        dst[i] = (c == '|' || c == ';' || c == '=' || c == '\n' || c == '\r') ? ' ' : c;
        i++;
    }
    dst[i] = '\0';
    while (i > 0 && dst[i - 1] == ' ') {                /* no trailing spaces */
        dst[--i] = '\0';
    }
}

static int key_ok(const char *k)
{
    size_t n = k ? strlen(k) : 0;
    if (n == 0 || n >= PURR_CT_SVC) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (!((k[i] >= 'a' && k[i] <= 'z') || (k[i] >= '0' && k[i] <= '9'))) {
            return 0;
        }
    }
    return 1;
}

void purr_ct_init(purr_contacts_t *b)
{
    memset(b, 0, sizeof(*b));
    b->next_id = 1;
}

static purr_contact_t *find(purr_contacts_t *b, uint32_t id)
{
    for (int i = 0; id != 0 && i < b->n; i++) {
        if (b->c[i].id == id) {
            return &b->c[i];
        }
    }
    return NULL;
}

const purr_contact_t *purr_ct_get(const purr_contacts_t *b, uint32_t id) { return find((purr_contacts_t *)b, id); }

int32_t purr_ct_add(purr_contacts_t *b, const char *name)
{
    char n[PURR_CT_NAME];
    clean(n, sizeof(n), name);
    if (b->n >= PURR_CT_MAX || n[0] == '\0') {
        return -1;
    }
    purr_contact_t *c = &b->c[b->n++];
    memset(c, 0, sizeof(*c));
    c->id = b->next_id++;
    memcpy(c->name, n, sizeof(c->name));
    return (int32_t)c->id;
}

int purr_ct_rename(purr_contacts_t *b, uint32_t id, const char *name)
{
    purr_contact_t *c = find(b, id);
    char n[PURR_CT_NAME];
    clean(n, sizeof(n), name);
    if (c == NULL || n[0] == '\0') {
        return -1;
    }
    memcpy(c->name, n, sizeof(c->name));
    return 0;
}

int purr_ct_set_fav(purr_contacts_t *b, uint32_t id, int fav)
{
    purr_contact_t *c = find(b, id);
    if (c == NULL) {
        return -1;
    }
    c->fav = fav ? 1 : 0;
    return 0;
}

int purr_ct_set_addr(purr_contacts_t *b, uint32_t id, const char *service, const char *addr)
{
    purr_contact_t *c = find(b, id);
    char a[PURR_CT_ADDR];
    clean(a, sizeof(a), addr);
    if (c == NULL || !key_ok(service) || a[0] == '\0') {
        return -1;
    }
    for (int i = 0; i < c->naddr; i++) {
        if (strcmp(c->a[i].service, service) == 0) {
            memcpy(c->a[i].addr, a, sizeof(a));
            return 0;
        }
    }
    if (c->naddr >= PURR_CT_ADDRS) {
        return -1;
    }
    strncpy(c->a[c->naddr].service, service, PURR_CT_SVC - 1);
    memcpy(c->a[c->naddr].addr, a, sizeof(a));
    c->naddr++;
    return 0;
}

int purr_ct_remove_addr(purr_contacts_t *b, uint32_t id, const char *service)
{
    purr_contact_t *c = find(b, id);
    for (int i = 0; c && service && i < c->naddr; i++) {
        if (strcmp(c->a[i].service, service) == 0) {
            c->a[i] = c->a[c->naddr - 1];
            memset(&c->a[c->naddr - 1], 0, sizeof(c->a[0]));
            c->naddr--;
            return 0;
        }
    }
    return -1;
}

int purr_ct_remove(purr_contacts_t *b, uint32_t id)
{
    for (int i = 0; i < b->n; i++) {
        if (b->c[i].id == id) {
            b->c[i] = b->c[b->n - 1];
            memset(&b->c[b->n - 1], 0, sizeof(b->c[0]));
            b->n--;
            return 0;
        }
    }
    return -1;
}

uint32_t purr_ct_find_addr(const purr_contacts_t *b, const char *service, const char *addr)
{
    for (int i = 0; service && addr && i < b->n; i++) {
        for (int k = 0; k < b->c[i].naddr; k++) {
            if (strcmp(b->c[i].a[k].service, service) == 0 && strcmp(b->c[i].a[k].addr, addr) == 0) {
                return b->c[i].id;
            }
        }
    }
    return 0;
}

static int name_cmp(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return (unsigned char)lower(*a) - (unsigned char)lower(*b);
}

static int contains(const char *hay, const char *needle)
{
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*n && *h && lower(*h) == lower(*n)) {
            h++;
            n++;
        }
        if (*n == '\0') {
            return 1;
        }
    }
    return 0;
}

int purr_ct_order(const purr_contacts_t *b, uint32_t *ids, int max, const char *filter)
{
    int idx[PURR_CT_MAX];
    int n = 0;
    for (int i = 0; i < b->n; i++) {
        if (filter == NULL || filter[0] == '\0' || contains(b->c[i].name, filter)) {
            idx[n++] = i;
        }
    }
    for (int i = 1; i < n; i++) {                       /* insertion sort: at most 64 */
        int v = idx[i], k = i;
        while (k > 0 && name_cmp(b->c[v].name, b->c[idx[k - 1]].name) < 0) {
            idx[k] = idx[k - 1];
            k--;
        }
        idx[k] = v;
    }
    int out = 0;
    for (int i = 0; i < n && out < max; i++) {
        ids[out++] = b->c[idx[i]].id;
    }
    return out;
}

size_t purr_ct_format(const purr_contacts_t *b, char *out, size_t cap)
{
    size_t n = 0;
    for (int i = 0; i < b->n && n + 1 < cap; i++) {
        const purr_contact_t *c = &b->c[i];
        int w = snprintf(out + n, cap - n, "C|%u|%d|%s|", (unsigned)c->id, c->fav, c->name);
        if (w < 0 || (size_t)w >= cap - n) {
            break;
        }
        n += (size_t)w;
        for (int k = 0; k < c->naddr; k++) {
            w = snprintf(out + n, cap - n, "%s%s=%s", k ? ";" : "", c->a[k].service, c->a[k].addr);
            if (w < 0 || (size_t)w >= cap - n) {
                break;
            }
            n += (size_t)w;
        }
        if (n + 2 >= cap) {
            break;
        }
        out[n++] = '\n';
        out[n] = '\0';
    }
    if (n < cap) {
        out[n] = '\0';
    }
    return n;
}

void purr_ct_parse(purr_contacts_t *b, const char *text)
{
    purr_ct_init(b);
    while (text && *text) {
        const char *eol = strchr(text, '\n');
        size_t len = eol ? (size_t)(eol - text) : strlen(text);
        char line[200];
        if (len > 2 && len < sizeof(line) && text[0] == 'C' && text[1] == '|') {
            memcpy(line, text, len);
            line[len] = '\0';
            char *f[5] = {0};
            int nf = 0;
            char *p = line;
            while (nf < 5) {
                f[nf++] = p;
                char *bar = nf < 5 ? strchr(p, '|') : NULL;
                if (bar == NULL) {
                    break;
                }
                *bar = '\0';
                p = bar + 1;
            }
            if (nf >= 4 && b->n < PURR_CT_MAX) {
                uint32_t id = (uint32_t)strtoul(f[1], NULL, 10);
                purr_contact_t *c = &b->c[b->n];
                memset(c, 0, sizeof(*c));
                char n[PURR_CT_NAME];
                clean(n, sizeof(n), f[3]);
                if (id != 0 && n[0] != '\0') {
                    c->id = id;
                    c->fav = f[2][0] == '1';
                    memcpy(c->name, n, sizeof(n));
                    b->n++;
                    if (id >= b->next_id) {
                        b->next_id = id + 1;
                    }
                    char *addrs = nf >= 5 ? f[4] : (char *)"";
                    while (*addrs) {
                        char *semi = strchr(addrs, ';');
                        if (semi) {
                            *semi = '\0';
                        }
                        char *eq = strchr(addrs, '=');
                        if (eq) {
                            *eq = '\0';
                            purr_ct_set_addr(b, id, addrs, eq + 1);
                        }
                        addrs = semi ? semi + 1 : addrs + strlen(addrs);
                    }
                }
            }
        }
        text = eol ? eol + 1 : text + len;
    }
}
