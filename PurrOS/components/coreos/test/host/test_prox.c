/* The proximity core (purr_prox.h): frames, the beacon, the table of nearby devices and the "brought close" gesture, and messages in pieces. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "purr_prox.h"
#include "testkit.h"

static uint32_t s_rng = 777;
static uint32_t rnd(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng >> 8;
}

static void id_of(uint8_t id[8], int n)
{
    memset(id, 0, 8);
    id[0] = (uint8_t)n;
    id[7] = (uint8_t)(n * 7);
}

static void test_header_and_beacon(void)
{
    uint8_t b[64];
    purr_px_hdr_t h = {PURR_PX_T_BEACON, 3, {1, 2, 3, 4, 5, 6, 7, 8}}, g;
    CHECK_EQ(purr_px_hdr_put(&h, b, sizeof(b)), PURR_PX_HDR);
    CHECK_EQ(b[0], PURR_PX_MAGIC);
    CHECK_EQ(purr_px_hdr_get(b, PURR_PX_HDR, &g), PURR_PX_HDR);
    CHECK(g.type == PURR_PX_T_BEACON && g.flags == 3 && memcmp(g.id, h.id, 8) == 0);
    CHECK_EQ(purr_px_hdr_get(b, PURR_PX_HDR - 1, &g), -1);
    CHECK_EQ(purr_px_hdr_put(&h, b, PURR_PX_HDR - 1), -1);
    b[0] ^= 0xFF;
    CHECK_EQ(purr_px_hdr_get(b, PURR_PX_HDR, &g), -2);            /* not ours */
    b[0] ^= 0xFF;
    b[1] = 9;
    CHECK_EQ(purr_px_hdr_get(b, PURR_PX_HDR, &g), -3);            /* a version we do not speak */

    purr_px_beacon_t bc, gc;
    memset(&bc, 0, sizeof(bc));
    bc.roles = PURR_PX_ROLE_SCREEN | PURR_PX_ROLE_PAIRING | PURR_PX_ROLE_SETUP;
    bc.klass = PURR_PX_CLASS_TDECK;
    bc.caps = PURR_PX_CAP_DISPLAY_HOST | PURR_PX_CAP_SHARE;
    bc.counter = 0xDEADBEEF;
    memcpy(bc.pair_hint, "\x01\x02\x03\x04", 4);
    strcpy(bc.name, "T-Deck Plus");
    CHECK_EQ(purr_px_beacon_put(&bc, b, sizeof(b)), PURR_PX_BEACON_LEN);
    CHECK_EQ(PURR_PX_BEACON_LEN, 28);
    CHECK_EQ(purr_px_beacon_get(b, PURR_PX_BEACON_LEN, &gc), PURR_PX_BEACON_LEN);
    CHECK((gc.roles & PURR_PX_ROLE_SETUP) != 0);
    CHECK(gc.roles == bc.roles && gc.klass == bc.klass && gc.caps == bc.caps && gc.counter == 0xDEADBEEF && strcmp(gc.name, "T-Deck Plus") == 0 && memcmp(gc.pair_hint, bc.pair_hint, 4) == 0);
    memset(b + 12, 'Z', 16);                                      /* a name with no end off the air is still a string */
    CHECK_EQ(purr_px_beacon_get(b, PURR_PX_BEACON_LEN, &gc), PURR_PX_BEACON_LEN);
    CHECK_EQ((int)strlen(gc.name), 15);
    CHECK_EQ(purr_px_beacon_get(b, PURR_PX_BEACON_LEN - 1, &gc), -1);
    CHECK(PURR_PX_HDR + PURR_PX_BEACON_LEN <= PURR_PX_MAX_FRAME);
}

