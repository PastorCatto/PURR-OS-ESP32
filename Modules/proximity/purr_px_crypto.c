/* purr_crypto.c - see purr_px_crypto.h. */
#include "purr_px_crypto.h"

#include <string.h>

/* ---------------------------------------------------------------- SHA-256 */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(purr_px_sha256_t *c, const uint8_t *p)
{
    uint32_t w[64], a, b, cc, d, e, f, g, h;
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25), ch = (e & f) ^ (~e & g), t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22), mj = (a & b) ^ (a & cc) ^ (b & cc), t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void purr_px_sha256_init(purr_px_sha256_t *c)
{
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof(iv));
    c->len = 0;
    c->n = 0;
}

void purr_px_sha256_update(purr_px_sha256_t *c, const void *data, size_t n)
{
    const uint8_t *p = data;
    c->len += n;
    while (n > 0) {
        size_t take = 64 - (size_t)c->n < n ? 64 - (size_t)c->n : n;
        memcpy(c->buf + c->n, p, take);
        c->n += (int)take;
        p += take;
        n -= take;
        if (c->n == 64) {
            sha_block(c, c->buf);
            c->n = 0;
        }
    }
}

void purr_px_sha256_final(purr_px_sha256_t *c, uint8_t out[32])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    purr_px_sha256_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56) {
        purr_px_sha256_update(c, &pad, 1);
    }
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) {
        lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    purr_px_sha256_update(c, lb, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

void purr_px_sha256(const void *data, size_t n, uint8_t out[32])
{
    purr_px_sha256_t c;
    purr_px_sha256_init(&c);
    purr_px_sha256_update(&c, data, n);
    purr_px_sha256_final(&c, out);
}

void purr_px_hmac_sha256(const uint8_t *key, size_t klen, const void *data, size_t n, uint8_t out[32])
{
    uint8_t k[64], pad[64], inner[32];
    memset(k, 0, sizeof(k));
    if (klen > 64) {
        purr_px_sha256(key, klen, k);
    } else if (klen > 0) {
        memcpy(k, key, klen);
    }
    purr_px_sha256_t c;
    for (int i = 0; i < 64; i++) {
        pad[i] = k[i] ^ 0x36;
    }
    purr_px_sha256_init(&c);
    purr_px_sha256_update(&c, pad, 64);
    purr_px_sha256_update(&c, data, n);
    purr_px_sha256_final(&c, inner);
    for (int i = 0; i < 64; i++) {
        pad[i] = k[i] ^ 0x5c;
    }
    purr_px_sha256_init(&c);
    purr_px_sha256_update(&c, pad, 64);
    purr_px_sha256_update(&c, inner, 32);
    purr_px_sha256_final(&c, out);
}

void purr_px_hkdf_sha256(const uint8_t *salt, size_t slen, const uint8_t *ikm, size_t ilen, const uint8_t *info, size_t infolen, uint8_t *out, size_t out_len)
{
    uint8_t zero[32] = {0}, prk[32], t[32];
    purr_px_hmac_sha256(slen > 0 ? salt : zero, slen > 0 ? slen : 32, ikm, ilen, prk);
    size_t done = 0;
    uint8_t counter = 1, buf[32 + 255 + 1];
    size_t tl = 0;
    while (done < out_len) {
        size_t m = 0;
        memcpy(buf, t, tl);
        m += tl;
        if (infolen > 0 && infolen <= 255) {
            memcpy(buf + m, info, infolen);
            m += infolen;
        }
        buf[m++] = counter++;
        purr_px_hmac_sha256(prk, 32, buf, m, t);
        tl = 32;
        size_t take = out_len - done < 32 ? out_len - done : 32;
        memcpy(out + done, t, take);
        done += take;
    }
}

/* ---------------------------------------------------------------- ChaCha20 */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define QR(a, b, c, d) a += b; d ^= a; d = ROL(d, 16); c += d; b ^= c; b = ROL(b, 12); a += b; d ^= a; d = ROL(d, 8); c += d; b ^= c; b = ROL(b, 7);

static void chacha_block(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, uint8_t out[64])
{
    uint32_t s[16], x[16];
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) {
        s[4 + i] = rd32(key + 4 * i);
    }
    s[12] = counter;
    s[13] = rd32(nonce);
    s[14] = rd32(nonce + 4);
    s[15] = rd32(nonce + 8);
    memcpy(x, s, sizeof(s));
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]) QR(x[1], x[5], x[9], x[13]) QR(x[2], x[6], x[10], x[14]) QR(x[3], x[7], x[11], x[15])
        QR(x[0], x[5], x[10], x[15]) QR(x[1], x[6], x[11], x[12]) QR(x[2], x[7], x[8], x[13]) QR(x[3], x[4], x[9], x[14])
    }
    for (int i = 0; i < 16; i++) {
        wr32(out + 4 * i, x[i] + s[i]);
    }
}

void purr_px_chacha20_xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, const uint8_t *in, uint8_t *out, size_t n)
{
    uint8_t ks[64];
    while (n > 0) {
        chacha_block(key, nonce, counter++, ks);
        size_t take = n < 64 ? n : 64;
        for (size_t i = 0; i < take; i++) {
            out[i] = in[i] ^ ks[i];
        }
        in += take;
        out += take;
        n -= take;
    }
}

/* ---------------------------------------------------------------- Poly1305 (17 limbs of 8 bits, as in TweetNaCl: slow, small, and no table or branch depends on the data) */

