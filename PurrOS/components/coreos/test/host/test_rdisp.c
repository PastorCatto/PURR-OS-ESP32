/* The remote display wire code (purr_rdisp.h, RemoteDisplay/SPEC.md): messages, the three codecs, splitting, damage merging. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "purr_rdisp.h"
#include "testkit.h"

static uint32_t s_rng = 12345;
static uint32_t rnd(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng >> 8;
}

/* ---------------------------------------------------------------- messages */

static void test_header(void)
{
    uint8_t b[16];
    purr_rd_hdr_t h = {PURR_RD_T_RECT, 0, 0x1234, 777}, g;
    CHECK_EQ(purr_rd_hdr_put(&h, b, sizeof(b)), 8);
    CHECK_EQ(b[0], PURR_RD_T_RECT);
    CHECK_EQ(b[2], 0x34);                                         /* little-endian */
    CHECK_EQ(b[3], 0x12);
    CHECK_EQ(purr_rd_hdr_get(b, 8, &g), 8);
    CHECK(g.type == h.type && g.seq == h.seq && g.len == h.len && g.flags == 0);
    CHECK_EQ(purr_rd_hdr_get(b, 7, &g), -1);                      /* short */
    CHECK_EQ(purr_rd_hdr_put(&h, b, 7), -1);
    h.len = PURR_RD_MAX_PAYLOAD;
    CHECK_EQ(purr_rd_hdr_put(&h, b, 8), 8);                       /* the largest allowed */
    h.len = PURR_RD_MAX_PAYLOAD + 1;
    CHECK_EQ(purr_rd_hdr_put(&h, b, 8), -1);                      /* over: refused */
    b[4] = 0x01;
    b[5] = 0x00;
    b[6] = 0x01;
    b[7] = 0x00;                                                  /* 65537 on the wire */
    CHECK_EQ(purr_rd_hdr_get(b, 8, &g), -2);
    b[4] = b[5] = b[6] = b[7] = 0xFF;                             /* 4 GB: ends the session */
    CHECK_EQ(purr_rd_hdr_get(b, 8, &g), -2);
}

