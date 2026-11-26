/* purr_mt.c - see purr_mt.h. */
#include "purr_mt.h"

#include <string.h>

/* ---------------------------------------------------------------- AES */

static uint8_t s_sbox[256];
static int s_sbox_ready;

static uint8_t rotl8(uint8_t x, int n) { return (uint8_t)((x << n) | (x >> (8 - n))); }

/* the S-box from its definition (the multiplicative inverse in GF(2^8), then an affine map): no table to mistype */
static void sbox_init(void)
{
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));         /* p times 3 */
        q ^= (uint8_t)(q << 1);                                         /* q divided by 3 */
        q ^= (uint8_t)(q << 2);
        q ^= (uint8_t)(q << 4);
        if (q & 0x80) {
            q ^= 0x09;
        }
        uint8_t x = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4));
        s_sbox[p] = (uint8_t)(x ^ 0x63);
    } while (p != 1);
    s_sbox[0] = 0x63;
    s_sbox_ready = 1;
}

int purr_mt_aes_init(purr_mt_aes_t *a, const uint8_t *key, int keylen)
{
    if (keylen != 16 && keylen != 32) {
        return -1;
    }
    if (!s_sbox_ready) {
        sbox_init();
    }
    int nk = keylen / 4;
    a->rounds = nk + 6;
    uint8_t *w = (uint8_t *)a->rk;
    memcpy(w, key, (size_t)keylen);
    uint8_t rcon = 1;
    for (int i = nk; i < 4 * (a->rounds + 1); i++) {
        uint8_t t[4];
        memcpy(t, w + 4 * (i - 1), 4);
        if (i % nk == 0) {
            uint8_t t0 = t[0];
            t[0] = (uint8_t)(s_sbox[t[1]] ^ rcon);
            t[1] = s_sbox[t[2]];
            t[2] = s_sbox[t[3]];
            t[3] = s_sbox[t0];
            rcon = (uint8_t)((rcon << 1) ^ ((rcon & 0x80) ? 0x1B : 0));
        } else if (nk > 6 && i % nk == 4) {
            for (int k = 0; k < 4; k++) {
                t[k] = s_sbox[t[k]];
            }
        }
        for (int k = 0; k < 4; k++) {
            w[4 * i + k] = (uint8_t)(w[4 * (i - nk) + k] ^ t[k]);
        }
    }
    return 0;
}

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

void purr_mt_aes_block(const purr_mt_aes_t *a, const uint8_t in[16], uint8_t out[16])
{
    const uint8_t *w = (const uint8_t *)a->rk;
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) {
        s[i] = (uint8_t)(in[i] ^ w[i]);
    }
    for (int round = 1; round <= a->rounds; round++) {
        for (int i = 0; i < 16; i++) {
            s[i] = s_sbox[s[i]];
        }
        for (int c = 0; c < 4; c++) {                                   /* ShiftRows: row r moves left by r */
            for (int r = 0; r < 4; r++) {
                t[r + 4 * c] = s[r + 4 * ((c + r) & 3)];
            }
        }
        if (round != a->rounds) {
            for (int c = 0; c < 4; c++) {                               /* MixColumns */
                uint8_t *col = t + 4 * c;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                uint8_t all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                col[0] = (uint8_t)(a0 ^ all ^ xtime((uint8_t)(a0 ^ a1)));
                col[1] = (uint8_t)(a1 ^ all ^ xtime((uint8_t)(a1 ^ a2)));
                col[2] = (uint8_t)(a2 ^ all ^ xtime((uint8_t)(a2 ^ a3)));
                col[3] = (uint8_t)(a3 ^ all ^ xtime((uint8_t)(a3 ^ a0)));
            }
        }
        for (int i = 0; i < 16; i++) {
            s[i] = (uint8_t)(t[i] ^ w[16 * round + i]);
        }
    }
    memcpy(out, s, 16);
}

