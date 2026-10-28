/* purr_hwprofile.c - see purr_hwprofile.h. */
#include "purr_hwprofile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy(char *dst, size_t cap, const char *src, size_t len)
{
    size_t n = len < cap - 1 ? len : cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int name_char_ok(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static int word_ok(const char *s)
{
    if (s[0] == '\0') {
        return 0;
    }
    for (; *s; s++) {
        if (!name_char_ok(*s)) {
            return 0;
        }
    }
    return 1;
}

int purr_hwp_dev_valid(const purr_hwp_dev_t *d, purr_pinmask_t reserved)
{
    if (!word_ok(d->name) || !word_ok(d->compatible)) {
        return 0;
    }
    if (d->addr < 0x08 || d->addr > 0x77) {
        return 0;
    }
    if (d->sda < 0 || d->scl < 0 || d->sda > 48 || d->scl > 48 || d->sda == d->scl) {
        return 0;
    }
    return !(reserved & (PURR_PINBIT(d->sda) | PURR_PINBIT(d->scl)));
}

static int same_device(const purr_hwp_dev_t *a, const purr_hwp_dev_t *b)
{
    return strcmp(a->compatible, b->compatible) == 0 && a->addr == b->addr && a->sda == b->sda && a->scl == b->scl;
}

int purr_hwp_add(purr_hwp_t *p, const purr_hwp_dev_t *d, purr_pinmask_t reserved)
{
    purr_hwp_dev_t n = *d;
    if (n.name[0] == '\0') {
        int k = 0;
        for (;; k++) {
            snprintf(n.name, sizeof(n.name), "%.14s_%d", n.compatible, k % 1000);
            int taken = 0;
            for (int i = 0; i < p->count; i++) {
                taken |= strcmp(p->dev[i].name, n.name) == 0;
            }
            if (!taken) {
                break;
            }
        }
    }
    if (!purr_hwp_dev_valid(&n, reserved)) {
        return -1;
    }
    for (int i = 0; i < p->count; i++) {
        if (same_device(&p->dev[i], &n)) {
            return 1;
        }
        if (strcmp(p->dev[i].name, n.name) == 0) {
            return -1;
        }
    }
    if (p->count >= PURR_HWP_MAX) {
        return -1;
    }
    p->dev[p->count++] = n;
    return 0;
}

typedef struct {
    purr_hwp_dev_t d;
    int have_bus_i2c, have_sda, have_scl, have_addr, any;
} draft_t;

static void commit(purr_hwp_t *p, draft_t *dr, purr_pinmask_t reserved)
{
    if (dr->any) {
        if (dr->have_bus_i2c && dr->have_sda && dr->have_scl && dr->have_addr && purr_hwp_add(p, &dr->d, reserved) >= 0) {
            /* kept (or an exact duplicate, silently merged) */
        } else {
            p->dropped++;
        }
    }
    memset(dr, 0, sizeof(*dr));
}

static int parse_num(const char *v, size_t len, long *out)
{
    char tmp[12];
    copy(tmp, sizeof(tmp), v, len);
    char *end = NULL;
    long n = strtol(tmp, &end, 0);
    if (end == tmp || *end != '\0') {
        return 0;
    }
    *out = n;
    return 1;
}

int purr_hwp_parse(purr_hwp_t *p, const char *text, size_t len, purr_pinmask_t reserved)
{
    memset(p, 0, sizeof(*p));
    draft_t dr;
    memset(&dr, 0, sizeof(dr));
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && text[j] != '\n') {
            j++;
        }
        const char *line = text + i;
        size_t ll = j - i;
        i = j + 1;
        while (ll > 0 && (line[ll - 1] == '\r' || line[ll - 1] == ' ' || line[ll - 1] == '\t')) {
            ll--;
        }
        if (ll == 0) {
            commit(p, &dr, reserved);
            continue;
        }
        if (line[0] == '#') {
            continue;
        }
        const char *eq = memchr(line, '=', ll);
        if (eq == NULL) {
            continue;
        }
        size_t kl = (size_t)(eq - line), vl = ll - kl - 1;
        const char *v = eq + 1;
        long n;
        if (kl == 6 && strncmp(line, "device", 6) == 0) {
            if (dr.any) {
                commit(p, &dr, reserved);               /* a new device without a blank line before it */
            }
            copy(dr.d.name, sizeof(dr.d.name), v, vl);
            dr.any = 1;
        } else if (!dr.any) {
            continue;
        } else if (kl == 10 && strncmp(line, "compatible", 10) == 0) {
            copy(dr.d.compatible, sizeof(dr.d.compatible), v, vl);
        } else if (kl == 5 && strncmp(line, "label", 5) == 0) {
            copy(dr.d.label, sizeof(dr.d.label), v, vl);
        } else if (kl == 3 && strncmp(line, "bus", 3) == 0) {
            dr.have_bus_i2c = vl == 3 && strncmp(v, "i2c", 3) == 0;
        } else if (kl == 3 && strncmp(line, "sda", 3) == 0 && parse_num(v, vl, &n)) {
            dr.d.sda = (int)n;
            dr.have_sda = 1;
        } else if (kl == 3 && strncmp(line, "scl", 3) == 0 && parse_num(v, vl, &n)) {
            dr.d.scl = (int)n;
            dr.have_scl = 1;
        } else if (kl == 4 && strncmp(line, "addr", 4) == 0 && parse_num(v, vl, &n) && n >= 0 && n < 256) {
            dr.d.addr = (uint8_t)n;
            dr.have_addr = 1;
        }
    }
    commit(p, &dr, reserved);
    return p->count;
}

int purr_hwp_format(const purr_hwp_t *p, char *out, size_t cap)
{
    size_t n = 0;
    int w = snprintf(out, cap, "# Devices found by \"Find hardware\" and confirmed by the owner. Edit by hand if you must.\n\n");
    if (w < 0 || (size_t)w >= cap) {
        return -1;
    }
    n = (size_t)w;
    for (int i = 0; i < p->count; i++) {
        const purr_hwp_dev_t *d = &p->dev[i];
        w = snprintf(out + n, cap - n, "device=%s\ncompatible=%s\nlabel=%s\nbus=i2c\nsda=%d\nscl=%d\naddr=0x%02X\n\n", d->name,
                     d->compatible, d->label, d->sda, d->scl, d->addr);
        if (w < 0 || (size_t)w >= cap - n) {
            return -1;
        }
        n += (size_t)w;
    }
    return (int)n;
}