static void test_messages(void)
{
    uint8_t b[128];
    purr_rd_rect_msg_t rm = {{10, 20, 300, 40}, PURR_RD_CODEC_LZ4, PURR_RD_FMT_RGB565}, rg;
    CHECK_EQ(purr_rd_rect_put(&rm, b, sizeof(b)), PURR_RD_RECT_HEAD);
    CHECK_EQ(purr_rd_rect_get(b, PURR_RD_RECT_HEAD, &rg), PURR_RD_RECT_HEAD);
    CHECK(rg.r.x == 10 && rg.r.y == 20 && rg.r.w == 300 && rg.r.h == 40 && rg.codec == PURR_RD_CODEC_LZ4 && rg.fmt == PURR_RD_FMT_RGB565);
    CHECK_EQ(purr_rd_rect_get(b, PURR_RD_RECT_HEAD - 1, &rg), -1);

    purr_rd_fill_t fm = {{1, 2, 3, 4}, 0xF81F}, fg;
    CHECK_EQ(purr_rd_fill_put(&fm, b, sizeof(b)), PURR_RD_FILL_LEN);
    CHECK_EQ(purr_rd_fill_get(b, PURR_RD_FILL_LEN, &fg), PURR_RD_FILL_LEN);
    CHECK(fg.r.w == 3 && fg.colour == 0xF81F);

    purr_rd_copy_t cm = {1, 2, 3, 4, 5, 6}, cg;
    CHECK_EQ(purr_rd_copy_put(&cm, b, sizeof(b)), PURR_RD_COPY_LEN);
    CHECK_EQ(purr_rd_copy_get(b, PURR_RD_COPY_LEN, &cg), PURR_RD_COPY_LEN);
    CHECK(cg.sx == 1 && cg.dy == 6 && cg.w == 3);

    purr_rd_input_t im = {PURR_RD_IN_TOUCH_MOVE, 1, 319, 239}, ig;
    CHECK_EQ(purr_rd_input_put(&im, b, sizeof(b)), PURR_RD_INPUT_LEN);
    CHECK_EQ(purr_rd_input_get(b, PURR_RD_INPUT_LEN, &ig), PURR_RD_INPUT_LEN);
    CHECK(ig.kind == PURR_RD_IN_TOUCH_MOVE && ig.id == 1 && ig.x == 319 && ig.y == 239);

    purr_rd_hello_t hm, hg;
    memset(&hm, 0, sizeof(hm));
    hm.major = PURR_RD_MAJOR;
    hm.minor = PURR_RD_MINOR;
    hm.width = 320;
    hm.height = 240;
    hm.formats = 1;
    hm.codecs = 7;
    hm.caps = PURR_RD_CAP_FILL | PURR_RD_CAP_TOUCH;
    hm.max_px = 4096;
    hm.rx_buf = 16384;
    strcpy(hm.board, "cyd_24c");
    for (int i = 0; i < 64; i++) {
        hm.pubkey[i] = (uint8_t)(i * 3);
    }
    CHECK_EQ(purr_rd_hello_put(&hm, b, sizeof(b)), PURR_RD_HELLO_LEN);
    CHECK_EQ(PURR_RD_HELLO_LEN, 94);
    CHECK_EQ(purr_rd_hello_get(b, PURR_RD_HELLO_LEN, &hg), PURR_RD_HELLO_LEN);
    CHECK(hg.width == 320 && hg.height == 240 && hg.max_px == 4096 && hg.rx_buf == 16384 && strcmp(hg.board, "cyd_24c") == 0 && memcmp(hg.pubkey, hm.pubkey, 64) == 0);
    memset(b + 14, 'A', 16);                                     /* a name with no terminator on the wire is still a string here */
    CHECK_EQ(purr_rd_hello_get(b, PURR_RD_HELLO_LEN, &hg), PURR_RD_HELLO_LEN);
    CHECK_EQ((int)strlen(hg.board), 15);
    CHECK_EQ(purr_rd_hello_get(b, PURR_RD_HELLO_LEN - 1, &hg), -1);

    purr_rd_welcome_t wm = {1, 0, PURR_RD_FMT_RGB565, 7, 320, 240, 4096, 3, 0, 0xCAFEBABE}, wg;
    CHECK_EQ(purr_rd_welcome_put(&wm, b, sizeof(b)), PURR_RD_WELCOME_LEN);
    CHECK_EQ(purr_rd_welcome_get(b, PURR_RD_WELCOME_LEN, &wg), PURR_RD_WELCOME_LEN);
    CHECK(wg.session == 0xCAFEBABE && wg.window == 3 && wg.width == 320 && wg.max_px == 4096);
}

/* ---------------------------------------------------------------- codecs */

static void test_raw_bytes(void)
{
    CHECK_EQ(purr_rd_raw_bytes(PURR_RD_FMT_RGB565, 320, 240), 153600);
    CHECK_EQ(purr_rd_raw_bytes(PURR_RD_FMT_MONO1, 128, 64), 1024);
    CHECK_EQ(purr_rd_raw_bytes(PURR_RD_FMT_MONO1, 9, 2), 4);      /* rows of two bytes */
    CHECK_EQ(purr_rd_raw_bytes(7, 1, 1), -1);
    CHECK_EQ(purr_rd_raw_bytes(PURR_RD_FMT_RGB565, -1, 1), -1);
}

/* what a UI looks like: flat areas, a row of text-like noise, a gradient strip */
static void ui_like(uint8_t *p, int n_px)
{
    uint16_t bg = 0xFFFF, ink = 0x18C3;
    for (int i = 0; i < n_px; i++) {
        uint16_t v = bg;
        if ((i / 160) % 3 == 0 && rnd() % 5 < 2) {
            v = ink;
        } else if ((i / 400) % 4 == 3) {
            v = (uint16_t)(0x001F + (i % 32) * 0x0841);
        }
        p[2 * i] = (uint8_t)v;
        p[2 * i + 1] = (uint8_t)(v >> 8);
    }
}