void purr_mt_ctr(const purr_mt_aes_t *a, const uint8_t iv[16], uint8_t *data, size_t len)
{
    uint8_t ctr[16], ks[16];
    memcpy(ctr, iv, 16);
    for (size_t off = 0; off < len; off += 16) {
        purr_mt_aes_block(a, ctr, ks);
        size_t n = len - off < 16 ? len - off : 16;
        for (size_t i = 0; i < n; i++) {
            data[off + i] = (uint8_t)(data[off + i] ^ ks[i]);
        }
        for (int i = 15; i >= 12; i--) {                                /* the counter is the last four bytes, big-endian */
            if (++ctr[i] != 0) {
                break;
            }
        }
    }
}

/* ---------------------------------------------------------------- the air format */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

int purr_mt_hdr_put(const purr_mt_hdr_t *h, uint8_t out[PURR_MT_HDR_LEN])
{
    put32(out, h->to);
    put32(out + 4, h->from);
    put32(out + 8, h->id);
    out[12] = (uint8_t)((h->hop_limit & 7) | ((h->want_ack & 1) << 3) | ((h->via_mqtt & 1) << 4) | ((h->hop_start & 7) << 5));
    out[13] = h->channel;
    out[14] = h->next_hop;
    out[15] = h->relay_node;
    return PURR_MT_HDR_LEN;
}

int purr_mt_hdr_get(const uint8_t *in, size_t n, purr_mt_hdr_t *h)
{
    if (n < PURR_MT_HDR_LEN) {
        return -1;
    }
    h->to = get32(in);
    h->from = get32(in + 4);
    h->id = get32(in + 8);
    h->hop_limit = in[12] & 7;
    h->want_ack = (in[12] >> 3) & 1;
    h->via_mqtt = (in[12] >> 4) & 1;
    h->hop_start = (in[12] >> 5) & 7;
    h->channel = in[13];
    h->next_hop = in[14];
    h->relay_node = in[15];
    return PURR_MT_HDR_LEN;
}

/* ---------------------------------------------------------------- channels and keys */

const uint8_t purr_mt_default_psk[PURR_MT_DEFAULT_PSK_LEN] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59, 0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};

int purr_mt_expand_psk(const uint8_t *psk, int psk_len, uint8_t out[32])
{
    if (psk_len < 0 || psk_len > 32) {
        return -1;
    }
    if (psk_len == 0) {
        return 0;
    }
    if (psk_len == 1) {
        if (psk[0] == 0) {
            return 0;                                                   /* index 0: no encryption */
        }
        memcpy(out, purr_mt_default_psk, 16);
        out[15] = (uint8_t)(out[15] + psk[0] - 1);
        return 16;
    }
    int target = psk_len <= 16 ? 16 : 32;
    memset(out, 0, (size_t)target);
    memcpy(out, psk, (size_t)psk_len);
    return target;
}

uint8_t purr_mt_channel_hash(const char *name, const uint8_t *key, int keylen)
{
    uint8_t h = 0;
    for (const char *c = name; *c != '\0'; c++) {
        h ^= (uint8_t)*c;
    }
    for (int i = 0; i < keylen; i++) {
        h ^= key[i];
    }
    return h;
}

void purr_mt_crypt(const uint8_t *key, int keylen, uint32_t from, uint32_t id, uint8_t *data, size_t len)
{
    purr_mt_aes_t a;
    if (keylen == 0 || len == 0 || purr_mt_aes_init(&a, key, keylen) != 0) {
        return;
    }
    uint8_t iv[16];
    memset(iv, 0, sizeof(iv));
    put32(iv, id);                                                      /* the packet id, as 64 bits little-endian */
    put32(iv + 8, from);
    purr_mt_ctr(&a, iv, data, len);
}

/* ---------------------------------------------------------------- protobuf */

void purr_mt_pb_w_init(purr_mt_pb_w_t *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->err = 0;
}

static void pb_byte(purr_mt_pb_w_t *w, uint8_t b)
{
    if (w->len < w->cap) {
        w->buf[w->len++] = b;
    } else {
        w->err = 1;
    }
}

static void pb_varint_raw(purr_mt_pb_w_t *w, uint64_t v)
{
    while (v >= 0x80) {
        pb_byte(w, (uint8_t)(v | 0x80));
        v >>= 7;
    }
    pb_byte(w, (uint8_t)v);
}

