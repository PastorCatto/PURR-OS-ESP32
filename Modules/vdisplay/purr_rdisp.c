/* purr_rdisp.c - see purr_rdisp.h and RemoteDisplay/SPEC.md. */
#include "purr_rdisp.h"

#include <string.h>

/* ---------------------------------------------------------------- little-endian helpers */

static void w16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static unsigned r16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* ---------------------------------------------------------------- messages */

int purr_rd_hdr_put(const purr_rd_hdr_t *h, uint8_t *out, int cap)
{
    if (h == NULL || out == NULL || cap < PURR_RD_HDR_LEN || h->len > PURR_RD_MAX_PAYLOAD) {
        return -1;
    }
    out[0] = h->type;
    out[1] = h->flags;
    w16(out + 2, h->seq);
    w32(out + 4, h->len);
    return PURR_RD_HDR_LEN;
}

int purr_rd_hdr_get(const uint8_t *in, int n, purr_rd_hdr_t *h)
{
    if (in == NULL || h == NULL || n < PURR_RD_HDR_LEN) {
        return -1;
    }
    h->type = in[0];
    h->flags = in[1];
    h->seq = (uint16_t)r16(in + 2);
    h->len = r32(in + 4);
    return h->len > PURR_RD_MAX_PAYLOAD ? -2 : PURR_RD_HDR_LEN;
}

static void rect_put(uint8_t *p, const purr_rd_rect_t *r)
{
    w16(p, r->x);
    w16(p + 2, r->y);
    w16(p + 4, r->w);
    w16(p + 6, r->h);
}

static void rect_get(const uint8_t *p, purr_rd_rect_t *r)
{
    r->x = (uint16_t)r16(p);
    r->y = (uint16_t)r16(p + 2);
    r->w = (uint16_t)r16(p + 4);
    r->h = (uint16_t)r16(p + 6);
}

int purr_rd_rect_put(const purr_rd_rect_msg_t *m, uint8_t *out, int cap)
{
    if (m == NULL || out == NULL || cap < PURR_RD_RECT_HEAD) {
        return -1;
    }
    rect_put(out, &m->r);
    out[8] = m->codec;
    out[9] = m->fmt;
    return PURR_RD_RECT_HEAD;
}

int purr_rd_rect_get(const uint8_t *in, int n, purr_rd_rect_msg_t *m)
{
    if (in == NULL || m == NULL || n < PURR_RD_RECT_HEAD) {
        return -1;
    }
    rect_get(in, &m->r);
    m->codec = in[8];
    m->fmt = in[9];
    return PURR_RD_RECT_HEAD;
}

int purr_rd_fill_put(const purr_rd_fill_t *m, uint8_t *out, int cap)
{
    if (m == NULL || out == NULL || cap < PURR_RD_FILL_LEN) {
        return -1;
    }
    rect_put(out, &m->r);
    w16(out + 8, m->colour);
    return PURR_RD_FILL_LEN;
}

int purr_rd_fill_get(const uint8_t *in, int n, purr_rd_fill_t *m)
{
    if (in == NULL || m == NULL || n < PURR_RD_FILL_LEN) {
        return -1;
    }
    rect_get(in, &m->r);
    m->colour = (uint16_t)r16(in + 8);
    return PURR_RD_FILL_LEN;
}

int purr_rd_copy_put(const purr_rd_copy_t *m, uint8_t *out, int cap)
{
    if (m == NULL || out == NULL || cap < PURR_RD_COPY_LEN) {
        return -1;
    }
    w16(out, m->sx);
    w16(out + 2, m->sy);
    w16(out + 4, m->w);
    w16(out + 6, m->h);
    w16(out + 8, m->dx);
    w16(out + 10, m->dy);
    return PURR_RD_COPY_LEN;
}

int purr_rd_copy_get(const uint8_t *in, int n, purr_rd_copy_t *m)
{
    if (in == NULL || m == NULL || n < PURR_RD_COPY_LEN) {
        return -1;
    }
    m->sx = (uint16_t)r16(in);
    m->sy = (uint16_t)r16(in + 2);
    m->w = (uint16_t)r16(in + 4);
    m->h = (uint16_t)r16(in + 6);
    m->dx = (uint16_t)r16(in + 8);
    m->dy = (uint16_t)r16(in + 10);
    return PURR_RD_COPY_LEN;
}

int purr_rd_input_put(const purr_rd_input_t *m, uint8_t *out, int cap)
{
    if (m == NULL || out == NULL || cap < PURR_RD_INPUT_LEN) {
        return -1;
    }
    out[0] = m->kind;
    out[1] = m->id;
    w16(out + 2, m->x);
    w16(out + 4, m->y);
    return PURR_RD_INPUT_LEN;
}