static void test_table(void)
{
    purr_px_table_t t;
    purr_px_table_init(&t);
    uint8_t a[8], b[8];
    id_of(a, 1);
    id_of(b, 2);
    CHECK_EQ(purr_px_table_count(&t), 0);
    purr_px_beacon_t bc;
    memset(&bc, 0, sizeof(bc));
    strcpy(bc.name, "one");
    purr_px_peer_t *p = purr_px_table_seen(&t, a, -60, 1000, &bc);
    CHECK(p != NULL && p->rssi == -60 && strcmp(p->beacon.name, "one") == 0);
    CHECK_EQ(purr_px_table_count(&t), 1);
    p = purr_px_table_seen(&t, a, -40, 1100, NULL);                /* smoothed: three parts old, one part new */
    CHECK_EQ(p->rssi, -55);
    CHECK(strcmp(p->beacon.name, "one") == 0);                    /* a frame with no beacon leaves the description alone */
    purr_px_table_seen(&t, b, -70, 1200, NULL);
    CHECK_EQ(purr_px_table_count(&t), 2);
    CHECK(purr_px_table_find(&t, a) != NULL && purr_px_table_find(&t, b) != NULL);
    uint8_t c[8];
    id_of(c, 3);
    CHECK(purr_px_table_find(&t, c) == NULL);
    CHECK_EQ(purr_px_table_prune(&t, 5000, 3850), 1);              /* a was last heard at 1100: 3900 ms ago; b at 1200: 3800 ms ago */
    CHECK(purr_px_table_find(&t, a) == NULL && purr_px_table_find(&t, b) != NULL);
    CHECK_EQ(purr_px_table_prune(&t, 5000, 3850), 0);

    purr_px_table_init(&t);                                        /* more devices than room: the one heard longest ago makes room */
    for (int i = 0; i < PURR_PX_PEERS; i++) {
        uint8_t x[8];
        id_of(x, i + 10);
        purr_px_table_seen(&t, x, -70, 1000 + (uint32_t)i * 10, NULL);
    }
    CHECK_EQ(purr_px_table_count(&t), PURR_PX_PEERS);
    uint8_t newest[8], oldest[8];
    id_of(newest, 99);
    id_of(oldest, 10);
    purr_px_table_seen(&t, newest, -50, 2000, NULL);
    CHECK_EQ(purr_px_table_count(&t), PURR_PX_PEERS);
    CHECK(purr_px_table_find(&t, newest) != NULL && purr_px_table_find(&t, oldest) == NULL);

    purr_px_table_init(&t);                                        /* time wraps at 2^32 */
    purr_px_table_seen(&t, a, -60, 0xFFFFFF00u, NULL);
    CHECK_EQ(purr_px_table_prune(&t, 0x00000200u, 1000), 0);       /* 0x300 ms later */
    CHECK_EQ(purr_px_table_prune(&t, 0x00001000u, 1000), 1);
    purr_px_table_seen(&t, a, 500, 0, NULL);                       /* nonsense signal values are clamped */
    CHECK(purr_px_table_find(&t, a)->rssi <= 0);
}

static void test_close(void)
{
    purr_px_table_t t;
    purr_px_table_init(&t);
    purr_px_table_set_close(&t, -50, 600);
    uint8_t a[8], b[8];
    id_of(a, 1);
    id_of(b, 2);
    purr_px_table_seen(&t, a, -70, 1000, NULL);                    /* far */
    CHECK(!purr_px_peer_is_close(&t, a, 1000));
    for (uint32_t ms = 1100; ms <= 2000; ms += 100) {              /* a beacon every 100 ms, the signal rising to -40 */
        purr_px_table_seen(&t, a, -40, ms, NULL);
    }
    CHECK(purr_px_peer_is_close(&t, a, 2000));                     /* held long enough */
    CHECK(!purr_px_peer_is_close(&t, a, 5000));                    /* and then it went quiet: not close any more */
    CHECK(purr_px_table_closest(&t, 2000) != NULL);
    CHECK(purr_px_table_closest(&t, 5000) == NULL);

    purr_px_table_init(&t);
    purr_px_table_set_close(&t, -50, 600);
    purr_px_table_seen(&t, b, -40, 1000, NULL);
    purr_px_table_seen(&t, b, -40, 1100, NULL);
    CHECK(!purr_px_peer_is_close(&t, b, 1200));                    /* a touch is not a hold: 200 ms of 600 */
    for (uint32_t ms = 1200; ms <= 1800; ms += 100) {
        purr_px_table_seen(&t, b, -40, ms, NULL);
    }
    CHECK(purr_px_peer_is_close(&t, b, 1800));
    for (int i = 0; i < 6; i++) {                                  /* it moves away: the hold is lost and starts again from nothing */
        purr_px_table_seen(&t, b, -80, 1900 + (uint32_t)i * 100, NULL);
    }
    CHECK(!purr_px_peer_is_close(&t, b, 2500));
    purr_px_table_seen(&t, b, -30, 2600, NULL);
    CHECK(!purr_px_peer_is_close(&t, b, 2600));

    purr_px_table_init(&t);                                        /* the closest of several */
    purr_px_table_set_close(&t, -55, 300);
    for (uint32_t ms = 1000; ms <= 1500; ms += 100) {
        purr_px_table_seen(&t, a, -50, ms, NULL);
        purr_px_table_seen(&t, b, -35, ms, NULL);
    }
    const purr_px_peer_t *best = purr_px_table_closest(&t, 1500);
    CHECK(best != NULL && memcmp(best->id, b, 8) == 0);
    purr_px_table_set_close(&t, -20, 300);                         /* a stricter threshold forgets the holds */
    CHECK(purr_px_table_closest(&t, 1500) == NULL);
}