static void test_known_lz4_vector(void)
{
    /* hand-made block: two literals "ab", a match of 6 at offset 2, then the literals "cdefg" */
    const uint8_t blk[] = {0x22, 'a', 'b', 0x02, 0x00, 0x50, 'c', 'd', 'e', 'f', 'g'};
    uint8_t out[32];
    CHECK_EQ(purr_rd_lz4_decode(blk, sizeof(blk), out, 13), 0);
    CHECK(memcmp(out, "abababab" "cdefg", 13) == 0);
    CHECK_EQ(purr_rd_lz4_decode(blk, sizeof(blk), out, 12), -1);  /* wrong expected length */
    CHECK_EQ(purr_rd_lz4_decode(blk, sizeof(blk), out, 14), -1);
    const uint8_t bad_off[] = {0x12, 'a', 0x05, 0x00, 0x50, 'c', 'd', 'e', 'f', 'g'};      /* offset reaches before the start */
    CHECK_EQ(purr_rd_lz4_decode(bad_off, sizeof(bad_off), out, 32), -1);
    const uint8_t zero_off[] = {0x12, 'a', 0x00, 0x00, 0x50, 'c', 'd', 'e', 'f', 'g'};
    CHECK_EQ(purr_rd_lz4_decode(zero_off, sizeof(zero_off), out, 32), -1);
    /* a literal of 15 + 255 + 3: the extension bytes are added */
    uint8_t big[1 + 2 + 273];
    big[0] = 0xF0;
    big[1] = 255;
    big[2] = 3;
    memset(big + 3, 'x', 273);
    uint8_t out2[273];
    CHECK_EQ(purr_rd_lz4_decode(big, sizeof(big), out2, 273), 0);
    CHECK(out2[0] == 'x' && out2[272] == 'x');
}

static uint16_t s_table[PURR_RD_LZ4_TABLE];

static void roundtrip_all(const uint8_t *src, int n)
{
    static uint8_t enc[70000], dec[70000];
    int e = purr_rd_lz4_encode(src, n, enc, (int)sizeof(enc), s_table);
    CHECK(e > 0);
    if (e > 0) {
        memset(dec, 0xAA, sizeof(dec));
        CHECK_EQ(purr_rd_lz4_decode(enc, e, dec, n), 0);
        CHECK(memcmp(src, dec, (size_t)n) == 0);
        CHECK(dec[n] == 0xAA);                                    /* nothing written past the end */
    }
    if ((n & 1) == 0) {
        e = purr_rd_rle16_encode(src, n, enc, (int)sizeof(enc));
        CHECK(e >= 0 || n == 0);
        if (e >= 0) {
            memset(dec, 0xAA, sizeof(dec));
            CHECK_EQ(purr_rd_rle16_decode(enc, e, dec, n), 0);
            CHECK(memcmp(src, dec, (size_t)n) == 0);
            CHECK(dec[n] == 0xAA);
        }
    }
}

static void test_roundtrips(void)
{
    static uint8_t src[70000];
    for (int n = 0; n <= 40; n++) {                               /* every small size, with runs and with noise */
        for (int kind = 0; kind < 3; kind++) {
            for (int i = 0; i < n; i++) {
                src[i] = kind == 0 ? 0x55 : (kind == 1 ? (uint8_t)(i / 3) : (uint8_t)rnd());
            }
            roundtrip_all(src, n);
        }
    }
    memset(src, 0, 8192);
    roundtrip_all(src, 8192);                                     /* one long run */
    for (int i = 0; i < 8192; i++) {
        src[i] = (uint8_t)rnd();
    }
    roundtrip_all(src, 8192);                                     /* incompressible: still has to round trip */
    ui_like(src, 4096);
    roundtrip_all(src, 8192);
    ui_like(src, 32767);
    roundtrip_all(src, 65534);                                    /* the largest */
    int period = 7;                                               /* a repeating pattern: matches that overlap their own output */
    for (int i = 0; i < 20000; i++) {
        src[i] = (uint8_t)((i % period) * 37);
    }
    roundtrip_all(src, 20000);
}