void purr_mt_pb_varint(purr_mt_pb_w_t *w, int field, uint64_t v)
{
    pb_varint_raw(w, ((uint64_t)field << 3) | 0);
    pb_varint_raw(w, v);
}

void purr_mt_pb_fixed32(purr_mt_pb_w_t *w, int field, uint32_t v)
{
    pb_varint_raw(w, ((uint64_t)field << 3) | 5);
    for (int i = 0; i < 4; i++) {
        pb_byte(w, (uint8_t)(v >> (8 * i)));
    }
}

void purr_mt_pb_bytes(purr_mt_pb_w_t *w, int field, const uint8_t *p, size_t n)
{
    pb_varint_raw(w, ((uint64_t)field << 3) | 2);
    pb_varint_raw(w, n);
    for (size_t i = 0; i < n; i++) {
        pb_byte(w, p[i]);
    }
}

void purr_mt_pb_r_init(purr_mt_pb_r_t *r, const uint8_t *p, size_t n)
{
    r->p = p;
    r->n = n;
    r->err = 0;
}

static int rd_varint(purr_mt_pb_r_t *r, uint64_t *v)
{
    uint64_t x = 0;
    for (int shift = 0; shift < 70; shift += 7) {
        if (r->n == 0) {
            return -1;
        }
        uint8_t b = *r->p++;
        r->n--;
        x |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *v = x;
            return 0;
        }
    }
    return -1;
}

int purr_mt_pb_next(purr_mt_pb_r_t *r, purr_mt_pb_field_t *f)
{
    if (r->n == 0) {
        return 0;
    }
    uint64_t tag;
    if (rd_varint(r, &tag) != 0 || (tag >> 3) == 0) {
        r->err = 1;
        return -1;
    }
    f->field = (int)(tag >> 3);
    f->wire = (int)(tag & 7);
    f->v = 0;
    f->data = NULL;
    f->len = 0;
    switch (f->wire) {
    case 0:
        if (rd_varint(r, &f->v) != 0) {
            r->err = 1;
            return -1;
        }
        return 1;
    case 5:
        if (r->n < 4) {
            r->err = 1;
            return -1;
        }
        f->v = get32(r->p);
        r->p += 4;
        r->n -= 4;
        return 1;
    case 1:
        if (r->n < 8) {
            r->err = 1;
            return -1;
        }
        f->v = (uint64_t)get32(r->p) | ((uint64_t)get32(r->p + 4) << 32);
        r->p += 8;
        r->n -= 8;
        return 1;
    case 2: {
        uint64_t len;
        if (rd_varint(r, &len) != 0 || len > r->n) {
            r->err = 1;
            return -1;
        }
        f->data = r->p;
        f->len = (size_t)len;
        r->p += len;
        r->n -= (size_t)len;
        return 1;
    }
    default:
        r->err = 1;
        return -1;
    }
}

/* ---------------------------------------------------------------- the messages */

int purr_mt_data_put(const purr_mt_data_t *d, uint8_t *out, size_t cap)
{
    purr_mt_pb_w_t w;
    purr_mt_pb_w_init(&w, out, cap);
    purr_mt_pb_varint(&w, 1, d->portnum);
    if (d->payload_len > 0) {
        purr_mt_pb_bytes(&w, 2, d->payload, d->payload_len);
    }
    if (d->want_response) {
        purr_mt_pb_varint(&w, 3, 1);
    }
    if (d->dest != 0) {
        purr_mt_pb_fixed32(&w, 4, d->dest);
    }
    if (d->source != 0) {
        purr_mt_pb_fixed32(&w, 5, d->source);
    }
    if (d->request_id != 0) {
        purr_mt_pb_fixed32(&w, 6, d->request_id);
    }
    if (d->reply_id != 0) {
        purr_mt_pb_fixed32(&w, 7, d->reply_id);
    }
    if (d->emoji != 0) {
        purr_mt_pb_fixed32(&w, 8, d->emoji);
    }
    if (d->has_bitfield) {
        purr_mt_pb_varint(&w, 9, d->bitfield);
    }
    return w.err ? -1 : (int)w.len;
}