static void test_fragments(void)
{
    static uint8_t msg[PURR_PX_MSG_MAX], out[PURR_PX_MSG_MAX];
    static purr_px_reasm_t r;
    for (int i = 0; i < PURR_PX_MSG_MAX; i++) {
        msg[i] = (uint8_t)(i * 31 + 7);
    }
    CHECK_EQ(PURR_PX_FRAG_PAYLOAD, 234);
    CHECK_EQ(purr_px_frag_count(0), 1);
    CHECK_EQ(purr_px_frag_count(1), 1);
    CHECK_EQ(purr_px_frag_count(234), 1);
    CHECK_EQ(purr_px_frag_count(235), 2);
    CHECK_EQ(purr_px_frag_count(PURR_PX_MSG_MAX), PURR_PX_FRAG_MAX);
    CHECK_EQ(purr_px_frag_count(PURR_PX_MSG_MAX + 1), -1);
    CHECK_EQ(purr_px_frag_count(-1), -1);

    static const int lens[] = {0, 1, 233, 234, 235, 468, 469, 1000, 4095, 4096};
    for (size_t li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
        int len = lens[li], n = purr_px_frag_count(len);
        static uint8_t pieces[PURR_PX_FRAG_MAX][PURR_PX_MAX_PAYLOAD];
        int plen[PURR_PX_FRAG_MAX];
        for (int i = 0; i < n; i++) {
            plen[i] = purr_px_frag_put(0x1234, msg, len, i, pieces[i], sizeof(pieces[i]));
            CHECK(plen[i] > 0 && plen[i] <= PURR_PX_MAX_PAYLOAD);
        }
        for (int order = 0; order < 2; order++) {                  /* in order, then shuffled */
            int idx[PURR_PX_FRAG_MAX];
            for (int i = 0; i < n; i++) {
                idx[i] = i;
            }
            if (order == 1) {
                for (int i = n - 1; i > 0; i--) {
                    int j = (int)(rnd() % (unsigned)(i + 1)), tmp = idx[i];
                    idx[i] = idx[j];
                    idx[j] = tmp;
                }
            }
            purr_px_reasm_init(&r);
            int done = 0;
            for (int i = 0; i < n; i++) {
                int rc = purr_px_reasm_push(&r, 100, pieces[idx[i]], plen[idx[i]]);
                CHECK(rc == (i == n - 1 ? 1 : 0));
                done = rc == 1;
                if (i + 1 < n) {
                    CHECK_EQ(purr_px_reasm_push(&r, 100, pieces[idx[i]], plen[idx[i]]), 0);     /* a repeat changes nothing */
                }
            }
            CHECK(done);
            memcpy(out, r.buf, (size_t)r.len);
            CHECK_EQ(r.len, len);
            CHECK(memcmp(out, msg, (size_t)len) == 0);
        }
    }
    CHECK_EQ(purr_px_frag_put(1, msg, 10, 1, out, 100), -1);       /* no such piece */
    CHECK_EQ(purr_px_frag_put(1, msg, 10, 0, out, 5), -1);         /* it does not fit */
    CHECK_EQ(purr_px_frag_put(1, msg, PURR_PX_MSG_MAX + 1, 0, out, 300), -1);
}