static void test_compression_is_worth_it(void)
{
    static uint8_t src[4096 * 2], out[4096 * 2];
    uint8_t codec;
    memset(src, 0xFF, sizeof(src));                               /* a flat area */
    int n = purr_rd_encode_best(src, sizeof(src), PURR_RD_FMT_RGB565, 7, out, sizeof(out), s_table, &codec);
    CHECK(n > 0 && n < 200);
    CHECK(codec == PURR_RD_CODEC_RLE16 || codec == PURR_RD_CODEC_LZ4);
    ui_like(src, 4096);
    n = purr_rd_encode_best(src, sizeof(src), PURR_RD_FMT_RGB565, 7, out, sizeof(out), s_table, &codec);
    CHECK(n > 0 && n < (int)sizeof(src) / 2);                     /* a UI piece comes out under half */
    static uint8_t dec[4096 * 2];
    CHECK_EQ(purr_rd_decode(codec, out, n, dec, sizeof(dec)), 0);
    CHECK(memcmp(src, dec, sizeof(src)) == 0);
    for (size_t i = 0; i < sizeof(src); i++) {
        src[i] = (uint8_t)rnd();
    }
    n = purr_rd_encode_best(src, sizeof(src), PURR_RD_FMT_RGB565, 7, out, sizeof(out), s_table, &codec);
    CHECK_EQ(n, (int)sizeof(src));                                /* noise: sent raw, never larger than raw */
    CHECK_EQ(codec, PURR_RD_CODEC_RAW);
    CHECK_EQ(purr_rd_decode(PURR_RD_CODEC_RAW, out, n, dec, sizeof(dec)), 0);
    CHECK(memcmp(src, dec, sizeof(src)) == 0);
    CHECK_EQ(purr_rd_encode_best(src, sizeof(src), PURR_RD_FMT_RGB565, 7, out, 100, s_table, &codec), -1);     /* not even raw fits */
    memset(src, 0xFF, sizeof(src));
    n = purr_rd_encode_best(src, sizeof(src), PURR_RD_FMT_RGB565, PURR_RD_CODEC_BIT(PURR_RD_CODEC_RAW), out, sizeof(out), NULL, &codec);
    CHECK(n == (int)sizeof(src) && codec == PURR_RD_CODEC_RAW);   /* a client that only does RAW is only sent RAW */
    n = purr_rd_encode_best(src, 1024, PURR_RD_FMT_MONO1, 7, out, sizeof(out), s_table, &codec);
    CHECK(n > 0 && codec != PURR_RD_CODEC_RLE16);                 /* RLE16 is for RGB565 only */
    CHECK_EQ(purr_rd_decode(9, out, n, dec, 1024), -1);           /* an unknown codec */
}

/* a decoder fed garbage or a damaged stream never writes outside its buffer and never reads past its input */
static void test_hostile_input(void)
{
    enum { N = 2048 };
    static uint8_t src[N], enc[4096];
    uint8_t guard[N + 64];
    ui_like(src, N / 2);
    for (int codec = 1; codec <= 2; codec++) {
        int e = codec == 1 ? purr_rd_rle16_encode(src, N, enc, sizeof(enc)) : purr_rd_lz4_encode(src, N, enc, sizeof(enc), s_table);
        CHECK(e > 0);
        for (int trial = 0; trial < 3000; trial++) {
            uint8_t mut[4096];
            memcpy(mut, enc, (size_t)e);
            int len = e;
            int flips = 1 + (int)(rnd() % 4);
            for (int f = 0; f < flips; f++) {
                mut[rnd() % (unsigned)e] = (uint8_t)rnd();
            }
            if (rnd() % 3 == 0) {
                len = (int)(rnd() % (unsigned)(e + 1));           /* truncated */
            }
            memset(guard, 0xCD, sizeof(guard));
            int rc = purr_rd_decode(codec, mut, len, guard + 16, N);
            (void)rc;
            int clean = 1;
            for (int i = 0; i < 16; i++) {
                clean &= guard[i] == 0xCD;
            }
            for (int i = 16 + N; i < N + 64; i++) {
                clean &= guard[i] == 0xCD;
            }
            if (!clean) {
                CHECK(clean);
                return;
            }
        }
    }
    for (int trial = 0; trial < 3000; trial++) {                  /* pure noise as input */
        uint8_t noise[300];
        int len = (int)(rnd() % sizeof(noise));
        for (int i = 0; i < len; i++) {
            noise[i] = (uint8_t)rnd();
        }
        for (int codec = 1; codec <= 2; codec++) {
            memset(guard, 0xCD, sizeof(guard));
            purr_rd_decode(codec, noise, len, guard + 16, 256);
            int clean = 1;
            for (int i = 0; i < 16; i++) {
                clean &= guard[i] == 0xCD;
            }
            for (int i = 16 + 256; i < 16 + 256 + 32; i++) {
                clean &= guard[i] == 0xCD;
            }
            if (!clean) {
                CHECK(clean);
                return;
            }
        }
    }
    CHECK(1);
}

/* ---------------------------------------------------------------- rectangles */