int purr_mt_data_get(const uint8_t *in, size_t n, purr_mt_data_t *d)
{
    memset(d, 0, sizeof(*d));
    purr_mt_pb_r_t r;
    purr_mt_pb_field_t f;
    purr_mt_pb_r_init(&r, in, n);
    int rc;
    while ((rc = purr_mt_pb_next(&r, &f)) > 0) {
        switch (f.field) {
        case 1: d->portnum = (uint32_t)f.v; break;
        case 2: d->payload = f.data; d->payload_len = f.len; break;
        case 3: d->want_response = f.v != 0; break;
        case 4: d->dest = (uint32_t)f.v; break;
        case 5: d->source = (uint32_t)f.v; break;
        case 6: d->request_id = (uint32_t)f.v; break;
        case 7: d->reply_id = (uint32_t)f.v; break;
        case 8: d->emoji = (uint32_t)f.v; break;
        case 9: d->bitfield = (uint32_t)f.v; d->has_bitfield = 1; break;
        default: break;                                                 /* a field this version does not know is skipped */
        }
    }
    return rc < 0 ? -1 : 0;
}

static size_t slen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void copy_str(char *dst, size_t cap, const uint8_t *src, size_t n)
{
    size_t k = n < cap - 1 ? n : cap - 1;
    memcpy(dst, src, k);
    dst[k] = '\0';
}

int purr_mt_user_put(const purr_mt_user_t *u, uint8_t *out, size_t cap)
{
    purr_mt_pb_w_t w;
    purr_mt_pb_w_init(&w, out, cap);
    purr_mt_pb_bytes(&w, 1, (const uint8_t *)u->id, slen(u->id));
    purr_mt_pb_bytes(&w, 2, (const uint8_t *)u->long_name, slen(u->long_name));
    purr_mt_pb_bytes(&w, 3, (const uint8_t *)u->short_name, slen(u->short_name));
    if (u->hw_model != 0) {
        purr_mt_pb_varint(&w, 5, u->hw_model);
    }
    if (u->role != 0) {
        purr_mt_pb_varint(&w, 7, u->role);
    }
    if (u->has_key) {
        purr_mt_pb_bytes(&w, 8, u->public_key, 32);
    }
    return w.err ? -1 : (int)w.len;
}

int purr_mt_user_get(const uint8_t *in, size_t n, purr_mt_user_t *u)
{
    memset(u, 0, sizeof(*u));
    purr_mt_pb_r_t r;
    purr_mt_pb_field_t f;
    purr_mt_pb_r_init(&r, in, n);
    int rc;
    while ((rc = purr_mt_pb_next(&r, &f)) > 0) {
        switch (f.field) {
        case 1: copy_str(u->id, sizeof(u->id), f.data, f.len); break;
        case 2: copy_str(u->long_name, sizeof(u->long_name), f.data, f.len); break;
        case 3: copy_str(u->short_name, sizeof(u->short_name), f.data, f.len); break;
        case 5: u->hw_model = (uint32_t)f.v; break;
        case 7: u->role = (uint32_t)f.v; break;
        case 8:
            if (f.len == 32) {
                memcpy(u->public_key, f.data, 32);
                u->has_key = 1;
            }
            break;
        default: break;
        }
    }
    return rc < 0 ? -1 : 0;
}

int purr_mt_pos_put(const purr_mt_pos_t *p, uint8_t *out, size_t cap)
{
    purr_mt_pb_w_t w;
    purr_mt_pb_w_init(&w, out, cap);
    purr_mt_pb_fixed32(&w, 1, (uint32_t)p->lat_e7);
    purr_mt_pb_fixed32(&w, 2, (uint32_t)p->lon_e7);
    if (p->has_alt) {
        purr_mt_pb_varint(&w, 3, (uint64_t)(int64_t)p->alt_m);          /* a negative int32 is sign-extended to 64 bits */
    }
    if (p->time != 0) {
        purr_mt_pb_fixed32(&w, 4, p->time);
    }
    return w.err ? -1 : (int)w.len;
}