static void test_fragment_trouble(void)
{
    static uint8_t msg[1000], p0[PURR_PX_MAX_PAYLOAD], p1[PURR_PX_MAX_PAYLOAD], q0[PURR_PX_MAX_PAYLOAD];
    static purr_px_reasm_t r;
    memset(msg, 0xAB, sizeof(msg));
    int n0 = purr_px_frag_put(7, msg, 1000, 0, p0, sizeof(p0));
    int n1 = purr_px_frag_put(7, msg, 1000, 1, p1, sizeof(p1));
    int nq = purr_px_frag_put(8, msg, 1000, 0, q0, sizeof(q0));
    purr_px_reasm_init(&r);
    CHECK_EQ(purr_px_reasm_push(&r, 0, p0, n0), 0);
    CHECK_EQ(purr_px_reasm_push(&r, 10, q0, nq), 0);               /* another message arrives: the newer one wins */
    CHECK_EQ(purr_px_reasm_push(&r, 20, p1, n1), 0);               /* a piece of the dropped one starts it over: not complete */
    CHECK_EQ(purr_px_reasm_expire(&r, 100, 500), 0);
    CHECK_EQ(purr_px_reasm_expire(&r, 1000, 500), 1);              /* an unfinished message is forgotten after a while */
    CHECK_EQ(purr_px_reasm_expire(&r, 2000, 500), 0);

    uint8_t bad[PURR_PX_MAX_PAYLOAD];
    memcpy(bad, p0, (size_t)n0);
    bad[3] = 0;                                                    /* a count of zero */
    CHECK_EQ(purr_px_reasm_push(&r, 0, bad, n0), -1);
    memcpy(bad, p0, (size_t)n0);
    bad[3] = PURR_PX_FRAG_MAX + 1;
    CHECK_EQ(purr_px_reasm_push(&r, 0, bad, n0), -1);
    memcpy(bad, p0, (size_t)n0);
    bad[2] = 5;                                                    /* an index past the count */
    CHECK_EQ(purr_px_reasm_push(&r, 0, bad, n0), -1);
    memcpy(bad, p0, (size_t)n0);
    CHECK_EQ(purr_px_reasm_push(&r, 0, bad, n0 - 1), -1);          /* a piece that is not the last but is short */
    CHECK_EQ(purr_px_reasm_push(&r, 0, p0, 3), -1);                /* shorter than its own header */
    CHECK_EQ(purr_px_reasm_push(&r, 0, NULL, 10), -1);
    uint8_t big[PURR_PX_MAX_PAYLOAD + 40];
    memset(big, 0, sizeof(big));
    big[3] = 2;
    CHECK_EQ(purr_px_reasm_push(&r, 0, big, (int)sizeof(big)), -1);   /* more than a piece can carry */

    for (int trial = 0; trial < 20000; trial++) {                  /* random pieces never write outside the buffer or report a length over the limit */
        uint8_t junk[PURR_PX_MAX_PAYLOAD];
        int n = (int)(rnd() % sizeof(junk));
        for (int i = 0; i < n; i++) {
            junk[i] = (uint8_t)rnd();
        }
        if (n >= 4 && rnd() % 2) {
            junk[3] = (uint8_t)(1 + rnd() % PURR_PX_FRAG_MAX);
            junk[2] = (uint8_t)(rnd() % junk[3]);
        }
        int rc = purr_px_reasm_push(&r, (uint32_t)trial, junk, n);
        if (rc == 1) {
            CHECK(r.len <= PURR_PX_MSG_MAX);
        }
    }
    CHECK(1);
}

int main(void)
{
    test_header_and_beacon();
    test_table();
    test_close();
    test_fragments();
    test_fragment_trouble();
    TK_DONE("test_prox");
}
