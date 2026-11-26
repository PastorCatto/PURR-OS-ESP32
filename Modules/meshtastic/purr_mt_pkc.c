/* purr_mt_pkc.c - see purr_mt_pkc.h. The curve arithmetic is the public-domain TweetNaCl construction (Bernstein, Lange, Schwabe and others), slow and small, which is right for a few
 * direct messages an hour. */
#include "purr_mt_pkc.h"

#include <string.h>

#include "purr_mt.h"
#include "../proximity/purr_px_crypto.h"

/* ---------------------------------------------------------------- X25519 */

typedef int64_t gf[16];
static const gf k121665 = {0xDB41, 1};

static void car25519(gf o)
{
    int64_t c;
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void sel25519(gf p, gf q, int b)
{
    int64_t c = ~((int64_t)b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t *o, const gf n)
{
    gf m, t;
    for (int i = 0; i < 16; i++) {
        t[i] = n[i];
    }
    car25519(t);
    car25519(t);
    car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i] = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static void unpack25519(gf o, const uint8_t *n)
{
    for (int i = 0; i < 16; i++) {
        o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    }
    o[15] &= 0x7fff;
}

static void fadd(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void fsub(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

static void fmul(gf o, const gf a, const gf b)
{
    int64_t t[31];
    for (int i = 0; i < 31; i++) {
        t[i] = 0;
    }
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) {
            t[i + j] += a[i] * b[j];
        }
    }
    for (int i = 0; i < 15; i++) {
        t[i] += 38 * t[i + 16];
    }
    for (int i = 0; i < 16; i++) {
        o[i] = t[i];
    }
    car25519(o);
    car25519(o);
}

static void fsq(gf o, const gf a) { fmul(o, a, a); }

static void finv(gf o, const gf in)
{
    gf c;
    for (int a = 0; a < 16; a++) {
        c[a] = in[a];
    }
    for (int a = 253; a >= 0; a--) {
        fsq(c, c);
        if (a != 2 && a != 4) {
            fmul(c, c, in);
        }
    }
    for (int a = 0; a < 16; a++) {
        o[a] = c[a];
    }
}

void purr_mt_x25519(uint8_t q[32], const uint8_t n[32], const uint8_t *p)
{
    uint8_t z[32];
    int64_t x[80];
    gf a, b, c, d, e, f;
    for (int i = 0; i < 31; i++) {
        z[i] = n[i];
    }
    z[31] = (uint8_t)((n[31] & 127) | 64);
    z[0] &= 248;
    uint8_t base[32] = {9};
    unpack25519(x, p != NULL ? p : base);
    for (int i = 0; i < 16; i++) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r);
        sel25519(c, d, r);
        fadd(e, a, c);
        fsub(a, a, c);
        fadd(c, b, d);
        fsub(b, b, d);
        fsq(d, e);
        fsq(f, a);
        fmul(a, c, a);
        fmul(c, b, e);
        fadd(e, a, c);
        fsub(a, a, c);
        fsq(b, a);
        fsub(c, d, f);
        fmul(a, c, k121665);
        fadd(a, a, d);
        fmul(c, c, a);
        fmul(a, d, f);
        fmul(d, b, x);
        fsq(b, e);
        sel25519(a, b, r);
        sel25519(c, d, r);
    }
    for (int i = 0; i < 16; i++) {
        x[i + 16] = a[i];
        x[i + 32] = c[i];
        x[i + 48] = b[i];
        x[i + 64] = d[i];
    }
    finv(x + 32, x + 32);
    fmul(x + 16, x + 16, x + 32);
    pack25519(q, x + 16);
}

void purr_mt_pkc_keypair(const uint8_t random[32], uint8_t priv[32], uint8_t pub[32])
{
    memcpy(priv, random, 32);
    priv[0] &= 248;
    priv[31] = (uint8_t)((priv[31] & 127) | 64);
    purr_mt_x25519(pub, priv, NULL);
}

/* ---------------------------------------------------------------- AES-CCM (RFC 3610), L = 2 */

static void xor_into(uint8_t *a, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        a[i] = (uint8_t)(a[i] ^ b[i]);
    }
}

/* the CBC-MAC over B0, the length-prefixed associated data and the message; t is the full 16 byte result */
static void ccm_mac(const purr_mt_aes_t *a, const uint8_t nonce[13], int tag_len, const uint8_t *aad, size_t aad_len, const uint8_t *msg, size_t n, uint8_t t[16])
{
    uint8_t blk[16];
    blk[0] = (uint8_t)((aad_len > 0 ? 0x40 : 0) | (((tag_len - 2) / 2) << 3) | 1);
    memcpy(blk + 1, nonce, 13);
    blk[14] = (uint8_t)(n >> 8);
    blk[15] = (uint8_t)n;
    purr_mt_aes_block(a, blk, t);
    if (aad_len > 0) {
        uint8_t first[16];
        memset(first, 0, sizeof(first));
        size_t k = 0;
        first[k++] = (uint8_t)(aad_len >> 8);                           /* aad shorter than 0xFF00: a two byte length */
        first[k++] = (uint8_t)aad_len;
        size_t done = 0;
        while (done < aad_len) {
            size_t take = 16 - k < aad_len - done ? 16 - k : aad_len - done;
            memcpy(first + k, aad + done, take);
            k += take;
            done += take;
            xor_into(t, first, 16);
            purr_mt_aes_block(a, t, t);
            memset(first, 0, sizeof(first));
            k = 0;
        }
    }
    for (size_t off = 0; off < n; off += 16) {
        uint8_t m[16];
        memset(m, 0, sizeof(m));
        memcpy(m, msg + off, n - off < 16 ? n - off : 16);
        xor_into(t, m, 16);
        purr_mt_aes_block(a, t, t);
    }
}