int purr_rd_input_get(const uint8_t *in, int n, purr_rd_input_t *m)
{
    if (in == NULL || m == NULL || n < PURR_RD_INPUT_LEN) {
        return -1;
    }
    m->kind = in[0];
    m->id = in[1];
    m->x = (uint16_t)r16(in + 2);
    m->y = (uint16_t)r16(in + 4);
    return PURR_RD_INPUT_LEN;
}

int purr_rd_hello_put(const purr_rd_hello_t *m, uint8_t *out, int cap)
{
    if (m == NULL || out == NULL || cap < PURR_RD_HELLO_LEN) {
        return -1;
    }
    out[0] = m->major;
    out[1] = m->minor;
    w16(out + 2, m->width);
    w16(out + 4, m->height);
    out[6] = m->formats;
    out[7] = m->codecs;
    w16(out + 8, m->caps);
    w16(out + 10, m->max_px);
    w16(out + 12, m->rx_buf);
    memcpy(out + 14, m->board, 16);
    memcpy(out + 30, m->pubkey, 64);
    return PURR_RD_HELLO_LEN;
}

int purr_rd_hello_get(const uint8_t *in, int n, purr_rd_hello_t *m)
{
    if (in == NULL || m == NULL || n < PURR_RD_HELLO_LEN) {
        return -1;
    }
    m->major = in[0];
    m->minor = in[1];
    m->width = (uint16_t)r16(in + 2);
    m->height = (uint16_t)r16(in + 4);
    m->formats = in[6];
    m->codecs = in[7];
    m->caps = (uint16_t)r16(in + 8);
    m->max_px = (uint16_t)r16(in + 10);
    m->rx_buf = (uint16_t)r16(in + 12);
    memcpy(m->board, in + 14, 16);
    m->board[15] = '\0';                         /* a name from the wire is always a string */
    memcpy(m->pubkey, in + 30, 64);
    return PURR_RD_HELLO_LEN;
}

int purr_rd_welcome_put(const purr_rd_welcome_t *m, uint8_t *out, int cap)
{
    if (m == NULL || out == NULL || cap < PURR_RD_WELCOME_LEN) {
        return -1;
    }
    out[0] = m->major;
    out[1] = m->minor;
    out[2] = m->format;
    out[3] = m->codecs;
    w16(out + 4, m->width);
    w16(out + 6, m->height);
    w16(out + 8, m->max_px);
    out[10] = m->window;
    out[11] = m->reserved;
    w32(out + 12, m->session);
    return PURR_RD_WELCOME_LEN;
}

int purr_rd_welcome_get(const uint8_t *in, int n, purr_rd_welcome_t *m)
{
    if (in == NULL || m == NULL || n < PURR_RD_WELCOME_LEN) {
        return -1;
    }
    m->major = in[0];
    m->minor = in[1];
    m->format = in[2];
    m->codecs = in[3];
    m->width = (uint16_t)r16(in + 4);
    m->height = (uint16_t)r16(in + 6);
    m->max_px = (uint16_t)r16(in + 8);
    m->window = in[10];
    m->reserved = in[11];
    m->session = r32(in + 12);
    return PURR_RD_WELCOME_LEN;
}

/* ---------------------------------------------------------------- codecs */

int purr_rd_raw_bytes(int fmt, int w, int h)
{
    if (w < 0 || h < 0) {
        return -1;
    }
    if (fmt == PURR_RD_FMT_RGB565) {
        return w * h * 2;
    }
    if (fmt == PURR_RD_FMT_MONO1) {
        return ((w + 7) / 8) * h;
    }
    return -1;
}

static int same_unit(const uint8_t *src, int a, int b)
{
    return src[2 * a] == src[2 * b] && src[2 * a + 1] == src[2 * b + 1];
}

int purr_rd_rle16_encode(const uint8_t *src, int n_bytes, uint8_t *dst, int cap)
{
    if (src == NULL || dst == NULL || n_bytes < 0 || (n_bytes & 1) || cap < 0) {
        return -1;
    }
    int n = n_bytes / 2, i = 0, o = 0;
    while (i < n) {
        int run = 1;
        while (i + run < n && run < 128 && same_unit(src, i, i + run)) {
            run++;
        }
        if (run >= 2) {
            if (o + 3 > cap) {
                return -1;
            }
            dst[o++] = (uint8_t)(0x80 | (run - 1));
            dst[o++] = src[2 * i];
            dst[o++] = src[2 * i + 1];
            i += run;
            continue;
        }
        int j = i + 1;                           /* literals until a run of three starts, at most 128 */
        while (j < n && j - i < 128 && !(j + 2 < n && same_unit(src, j, j + 1) && same_unit(src, j, j + 2))) {
            j++;
        }
        int lit = j - i;
        if (o + 1 + 2 * lit > cap) {
            return -1;
        }
        dst[o++] = (uint8_t)(lit - 1);
        memcpy(dst + o, src + 2 * i, (size_t)(2 * lit));
        o += 2 * lit;
        i = j;
    }
    return o;
}

