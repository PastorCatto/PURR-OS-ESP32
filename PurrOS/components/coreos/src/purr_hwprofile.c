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

static const char *const k_pin_keys[] = {"tx", "rx", "cs", "rst", "busy", "dio1", "dio2", "pwr", "pps", "int", "en", "irq", "mosi", "miso", "sclk",
                                         "bck", "ws", "din", "dout", "mclk", "sda", "scl"};

int purr_hwp_key_is_pin(const char *key)
{
    for (size_t i = 0; i < sizeof(k_pin_keys) / sizeof(k_pin_keys[0]); i++) {
        if (strcmp(key, k_pin_keys[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int key_ok(const char *k)
{
    size_t n = strlen(k);
    if (n == 0 || n > 7) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        char c = k[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return 0;
        }
    }
    return 1;
}

int32_t purr_hwp_kv_get(const purr_hwp_dev_t *d, const char *key, int32_t fallback)
{
    for (int i = 0; i < d->nkv && i < PURR_HWP_KV; i++) {
        if (strcmp(d->kv[i].key, key) == 0) {
            return d->kv[i].val;
        }
    }
    return fallback;
}

int purr_hwp_kv_set(purr_hwp_dev_t *d, const char *key, int32_t val)
{
    if (!key_ok(key)) {
        return -1;
    }
    for (int i = 0; i < d->nkv; i++) {
        if (strcmp(d->kv[i].key, key) == 0) {
            d->kv[i].val = val;
            return 0;
        }
    }
    if (d->nkv >= PURR_HWP_KV) {
        return -1;
    }
    memset(d->kv[d->nkv].key, 0, sizeof(d->kv[0].key));
    strcpy(d->kv[d->nkv].key, key);
    d->kv[d->nkv].val = val;
    d->nkv++;
    return 0;
}

static int is_i2c(const purr_hwp_dev_t *d)
{
    return d->bus[0] == '\0' || strcmp(d->bus, "i2c") == 0;
}

int purr_hwp_dev_valid(const purr_hwp_dev_t *d, purr_pinmask_t reserved)
{
    if (!word_ok(d->name) || !word_ok(d->compatible)) {
        return 0;
    }
    if (!is_i2c(d)) {                                      /* a device configured by named values */
        if (!word_ok(d->bus) || d->nkv == 0 || d->nkv > PURR_HWP_KV) {
            return 0;
        }
        const purr_pinmask_t hard = purr_hw_reserved_hard_s3();
        (void)reserved;                                    /* the caller's mask is for scans; a named pin only has to be one the chip can drive */
        int pins = 0;
        for (int i = 0; i < d->nkv; i++) {
            if (!key_ok(d->kv[i].key)) {
                return 0;
            }
            if (purr_hwp_key_is_pin(d->kv[i].key)) {
                int32_t v = d->kv[i].val;
                if (v == -1) {
                    continue;                              /* not connected */
                }
                if (v < 0 || v > 48 || (hard & PURR_PINBIT(v))) {
                    return 0;
                }
                if (++pins > 6) {
                    return 0;                              /* only six pins can be claimed per device */
                }
            }
        }
        return 1;
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
    if (strcmp(a->compatible, b->compatible) != 0) {
        return 0;
    }
    if (!is_i2c(a) || !is_i2c(b)) {
        if (is_i2c(a) != is_i2c(b) || strcmp(a->bus, b->bus) != 0 || a->nkv != b->nkv) {
            return 0;
        }
        for (int i = 0; i < a->nkv; i++) {
            if (purr_hwp_kv_get(b, a->kv[i].key, a->kv[i].val + 1) != a->kv[i].val) {
                return 0;
            }
        }
        return 1;
    }
    return a->addr == b->addr && a->sda == b->sda && a->scl == b->scl;
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
    int have_bus_i2c, have_bus_other, have_sda, have_scl, have_addr, any, bad_kv;
} draft_t;

static void commit(purr_hwp_t *p, draft_t *dr, purr_pinmask_t reserved)
{
    if (dr->any) {
        if (dr->have_bus_other) {                                   /* sda, scl and addr are ordinary named values for a device on another bus */
            if (dr->have_sda) purr_hwp_kv_set(&dr->d, "sda", dr->d.sda);
            if (dr->have_scl) purr_hwp_kv_set(&dr->d, "scl", dr->d.scl);
            if (dr->have_addr) purr_hwp_kv_set(&dr->d, "addr", dr->d.addr);
        }
        int ok = (dr->have_bus_i2c && dr->have_sda && dr->have_scl && dr->have_addr) || (dr->have_bus_other && !dr->bad_kv);
        if (ok && purr_hwp_add(p, &dr->d, reserved) >= 0) {
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
            dr.have_bus_other = !dr.have_bus_i2c && vl > 0;
            copy(dr.d.bus, sizeof(dr.d.bus), v, vl);
        } else if (kl == 3 && strncmp(line, "sda", 3) == 0 && parse_num(v, vl, &n)) {
            dr.d.sda = (int)n;
            dr.have_sda = 1;
        } else if (kl == 3 && strncmp(line, "scl", 3) == 0 && parse_num(v, vl, &n)) {
            dr.d.scl = (int)n;
            dr.have_scl = 1;
        } else if (kl == 4 && strncmp(line, "addr", 4) == 0 && parse_num(v, vl, &n) && n >= 0 && n < 256) {
            dr.d.addr = (uint8_t)n;
            dr.have_addr = 1;
        } else if (kl >= 1 && kl <= 7 && parse_num(v, vl, &n)) {
            char key[8];
            copy(key, sizeof(key), line, kl);
            if (purr_hwp_kv_set(&dr.d, key, (int32_t)n) != 0) {
                dr.bad_kv = 1;                          /* a bad key, or more than twelve of them */
            }
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
        if (is_i2c(d)) {
            w = snprintf(out + n, cap - n, "device=%s\ncompatible=%s\nlabel=%s\nbus=i2c\nsda=%d\nscl=%d\naddr=0x%02X\n\n", d->name,
                         d->compatible, d->label, d->sda, d->scl, d->addr);
        } else {
            w = snprintf(out + n, cap - n, "device=%s\ncompatible=%s\nlabel=%s\nbus=%s\n", d->name, d->compatible, d->label, d->bus);
            if (w < 0 || (size_t)w >= cap - n) {
                return -1;
            }
            n += (size_t)w;
            for (int k = 0; k < d->nkv; k++) {
                w = snprintf(out + n, cap - n, "%s=%d\n", d->kv[k].key, (int)d->kv[k].val);
                if (w < 0 || (size_t)w >= cap - n) {
                    return -1;
                }
                n += (size_t)w;
            }
            w = snprintf(out + n, cap - n, "\n");
        }
        if (w < 0 || (size_t)w >= cap - n) {
            return -1;
        }
        n += (size_t)w;
    }
    return (int)n;
}