static void ccm_ctr(const purr_mt_aes_t *a, const uint8_t nonce[13], uint8_t *data, size_t n, uint8_t s0[16])
{
    uint8_t ctr[16], ks[16];
    ctr[0] = 1;                                                         /* L - 1 */
    memcpy(ctr + 1, nonce, 13);
    ctr[14] = 0;
    ctr[15] = 0;
    purr_mt_aes_block(a, ctr, s0);                                      /* S0 masks the tag */
    for (size_t off = 0; off < n; off += 16) {
        for (int i = 15; i >= 14; i--) {
            if (++ctr[i] != 0) {
                break;
            }
        }
        purr_mt_aes_block(a, ctr, ks);
        size_t k = n - off < 16 ? n - off : 16;
        for (size_t i = 0; i < k; i++) {
            data[off + i] = (uint8_t)(data[off + i] ^ ks[i]);
        }
    }
}

int purr_mt_ccm_seal(const uint8_t *key, int keylen, const uint8_t nonce[13], int tag_len, const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t n, uint8_t *ct, uint8_t *tag)
{
    purr_mt_aes_t a;
    if (tag_len < 4 || tag_len > 16 || (tag_len & 1) || aad_len >= 0xFF00u || n > 0xFFFFu || purr_mt_aes_init(&a, key, keylen) != 0) {
        return -1;
    }
    uint8_t t[16], s0[16];
    ccm_mac(&a, nonce, tag_len, aad, aad_len, pt, n, t);
    if (ct != pt) {
        memcpy(ct, pt, n);
    }
    ccm_ctr(&a, nonce, ct, n, s0);
    for (int i = 0; i < tag_len; i++) {
        tag[i] = (uint8_t)(t[i] ^ s0[i]);
    }
    return 0;
}

int purr_mt_ccm_open(const uint8_t *key, int keylen, const uint8_t nonce[13], int tag_len, const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t n, const uint8_t *tag, uint8_t *pt)
{
    purr_mt_aes_t a;
    if (tag_len < 4 || tag_len > 16 || (tag_len & 1) || aad_len >= 0xFF00u || n > 0xFFFFu || purr_mt_aes_init(&a, key, keylen) != 0) {
        return -1;
    }
    uint8_t t[16], s0[16];
    if (pt != ct) {
        memcpy(pt, ct, n);
    }
    ccm_ctr(&a, nonce, pt, n, s0);
    ccm_mac(&a, nonce, tag_len, aad, aad_len, pt, n, t);
    uint8_t diff = 0;
    for (int i = 0; i < tag_len; i++) {
        diff |= (uint8_t)(tag[i] ^ t[i] ^ s0[i]);
    }
    if (diff != 0) {
        memset(pt, 0, n);
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- a mesh packet (VERIFY against the firmware) */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static int shared_key(const uint8_t my_priv[32], const uint8_t their_pub[32], uint8_t key[32])
{
    uint8_t s[32];
    purr_mt_x25519(s, my_priv, their_pub);
    uint8_t zero = 0;
    for (int i = 0; i < 32; i++) {
        zero |= s[i];
    }
    if (zero == 0) {
        return -1;                                                      /* a key of low order gives all zeros: refused */
    }
    purr_px_sha256(s, 32, key);
    return 0;
}

/* the nonce: the packet id as 64 bits, the sender, and the extra nonce; CCM takes the first 13 bytes */
static void pkc_nonce(uint8_t nonce[13], uint32_t from, uint32_t id, uint32_t extra)
{
    uint8_t n16[16];
    memset(n16, 0, sizeof(n16));
    put32(n16, id);
    put32(n16 + 8, from);
    put32(n16 + 12, extra);
    memcpy(nonce, n16, 13);
}

int purr_mt_pkc_seal(const uint8_t my_priv[32], const uint8_t their_pub[32], uint32_t from, uint32_t id, uint32_t extra_nonce, const uint8_t *pt, size_t n, uint8_t *out)
{
    uint8_t key[32], nonce[13];
    if (shared_key(my_priv, their_pub, key) != 0) {
        return -1;
    }
    pkc_nonce(nonce, from, id, extra_nonce);
    if (purr_mt_ccm_seal(key, 32, nonce, 8, NULL, 0, pt, n, out, out + n) != 0) {
        return -1;
    }
    put32(out + n + 8, extra_nonce);
    return (int)(n + PURR_MT_PKC_OVERHEAD);
}

int purr_mt_pkc_open(const uint8_t my_priv[32], const uint8_t their_pub[32], uint32_t from, uint32_t id, const uint8_t *in, size_t n, uint8_t *pt)
{
    if (n < PURR_MT_PKC_OVERHEAD) {
        return -1;
    }
    size_t body = n - PURR_MT_PKC_OVERHEAD;
    uint8_t key[32], nonce[13];
    if (shared_key(my_priv, their_pub, key) != 0) {
        return -1;
    }
    uint32_t extra = (uint32_t)in[body + 8] | ((uint32_t)in[body + 9] << 8) | ((uint32_t)in[body + 10] << 16) | ((uint32_t)in[body + 11] << 24);
    pkc_nonce(nonce, from, id, extra);
    if (purr_mt_ccm_open(key, 32, nonce, 8, NULL, 0, in, body, in + body, pt) != 0) {
        return -1;
    }
    return (int)body;
}