int purr_rd_rle16_decode(const uint8_t *src, int n, uint8_t *dst, int n_bytes)
{
    if (src == NULL || dst == NULL || n < 0 || n_bytes < 0) {
        return -1;
    }
    int i = 0, o = 0;
    while (i < n) {
        int c = src[i++];
        int cnt = (c & 0x7F) + 1;
        if (o + 2 * cnt > n_bytes) {
            return -1;
        }
        if (c & 0x80) {
            if (i + 2 > n) {
                return -1;
            }
            uint8_t a = src[i], b = src[i + 1];
            i += 2;
            for (int k = 0; k < cnt; k++) {
                dst[o++] = a;
                dst[o++] = b;
            }
        } else {
            if (i + 2 * cnt > n) {
                return -1;
            }
            memcpy(dst + o, src + i, (size_t)(2 * cnt));
            i += 2 * cnt;
            o += 2 * cnt;
        }
    }
    return o == n_bytes ? 0 : -1;
}

/* LZ4 block format: a sequence is a token (high nibble: literal count, low: match length - 4; 15 means more bytes follow, each added, until one is not 255), the literals, a 2-byte
 * offset and the match's extra length bytes. The last sequence has literals only. The format's own rules: the last match starts at least 12 bytes before the end and the last 5 bytes are literals. */
static uint32_t rd32(const uint8_t *p) { return r32(p); }

static int ext_bytes(int v)                     /* the extra length bytes a count of v needs when its nibble is 15 */
{
    return v >= 15 ? (v - 15) / 255 + 1 : 0;
}

static int put_ext(uint8_t *dst, int op, int v)
{
    if (v >= 15) {
        int rem = v - 15;
        while (rem >= 255) {
            dst[op++] = 255;
            rem -= 255;
        }
        dst[op++] = (uint8_t)rem;
    }
    return op;
}

int purr_rd_lz4_encode(const uint8_t *src, int n, uint8_t *dst, int cap, uint16_t *table)
{
    if (src == NULL || dst == NULL || n < 0 || n > 65534 || cap < 0 || (n >= 13 && table == NULL)) {
        return -1;
    }
    int ip = 0, anchor = 0, op = 0;
    if (n >= 13) {
        for (int i = 0; i < PURR_RD_LZ4_TABLE; i++) {
            table[i] = 0xFFFF;
        }
        const int matchlimit = n - 5;
        while (ip <= n - 12) {
            uint32_t v = rd32(src + ip);
            uint32_t h = (v * 2654435761u) >> 20;
            int ref = table[h];
            table[h] = (uint16_t)ip;
            if (ref != 0xFFFF && ref < ip && rd32(src + ref) == v) {
                int mlen = 4;
                while (ip + mlen < matchlimit && src[ref + mlen] == src[ip + mlen]) {
                    mlen++;
                }
                int lit = ip - anchor, m = mlen - 4;
                if (op + 1 + ext_bytes(lit) + lit + 2 + ext_bytes(m) > cap) {
                    return -1;
                }
                int tpos = op++;
                op = put_ext(dst, op, lit);
                memcpy(dst + op, src + anchor, (size_t)lit);
                op += lit;
                dst[op++] = (uint8_t)(ip - ref);
                dst[op++] = (uint8_t)((ip - ref) >> 8);
                op = put_ext(dst, op, m);
                dst[tpos] = (uint8_t)(((lit >= 15 ? 15 : lit) << 4) | (m >= 15 ? 15 : m));
                ip += mlen;
                anchor = ip;
            } else {
                ip++;
            }
        }
    }
    int lit = n - anchor;
    if (op + 1 + ext_bytes(lit) + lit > cap) {
        return -1;
    }
    int tpos = op++;
    op = put_ext(dst, op, lit);
    memcpy(dst + op, src + anchor, (size_t)lit);
    op += lit;
    dst[tpos] = (uint8_t)((lit >= 15 ? 15 : lit) << 4);
    return op;
}