static void add1305(uint32_t *h, const uint32_t *c)
{
    uint32_t u = 0;
    for (int j = 0; j < 17; j++) {
        u += h[j] + c[j];
        h[j] = u & 255;
        u >>= 8;
    }
}

typedef struct {
    uint32_t r[17], h[17];
    uint8_t buf[16];
    int n;
} poly_t;

static void poly_block(poly_t *p, const uint8_t *m, int len, int hibit)
{
    uint32_t c[17], x[17];
    for (int j = 0; j < 17; j++) {
        c[j] = 0;
    }
    for (int j = 0; j < len; j++) {
        c[j] = m[j];
    }
    if (hibit) {
        c[len] = 1;
    }
    add1305(p->h, c);
    for (int i = 0; i < 17; i++) {
        x[i] = 0;
        for (int j = 0; j < 17; j++) {
            x[i] += p->h[j] * ((j <= i) ? p->r[i - j] : 320 * p->r[i + 17 - j]);
        }
    }
    for (int i = 0; i < 17; i++) {
        p->h[i] = x[i];
    }
    uint32_t u = 0;
    for (int j = 0; j < 16; j++) {
        u += p->h[j];
        p->h[j] = u & 255;
        u >>= 8;
    }
    u += p->h[16];
    p->h[16] = u & 3;
    u = 5 * (u >> 2);
    for (int j = 0; j < 16; j++) {
        u += p->h[j];
        p->h[j] = u & 255;
        u >>= 8;
    }
    u += p->h[16];
    p->h[16] = u;
}

static void poly_init(poly_t *p, const uint8_t key[32])
{
    memset(p, 0, sizeof(*p));
    for (int j = 0; j < 16; j++) {
        p->r[j] = key[j];
    }
    p->r[3] &= 15; p->r[4] &= 252; p->r[7] &= 15; p->r[8] &= 252; p->r[11] &= 15; p->r[12] &= 252; p->r[15] &= 15;
}

static void poly_update(poly_t *p, const uint8_t *m, size_t n)
{
    while (n > 0) {
        size_t take = 16 - (size_t)p->n < n ? 16 - (size_t)p->n : n;
        memcpy(p->buf + p->n, m, take);
        p->n += (int)take;
        m += take;
        n -= take;
        if (p->n == 16) {
            poly_block(p, p->buf, 16, 1);
            p->n = 0;
        }
    }
}

static void poly_final(poly_t *p, const uint8_t key[32], uint8_t tag[16])
{
    static const uint32_t minusp[17] = {5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 252};
    if (p->n > 0) {
        poly_block(p, p->buf, p->n, 1);
    }
    uint32_t g[17], c[17];
    for (int j = 0; j < 17; j++) {
        g[j] = p->h[j];
    }
    add1305(p->h, minusp);
    uint32_t s = (uint32_t)(-(int32_t)(p->h[16] >> 7));
    for (int j = 0; j < 17; j++) {
        p->h[j] ^= s & (g[j] ^ p->h[j]);
    }
    for (int j = 0; j < 16; j++) {
        c[j] = key[j + 16];
    }
    c[16] = 0;
    add1305(p->h, c);
    for (int j = 0; j < 16; j++) {
        tag[j] = (uint8_t)p->h[j];
    }
}

void purr_px_poly1305(const uint8_t key[32], const uint8_t *msg, size_t n, uint8_t tag[16])
{
    poly_t p;
    poly_init(&p, key);
    poly_update(&p, msg, n);
    poly_final(&p, key, tag);
}

/* ---------------------------------------------------------------- the AEAD (RFC 8439 section 2.8) */

static void mac_aead(const uint8_t otk[32], const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t ct_len, uint8_t tag[16])
{
    static const uint8_t zeros[16] = {0};
    poly_t p;
    poly_init(&p, otk);
    poly_update(&p, aad, aad_len);
    poly_update(&p, zeros, (16 - aad_len % 16) % 16);
    poly_update(&p, ct, ct_len);
    poly_update(&p, zeros, (16 - ct_len % 16) % 16);
    uint8_t lens[16];
    for (int i = 0; i < 8; i++) {
        lens[i] = (uint8_t)((uint64_t)aad_len >> (8 * i));
        lens[8 + i] = (uint8_t)((uint64_t)ct_len >> (8 * i));
    }
    poly_update(&p, lens, 16);
    poly_final(&p, otk, tag);
}

void purr_px_aead_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t pt_len, uint8_t *out)
{
    uint8_t block[64];
    chacha_block(key, nonce, 0, block);                          /* the first 32 bytes of block 0 are the one-time Poly1305 key */
    purr_px_chacha20_xor(key, nonce, 1, pt, out, pt_len);
    mac_aead(block, aad, aad_len, out, pt_len, out + pt_len);
}

int purr_px_aead_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t ct_len, uint8_t *pt)
{
    if (ct_len < PURR_PX_AEAD_TAG) {
        return -1;
    }
    size_t n = ct_len - PURR_PX_AEAD_TAG;
    uint8_t block[64], tag[16];
    chacha_block(key, nonce, 0, block);
    mac_aead(block, aad, aad_len, ct, n, tag);
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) {
        diff |= (uint8_t)(tag[i] ^ ct[n + i]);
    }
    if (diff != 0) {
        return -1;
    }
    purr_px_chacha20_xor(key, nonce, 1, ct, pt, n);
    return 0;
}