int purr_mt_pos_get(const uint8_t *in, size_t n, purr_mt_pos_t *p)
{
    memset(p, 0, sizeof(*p));
    purr_mt_pb_r_t r;
    purr_mt_pb_field_t f;
    purr_mt_pb_r_init(&r, in, n);
    int rc;
    while ((rc = purr_mt_pb_next(&r, &f)) > 0) {
        switch (f.field) {
        case 1: p->lat_e7 = (int32_t)(uint32_t)f.v; break;
        case 2: p->lon_e7 = (int32_t)(uint32_t)f.v; break;
        case 3: p->alt_m = (int32_t)(int64_t)f.v; p->has_alt = 1; break;
        case 4: p->time = (uint32_t)f.v; break;
        default: break;
        }
    }
    return rc < 0 ? -1 : 0;
}

int purr_mt_routing_put(uint32_t error_reason, uint8_t *out, size_t cap)
{
    purr_mt_pb_w_t w;
    purr_mt_pb_w_init(&w, out, cap);
    purr_mt_pb_varint(&w, 3, error_reason);                             /* a member of a oneof: written even when it is 0 (an acknowledgement is "18 00") */
    return w.err ? -1 : (int)w.len;
}

int purr_mt_routing_get(const uint8_t *in, size_t n, uint32_t *error_reason)
{
    *error_reason = 0;
    purr_mt_pb_r_t r;
    purr_mt_pb_field_t f;
    purr_mt_pb_r_init(&r, in, n);
    int rc;
    while ((rc = purr_mt_pb_next(&r, &f)) > 0) {
        if (f.field == 3) {
            *error_reason = (uint32_t)f.v;
        }
    }
    return rc < 0 ? -1 : 0;
}

/* ---------------------------------------------------------------- regions and presets */

static const purr_mt_region_t k_regions[] = {
    {"US", 902000000u, 928000000u, 30, 100},
    {"EU_433", 433000000u, 434000000u, 12, 10},
    {"EU_868", 869400000u, 869650000u, 27, 10},
    {"CN", 470000000u, 510000000u, 19, 100},
    {"JP", 920500000u, 923500000u, 13, 100},
    {"ANZ", 915000000u, 928000000u, 30, 100},
    {"KR", 920000000u, 923000000u, 23, 100},
    {"TW", 920000000u, 925000000u, 27, 100},
    {"RU", 868700000u, 869200000u, 20, 100},
    {"IN", 865000000u, 867000000u, 30, 100},
    {"NZ_865", 864000000u, 868000000u, 36, 100},
    {"TH", 920000000u, 925000000u, 16, 100},
    {"UA_433", 433000000u, 434700000u, 10, 10},
    {"UA_868", 868000000u, 868600000u, 14, 1},
    {"MY_433", 433000000u, 435000000u, 20, 100},
    {"MY_919", 919000000u, 924000000u, 27, 100},
    {"SG_923", 917000000u, 925000000u, 20, 100},
};

static const purr_mt_preset_t k_presets[] = {
    {"LongFast", 250000u, 11, 5, 16},
    {"LongSlow", 125000u, 12, 8, 16},
    {"VeryLongSlow", 62500u, 12, 8, 16},
    {"MediumSlow", 250000u, 10, 5, 16},
    {"MediumFast", 250000u, 9, 5, 16},
    {"ShortSlow", 250000u, 8, 5, 16},
    {"ShortFast", 250000u, 7, 5, 16},
    {"LongModerate", 125000u, 11, 8, 16},
    {"ShortTurbo", 500000u, 7, 5, 16},
};

int purr_mt_region_count(void) { return (int)(sizeof(k_regions) / sizeof(k_regions[0])); }
const purr_mt_region_t *purr_mt_region(int i) { return i >= 0 && i < purr_mt_region_count() ? &k_regions[i] : NULL; }
int purr_mt_preset_count(void) { return (int)(sizeof(k_presets) / sizeof(k_presets[0])); }
const purr_mt_preset_t *purr_mt_preset(int i) { return i >= 0 && i < purr_mt_preset_count() ? &k_presets[i] : NULL; }

static int str_eq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