int purr_rd_lz4_decode(const uint8_t *src, int n, uint8_t *dst, int n_bytes)
{
    if (src == NULL || dst == NULL || n < 0 || n_bytes < 0) {
        return -1;
    }
    int ip = 0, op = 0;
    while (ip < n) {
        int token = src[ip++];
        int lit = token >> 4;
        if (lit == 15) {
            int s;
            do {
                if (ip >= n) {
                    return -1;
                }
                s = src[ip++];
                lit += s;
                if (lit > n_bytes + n) {         /* far past anything real: stop before the sum can wrap */
                    return -1;
                }
            } while (s == 255);
        }
        if (lit > n - ip || lit > n_bytes - op) {
            return -1;
        }
        memcpy(dst + op, src + ip, (size_t)lit);
        ip += lit;
        op += lit;
        if (ip == n) {
            break;                               /* the last sequence has literals only */
        }
        if (n - ip < 2) {
            return -1;
        }
        int off = src[ip] | (src[ip + 1] << 8);
        ip += 2;
        if (off == 0 || off > op) {
            return -1;
        }
        int ml = token & 15;
        if (ml == 15) {
            int s;
            do {
                if (ip >= n) {
                    return -1;
                }
                s = src[ip++];
                ml += s;
                if (ml > n_bytes) {
                    return -1;
                }
            } while (s == 255);
        }
        ml += 4;
        if (ml > n_bytes - op) {
            return -1;
        }
        for (int k = 0; k < ml; k++) {           /* a match may overlap what it is copying (a run) */
            dst[op + k] = dst[op + k - off];
        }
        op += ml;
    }
    return op == n_bytes ? 0 : -1;
}

int purr_rd_encode_best(const uint8_t *raw, int n, int fmt, unsigned codec_mask, uint8_t *out, int cap, uint16_t *lz_table, uint8_t *codec)
{
    if (raw == NULL || out == NULL || codec == NULL || n < 0 || cap < 0) {
        return -1;
    }
    int limit = cap < n - 1 ? cap : n - 1;       /* a coded form must be smaller than the raw bytes to be worth sending */
    int rle = -1, lz = -1;
    if (limit > 0 && (codec_mask & PURR_RD_CODEC_BIT(PURR_RD_CODEC_RLE16)) && fmt == PURR_RD_FMT_RGB565) {
        rle = purr_rd_rle16_encode(raw, n, out, limit);
    }
    if (limit > 0 && (codec_mask & PURR_RD_CODEC_BIT(PURR_RD_CODEC_LZ4)) && lz_table != NULL) {
        int lz_limit = (rle > 0 && rle - 1 < limit) ? rle - 1 : limit;      /* only worth keeping if it beats the run-length form */
        if (lz_limit > 0) {
            lz = purr_rd_lz4_encode(raw, n, out, lz_limit, lz_table);       /* overwrites a run-length form in out: it is made again below if LZ4 does not win */
        }
    }
    if (lz > 0) {
        *codec = PURR_RD_CODEC_LZ4;
        return lz;
    }
    if (rle > 0) {
        /* `out` may have been overwritten by a failed LZ4 attempt: make the run-length form again (it fit before, so it fits now) */
        rle = purr_rd_rle16_encode(raw, n, out, limit);
        if (rle > 0) {
            *codec = PURR_RD_CODEC_RLE16;
            return rle;
        }
    }
    if (cap < n) {
        return -1;
    }
    memcpy(out, raw, (size_t)n);
    *codec = PURR_RD_CODEC_RAW;
    return n;
}

int purr_rd_decode(int codec, const uint8_t *in, int n, uint8_t *dst, int n_bytes)
{
    switch (codec) {
    case PURR_RD_CODEC_RAW:
        if (in == NULL || dst == NULL || n != n_bytes || n < 0) {
            return -1;
        }
        memcpy(dst, in, (size_t)n);
        return 0;
    case PURR_RD_CODEC_RLE16:
        return purr_rd_rle16_decode(in, n, dst, n_bytes);
    case PURR_RD_CODEC_LZ4:
        return purr_rd_lz4_decode(in, n, dst, n_bytes);
    default:
        return -1;
    }
}

/* ---------------------------------------------------------------- rectangles */