static void test_split(void)
{
    purr_rd_rect_t out[400];
    for (int trial = 0; trial < 500; trial++) {
        purr_rd_rect_t in = {(uint16_t)(rnd() % 100), (uint16_t)(rnd() % 100), (uint16_t)(1 + rnd() % 330), (uint16_t)(1 + rnd() % 250)};
        int max_px = 1 + (int)(rnd() % 6000);
        int n = purr_rd_split(in, max_px, out, 400);
        if (n < 0) {
            CHECK(n == -1);
            continue;
        }
        long total = 0;
        int ok = 1;
        for (int i = 0; i < n; i++) {
            ok &= (long)out[i].w * out[i].h <= max_px;
            ok &= out[i].x >= in.x && out[i].y >= in.y && out[i].x + out[i].w <= in.x + in.w && out[i].y + out[i].h <= in.y + in.h;
            total += (long)out[i].w * out[i].h;
            for (int j = 0; j < i; j++) {                         /* no overlap */
                ok &= !(out[i].x < out[j].x + out[j].w && out[j].x < out[i].x + out[i].w && out[i].y < out[j].y + out[j].h && out[j].y < out[i].y + out[i].h);
            }
        }
        CHECK(ok);
        CHECK_EQ(total, (long)in.w * in.h);                       /* together, exactly the rectangle */
    }
    purr_rd_rect_t one = {5, 5, 320, 240};
    CHECK_EQ(purr_rd_split(one, 4096, out, 400), 20);             /* 4096 / 320 = 12 rows a piece: 240 rows are exactly 20 pieces */
}

static void test_damage(void)
{
    purr_rd_damage_t d;
    purr_rd_rect_t out[PURR_RD_DAMAGE_MAX + 4];
    purr_rd_damage_init(&d, 320, 240);
    purr_rd_damage_add(&d, 10, 10, 0, 5);                         /* empty */
    purr_rd_damage_add(&d, 400, 10, 5, 5);                        /* outside */
    CHECK_EQ(d.n, 0);
    purr_rd_damage_add(&d, -5, -5, 20, 20);                       /* clipped to the screen */
    CHECK_EQ(d.n, 1);
    CHECK(d.r[0].x == 0 && d.r[0].y == 0 && d.r[0].w == 15 && d.r[0].h == 15);
    purr_rd_damage_add(&d, 10, 10, 20, 20);                       /* overlaps: one rectangle */
    CHECK_EQ(d.n, 1);
    purr_rd_damage_add(&d, 200, 200, 10, 10);                     /* far away: a second */
    CHECK_EQ(d.n, 2);
    CHECK_EQ(purr_rd_damage_take(&d, out, 4), 2);
    CHECK_EQ(d.n, 0);

    /* property: however many are added, every pixel ever added is inside a result, the set is bounded, and everything is on the screen */
    for (int trial = 0; trial < 300; trial++) {
        static uint8_t covered[320 * 240];
        memset(covered, 0, sizeof(covered));
        purr_rd_damage_init(&d, 320, 240);
        int adds = 1 + (int)(rnd() % 40);
        for (int a = 0; a < adds; a++) {
            int x = (int)(rnd() % 320), y = (int)(rnd() % 240), w = 1 + (int)(rnd() % 60), h = 1 + (int)(rnd() % 40);
            purr_rd_damage_add(&d, x, y, w, h);
            for (int yy = y; yy < y + h && yy < 240; yy++) {
                for (int xx = x; xx < x + w && xx < 320; xx++) {
                    covered[yy * 320 + xx] = 1;
                }
            }
            CHECK(d.n <= PURR_RD_DAMAGE_MAX);
        }
        int n = purr_rd_damage_take(&d, out, PURR_RD_DAMAGE_MAX + 4);
        int all = 1;
        for (int i = 0; i < n; i++) {
            all &= out[i].x + out[i].w <= 320 && out[i].y + out[i].h <= 240 && out[i].w > 0 && out[i].h > 0;
        }
        for (int y = 0; y < 240 && all; y++) {
            for (int x = 0; x < 320; x++) {
                if (covered[y * 320 + x]) {
                    int in = 0;
                    for (int i = 0; i < n; i++) {
                        in |= x >= out[i].x && x < out[i].x + out[i].w && y >= out[i].y && y < out[i].y + out[i].h;
                    }
                    if (!in) {
                        all = 0;
                        break;
                    }
                }
            }
        }
        CHECK(all);
    }
}

int main(void)
{
    test_header();
    test_messages();
    test_raw_bytes();
    test_known_lz4_vector();
    test_roundtrips();
    test_compression_is_worth_it();
    test_hostile_input();
    test_split();
    test_damage();
    TK_DONE("test_rdisp");
}