const purr_mt_region_t *purr_mt_region_by_name(const char *name)
{
    for (int i = 0; i < purr_mt_region_count(); i++) {
        if (str_eq(k_regions[i].name, name)) {
            return &k_regions[i];
        }
    }
    return NULL;
}

const purr_mt_preset_t *purr_mt_preset_by_name(const char *name)
{
    for (int i = 0; i < purr_mt_preset_count(); i++) {
        if (str_eq(k_presets[i].name, name)) {
            return &k_presets[i];
        }
    }
    return NULL;
}

uint32_t purr_mt_hash(const char *s)
{
    uint32_t h = 5381;
    for (; *s != '\0'; s++) {
        h = ((h << 5) + h) + (uint8_t)*s;
    }
    return h;
}

static uint32_t slots_in(const purr_mt_region_t *r, const purr_mt_preset_t *p)
{
    uint32_t n = (r->end_hz - r->start_hz) / p->bw_hz;
    return n == 0 ? 1 : n;
}

int purr_mt_slot(const purr_mt_region_t *r, const purr_mt_preset_t *p, const char *channel_name, int slot_override)
{
    uint32_t n = slots_in(r, p);
    if (slot_override > 0) {
        return (int)(((uint32_t)slot_override - 1) % n);
    }
    return (int)(purr_mt_hash(channel_name[0] != '\0' ? channel_name : p->name) % n);
}

uint32_t purr_mt_freq_hz(const purr_mt_region_t *r, const purr_mt_preset_t *p, int slot)
{
    return r->start_hz + p->bw_hz / 2 + (uint32_t)slot * p->bw_hz;
}

/* ---------------------------------------------------------------- channel share links */

static const char *k_region_enum[] = {"", "US", "EU_433", "EU_868", "CN", "JP", "ANZ", "KR", "TW", "RU", "IN", "NZ_865", "TH", "", "UA_433", "UA_868", "MY_433", "MY_919", "SG_923"};
static const char *k_preset_enum[] = {"LongFast", "LongSlow", "VeryLongSlow", "MediumSlow", "MediumFast", "ShortSlow", "ShortFast", "LongModerate", "ShortTurbo"};
#define N_REGION_ENUM ((int)(sizeof(k_region_enum) / sizeof(k_region_enum[0])))
#define N_PRESET_ENUM ((int)(sizeof(k_preset_enum) / sizeof(k_preset_enum[0])))

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return c == '-' ? 62 : 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

/* base64 or base64url, with or without padding */
static int b64_decode(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; *s != '\0'; s++) {
        if (*s == '=' || *s == '\n' || *s == '\r' || *s == ' ') {
            continue;
        }
        int v = b64_val(*s);
        if (v < 0) {
            return -1;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) {
                return -1;
            }
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)n;
}

static int b64_encode(const uint8_t *in, size_t n, char *out, size_t cap)      /* base64url, no padding */
{
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        acc = (acc << 8) | in[i];
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            if (o + 1 >= cap) {
                return -1;
            }
            out[o++] = a[(acc >> bits) & 63];
        }
    }
    if (bits > 0) {
        if (o + 1 >= cap) {
            return -1;
        }
        out[o++] = a[(acc << (6 - bits)) & 63];
    }
    out[o] = '\0';
    return (int)o;
}