int purr_rd_split(purr_rd_rect_t in, int max_px, purr_rd_rect_t *out, int max_out)
{
    if (max_px < 1 || out == NULL || max_out < 0) {
        return -1;
    }
    if (in.w == 0 || in.h == 0) {
        return 0;
    }
    int n = 0;
    if (in.w <= max_px) {
        int band = max_px / in.w;                /* whole rows per piece */
        for (int y = 0; y < in.h; y += band) {
            if (n >= max_out) {
                return -1;
            }
            int hh = in.h - y < band ? in.h - y : band;
            out[n++] = (purr_rd_rect_t){in.x, (uint16_t)(in.y + y), in.w, (uint16_t)hh};
        }
        return n;
    }
    for (int y = 0; y < in.h; y++) {             /* wider than a piece may be: one row at a time, in segments */
        for (int x = 0; x < in.w; x += max_px) {
            if (n >= max_out) {
                return -1;
            }
            int ww = in.w - x < max_px ? in.w - x : max_px;
            out[n++] = (purr_rd_rect_t){(uint16_t)(in.x + x), (uint16_t)(in.y + y), (uint16_t)ww, 1};
        }
    }
    return n;
}

static long area(const purr_rd_rect_t *r) { return (long)r->w * r->h; }

static purr_rd_rect_t unite(const purr_rd_rect_t *a, const purr_rd_rect_t *b)
{
    int x0 = a->x < b->x ? a->x : b->x, y0 = a->y < b->y ? a->y : b->y;
    int x1 = a->x + a->w > b->x + b->w ? a->x + a->w : b->x + b->w;
    int y1 = a->y + a->h > b->y + b->h ? a->y + a->h : b->y + b->h;
    return (purr_rd_rect_t){(uint16_t)x0, (uint16_t)y0, (uint16_t)(x1 - x0), (uint16_t)(y1 - y0)};
}

static int overlaps(const purr_rd_rect_t *a, const purr_rd_rect_t *b)
{
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

void purr_rd_damage_init(purr_rd_damage_t *d, int screen_w, int screen_h)
{
    memset(d, 0, sizeof(*d));
    d->w = screen_w;
    d->h = screen_h;
}

static void remove_at(purr_rd_damage_t *d, int i)
{
    d->r[i] = d->r[d->n - 1];
    d->n--;
}

void purr_rd_damage_add(purr_rd_damage_t *d, int x, int y, int w, int h)
{
    int x1 = x + w, y1 = y + h;
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (x1 > d->w) {
        x1 = d->w;
    }
    if (y1 > d->h) {
        y1 = d->h;
    }
    if (x1 <= x || y1 <= y) {
        return;
    }
    purr_rd_rect_t cur = {(uint16_t)x, (uint16_t)y, (uint16_t)(x1 - x), (uint16_t)(y1 - y)};
    int again = 1;
    while (again) {                              /* fold in everything it overlaps, or that costs little to join (a union at most a quarter bigger than the two) */
        again = 0;
        for (int i = 0; i < d->n; i++) {
            purr_rd_rect_t u = unite(&d->r[i], &cur);
            if (overlaps(&d->r[i], &cur) || area(&u) * 4 <= (area(&d->r[i]) + area(&cur)) * 5) {
                cur = u;
                remove_at(d, i);
                again = 1;
                break;
            }
        }
    }
    if (d->n == PURR_RD_DAMAGE_MAX) {            /* full: join the pair, the new one included, that costs least */
        int bi = -1;
        long best = 0;
        for (int i = 0; i < d->n; i++) {
            purr_rd_rect_t u = unite(&d->r[i], &cur);
            long cost = area(&u) - area(&d->r[i]) - area(&cur);
            if (bi < 0 || cost < best) {
                bi = i;
                best = cost;
            }
        }
        int pi = -1, pj = -1;
        for (int i = 0; i < d->n; i++) {
            for (int j = i + 1; j < d->n; j++) {
                purr_rd_rect_t u = unite(&d->r[i], &d->r[j]);
                long cost = area(&u) - area(&d->r[i]) - area(&d->r[j]);
                if (cost < best) {
                    pi = i;
                    pj = j;
                    best = cost;
                    bi = -1;
                }
            }
        }
        if (bi >= 0) {
            cur = unite(&d->r[bi], &cur);
            remove_at(d, bi);
        } else {
            d->r[pi] = unite(&d->r[pi], &d->r[pj]);
            remove_at(d, pj);
        }
        /* the joined rectangle may now overlap another: one more fold keeps the set tidy */
        for (int i = 0; i < d->n; i++) {
            if (overlaps(&d->r[i], &cur)) {
                cur = unite(&d->r[i], &cur);
                remove_at(d, i);
                i = -1;
            }
        }
    }
    d->r[d->n++] = cur;
}

int purr_rd_damage_take(purr_rd_damage_t *d, purr_rd_rect_t *out, int max)
{
    int n = d->n < max ? d->n : max;
    for (int i = 0; i < n; i++) {
        out[i] = d->r[i];
    }
    d->n = 0;
    return n;
}
