#include "purr_util.h"

#include <string.h>

/* ---------------------------------------------------------------- names (F-11) */

int purr_name_is_safe(const char *name, size_t max_len)
{
    size_t len = strnlen(name, max_len);
    if (len == 0 || len >= max_len) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

/* ---------------------------------------------------------------- CRC-32 */

uint32_t purr_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

/* --------------------------------------------------------------- SHA-256 */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(purr_sha256_t *ctx, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void purr_sha256_init(purr_sha256_t *ctx)
{
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->bitlen = 0;
    ctx->buflen = 0;
}

void purr_sha256_update(purr_sha256_t *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    ctx->bitlen += (uint64_t)len * 8u;
    while (len) {
        size_t take = 64u - ctx->buflen;
        if (take > len) {
            take = len;
        }
        memcpy(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += (uint32_t)take;
        p += take;
        len -= take;
        if (ctx->buflen == 64) {
            sha256_block(ctx, ctx->buf);
            ctx->buflen = 0;
        }
    }
}

void purr_sha256_final(purr_sha256_t *ctx, uint8_t out[32])
{
    uint64_t bitlen = ctx->bitlen;
    uint8_t pad = 0x80;

    purr_sha256_update(ctx, &pad, 1);
    pad = 0;
    while (ctx->buflen != 56) {
        purr_sha256_update(ctx, &pad, 1);
    }
    uint8_t lenbytes[8];
    for (int i = 0; i < 8; i++) {
        lenbytes[i] = (uint8_t)(bitlen >> (56 - 8 * i));
    }
    purr_sha256_update(ctx, lenbytes, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i]     = (uint8_t)(ctx->state[i] >> 24);
        out[4 * i + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[4 * i + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[4 * i + 3] = (uint8_t)(ctx->state[i]);
    }
}

void purr_sha256(const void *data, size_t len, uint8_t out[32])
{
    purr_sha256_t ctx;
    purr_sha256_init(&ctx);
    purr_sha256_update(&ctx, data, len);
    purr_sha256_final(&ctx, out);
}

/* --------------------------------------------------------------- version */

static int parse_part(const char *text, size_t len, size_t *pos, uint32_t *out)
{
    uint32_t v = 0;
    size_t digits = 0;

    while (*pos < len && text[*pos] >= '0' && text[*pos] <= '9') {
        v = v * 10u + (uint32_t)(text[*pos] - '0');
        if (v > 255u) {
            return -1;
        }
        (*pos)++;
        digits++;
    }
    if (digits == 0) {
        return -1;
    }
    *out = v;
    return 0;
}

int purr_version_parse(const char *text, size_t max_len, uint32_t *out)
{
    uint32_t part[3] = {0, 0, 0};
    size_t len = 0, pos = 0;

    if (text == NULL || out == NULL) {
        return -1;
    }
    while (len < max_len && text[len] != '\0') {
        len++;
    }
    for (int i = 0; i < 3; i++) {
        if (parse_part(text, len, &pos, &part[i]) != 0) {
            return -1;
        }
        if (pos < len && text[pos] == '.' && i < 2) {
            pos++;
        } else {
            break;
        }
    }
    if (pos < len && text[pos] != '-' && text[pos] != '+') {
        return -1;   /* trailing junk that is not a suffix */
    }
    *out = PURR_VERSION(part[0], part[1], part[2]);
    return 0;
}