int purr_mt_url_parse(const char *url, purr_mt_url_t *out)
{
    memset(out, 0, sizeof(*out));
    const char *frag = url;
    for (const char *p = url; *p != '\0'; p++) {
        if (*p == '#') {
            frag = p + 1;
        }
    }
    uint8_t buf[512];
    int n = b64_decode(frag, buf, sizeof(buf));
    if (n <= 0) {
        return -1;
    }
    purr_mt_pb_r_t r;
    purr_mt_pb_field_t f;
    purr_mt_pb_r_init(&r, buf, (size_t)n);
    int rc;
    while ((rc = purr_mt_pb_next(&r, &f)) > 0) {
        if (f.field == 1 && f.wire == 2 && out->count < 8) {                /* a ChannelSettings */
            purr_mt_url_channel_t *c = &out->ch[out->count];
            purr_mt_pb_r_t cr;
            purr_mt_pb_field_t cf;
            purr_mt_pb_r_init(&cr, f.data, f.len);
            int crc;
            while ((crc = purr_mt_pb_next(&cr, &cf)) > 0) {
                if (cf.field == 2 && cf.wire == 2 && cf.len <= 32) {
                    memcpy(c->psk, cf.data, cf.len);
                    c->psk_len = (int)cf.len;
                } else if (cf.field == 3 && cf.wire == 2) {
                    copy_str(c->name, sizeof(c->name), cf.data, cf.len);
                }
            }
            if (crc < 0) {
                return -1;
            }
            out->count++;
        } else if (f.field == 2 && f.wire == 2) {                           /* the LoRaConfig */
            purr_mt_pb_r_t lr;
            purr_mt_pb_field_t lf;
            purr_mt_pb_r_init(&lr, f.data, f.len);
            int lrc;
            int preset = 0, region = 0;
            while ((lrc = purr_mt_pb_next(&lr, &lf)) > 0) {
                if (lf.field == 2 && lf.wire == 0) {
                    preset = (int)lf.v;
                } else if (lf.field == 7 && lf.wire == 0) {
                    region = (int)lf.v;
                }
            }
            if (lrc < 0) {
                return -1;
            }
            if (region > 0 && region < N_REGION_ENUM && k_region_enum[region][0] != '\0') {
                copy_str(out->region, sizeof(out->region), (const uint8_t *)k_region_enum[region], slen(k_region_enum[region]));
            }
            if (preset >= 0 && preset < N_PRESET_ENUM) {
                copy_str(out->preset, sizeof(out->preset), (const uint8_t *)k_preset_enum[preset], slen(k_preset_enum[preset]));
            }
        }
    }
    return rc < 0 || out->count == 0 ? -1 : 0;
}

int purr_mt_url_make(const purr_mt_url_t *in, char *out, size_t cap)
{
    static const char prefix[] = "https://meshtastic.org/e/#";
    size_t pl = sizeof(prefix) - 1;
    if (cap <= pl) {
        return -1;
    }
    uint8_t buf[512];
    purr_mt_pb_w_t w;
    purr_mt_pb_w_init(&w, buf, sizeof(buf));
    for (int i = 0; i < in->count && i < 8; i++) {
        uint8_t cs[64];
        purr_mt_pb_w_t cw;
        purr_mt_pb_w_init(&cw, cs, sizeof(cs));
        if (in->ch[i].psk_len > 0) {
            purr_mt_pb_bytes(&cw, 2, in->ch[i].psk, (size_t)in->ch[i].psk_len);
        }
        if (in->ch[i].name[0] != '\0') {
            purr_mt_pb_bytes(&cw, 3, (const uint8_t *)in->ch[i].name, slen(in->ch[i].name));
        }
        if (cw.err) {
            return -1;
        }
        purr_mt_pb_bytes(&w, 1, cs, cw.len);
    }
    int region = 0, preset = -1;
    for (int i = 1; i < N_REGION_ENUM; i++) {
        if (in->region[0] != '\0' && str_eq(k_region_enum[i], in->region)) {
            region = i;
        }
    }
    for (int i = 0; i < N_PRESET_ENUM; i++) {
        if (in->preset[0] != '\0' && str_eq(k_preset_enum[i], in->preset)) {
            preset = i;
        }
    }
    if (region != 0 || preset >= 0) {
        uint8_t ls[16];
        purr_mt_pb_w_t lw;
        purr_mt_pb_w_init(&lw, ls, sizeof(ls));
        purr_mt_pb_varint(&lw, 1, 1);                                       /* use_preset */
        if (preset > 0) {
            purr_mt_pb_varint(&lw, 2, (uint64_t)preset);
        }
        if (region != 0) {
            purr_mt_pb_varint(&lw, 7, (uint64_t)region);
        }
        purr_mt_pb_bytes(&w, 2, ls, lw.len);
    }
    if (w.err) {
        return -1;
    }
    memcpy(out, prefix, pl);
    int e = b64_encode(buf, w.len, out + pl, cap - pl);
    return e < 0 ? -1 : (int)pl + e;
}
