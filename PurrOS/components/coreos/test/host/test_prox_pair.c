/* Pairing, the encrypted session, the peer store and the auto-pair rule (Modules/proximity/purr_px_pair.h), with real P-256 from micro-ecc. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "purr_px_pair.h"
#include "testkit.h"
#include "uECC.h"

static uint32_t s_rng = 4242;
static uint32_t rnd(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng >> 8;
}

static int rng_fn(uint8_t *dest, unsigned size)
{
    for (unsigned i = 0; i < size; i++) {
        dest[i] = (uint8_t)rnd();
    }
    return 1;
}

static void t_random(uint8_t *out, int n) { rng_fn(out, (unsigned)n); }
static int t_keypair(uint8_t pub[64], uint8_t priv[32]) { return uECC_make_key(pub, priv, uECC_secp256r1()); }
static int t_shared(const uint8_t pub[64], const uint8_t priv[32], uint8_t out[32]) { return uECC_shared_secret(pub, priv, out, uECC_secp256r1()); }
static int t_sign(const uint8_t priv[32], const uint8_t hash[32], uint8_t sig[64]) { return uECC_sign(priv, hash, 32, sig, uECC_secp256r1()); }
static int t_verify(const uint8_t pub[64], const uint8_t hash[32], const uint8_t sig[64]) { return uECC_verify(pub, hash, 32, sig, uECC_secp256r1()); }
static const purr_px_ecc_t ECC = {t_random, t_keypair, t_shared, t_sign, t_verify};

typedef struct {
    uint8_t pub[64], priv[32], id[8];
} dev_t_;

static void make_dev(dev_t_ *d)
{
    CHECK(t_keypair(d->pub, d->priv));
    purr_px_id_from_pub(d->pub, d->id);
}

/* ---------------------------------------------------------------- the rule */

static void test_policy(void)
{
    purr_px_policy_in_t in;
    memset(&in, 0, sizeof(in));
    CHECK_EQ(purr_px_policy(&in), PURR_PX_DENY);                  /* nothing known: nothing happens */
    in.peer_close = 1;
    CHECK_EQ(purr_px_policy(&in), PURR_PX_DENY);                  /* close, but nobody here to ask */
    in.we_have_screen = 1;
    CHECK_EQ(purr_px_policy(&in), PURR_PX_ASK);                   /* close and a screen: ask, with the six digits */

    memset(&in, 0, sizeof(in));
    in.peer_known = 1;
    CHECK_EQ(purr_px_policy(&in), PURR_PX_DENY);                  /* a paired device on a network this one does not trust */
    in.network_trusted = 1;
    CHECK_EQ(purr_px_policy(&in), PURR_PX_ALLOW_KNOWN);
    in.network_trusted = 0;
    in.allow_any_network = 1;
    CHECK_EQ(purr_px_policy(&in), PURR_PX_ALLOW_KNOWN);           /* the setting lifts the limit */
    in.allow_any_network = 0;
    in.peer_close = 1;
    CHECK_EQ(purr_px_policy(&in), PURR_PX_ALLOW_KNOWN);           /* or the device is held next to this one */

    memset(&in, 0, sizeof(in));                                   /* (a) trust by signature */
    in.peer_signed_by_trusted = 1;
    CHECK_EQ(purr_px_policy(&in), PURR_PX_DENY);                  /* but only on a trusted network, any network if allowed, or close */
    in.network_trusted = 1;
    CHECK_EQ(purr_px_policy(&in), PURR_PX_AUTO_PAIR);
    in.network_trusted = 0;
    in.peer_close = 1;
    CHECK_EQ(purr_px_policy(&in), PURR_PX_AUTO_PAIR);

    memset(&in, 0, sizeof(in));                                   /* (b) the first close claim on an unowned device */
    in.we_unowned = 1;
    in.peer_close = 1;
    CHECK_EQ(purr_px_policy(&in), PURR_PX_DENY);                  /* the window is shut */
    in.claim_window_open = 1;
    CHECK_EQ(purr_px_policy(&in), PURR_PX_AUTO_PAIR);
    in.peer_close = 0;
    CHECK_EQ(purr_px_policy(&in), PURR_PX_DENY);                  /* a claim has to be close, whatever the network */
    in.network_trusted = 1;
    in.allow_any_network = 1;
    CHECK_EQ(purr_px_policy(&in), PURR_PX_DENY);
    in.we_unowned = 0;                                            /* an owned device never accepts a claim */
    in.peer_close = 1;
    in.claim_window_open = 1;
    CHECK_EQ(purr_px_policy(&in), PURR_PX_DENY);

    memset(&in, 0, sizeof(in));                                   /* the pairing window: a stranger over the network is asked only when the person opened it, and only at a screen */
    in.we_have_screen = 1;
    CHECK_EQ(purr_px_policy(&in), PURR_PX_DENY);
    in.pairing_window_open = 1;
    CHECK_EQ(purr_px_policy(&in), PURR_PX_ASK);
    in.we_have_screen = 0;
    CHECK_EQ(purr_px_policy(&in), PURR_PX_DENY);

    memset(&in, 0, sizeof(in));                                   /* (c) the owner said so, (d) developer mode on the same network */
    in.owner_sponsored = 1;
    CHECK_EQ(purr_px_policy(&in), PURR_PX_AUTO_PAIR);
    memset(&in, 0, sizeof(in));
    in.dev_mode_lan = 1;
    CHECK_EQ(purr_px_policy(&in), PURR_PX_AUTO_PAIR);

    /* every combination of the inputs: an owned device with no signature, no sponsor and no developer mode never auto-pairs an unknown peer */
    for (unsigned m = 0; m < (1u << 11); m++) {
        purr_px_policy_in_t x = {(int)(m & 1), (int)((m >> 1) & 1), (int)((m >> 2) & 1), (int)((m >> 3) & 1), (int)((m >> 4) & 1), (int)((m >> 5) & 1), (int)((m >> 6) & 1),
                                 (int)((m >> 7) & 1), (int)((m >> 8) & 1), (int)((m >> 9) & 1), (int)((m >> 10) & 1)};
        purr_px_decision_t d = purr_px_policy(&x);
        if (!x.peer_known && !x.we_unowned && !x.peer_signed_by_trusted && !x.owner_sponsored && !x.dev_mode_lan) {
            CHECK(d != PURR_PX_AUTO_PAIR && d != PURR_PX_ALLOW_KNOWN);
        }
        if (x.peer_known) {
            CHECK(d == PURR_PX_ALLOW_KNOWN || d == PURR_PX_DENY);
        }
    }
}

/* ---------------------------------------------------------------- the store */

static void test_store(void)
{
    purr_px_store_t s, t;
    dev_t_ a, b, c;
    make_dev(&a);
    make_dev(&b);
    make_dev(&c);
    purr_px_store_init(&s);
    CHECK(purr_px_store_find_peer(&s, a.id) == NULL);
    CHECK(purr_px_store_add_peer(&s, a.pub, "Alice", PURR_PX_PEER_OWNER) >= 0);
    CHECK(s.has_owner && memcmp(s.owner_id, a.id, 8) == 0);
    CHECK(purr_px_store_add_peer(&s, b.pub, "Bob's phone with a long name", 0) >= 0);
    const purr_px_peer_rec_t *r = purr_px_store_find_peer(&s, b.id);
    CHECK(r != NULL && strlen(r->name) == 15 && memcmp(r->pub, b.pub, 64) == 0);
    int before = 0;
    for (int i = 0; i < PURR_PX_MAX_PEERS; i++) {
        before += s.peers[i].used;
    }
    CHECK_EQ(before, 2);
    purr_px_store_add_peer(&s, b.pub, "Bob", PURR_PX_PEER_AUTO);  /* the same device again is an update, not a second entry */
    int after = 0;
    for (int i = 0; i < PURR_PX_MAX_PEERS; i++) {
        after += s.peers[i].used;
    }
    CHECK_EQ(after, 2);
    CHECK_EQ(purr_px_store_find_peer(&s, b.id)->flags, PURR_PX_PEER_AUTO);

    CHECK_EQ(purr_px_store_add_net(&s, "HomeNet", NULL, 0), 1);   /* the user chose it: trusted, no notice */
    CHECK_EQ(purr_px_store_add_net(&s, "HomeNet", NULL, 0), 0);
    CHECK_EQ(purr_px_store_add_net(&s, "Cafe", b.id, 1), 1);      /* a paired device gave it: trusted, and a notice is due */
    CHECK(purr_px_store_net_trusted(&s, "Cafe") && purr_px_store_net_trusted(&s, "HomeNet") && !purr_px_store_net_trusted(&s, "Other"));
    const purr_px_net_rec_t *note = purr_px_store_next_notice(&s);
    CHECK(note != NULL && strcmp(note->ssid, "Cafe") == 0 && memcmp(note->from, b.id, 8) == 0);
    CHECK(purr_px_store_next_notice(&s) == NULL);                 /* shown once */
    CHECK(purr_px_store_forget_net(&s, "Cafe") && !purr_px_store_net_trusted(&s, "Cafe"));
    CHECK_EQ(purr_px_store_add_net(&s, "", NULL, 0), -1);
    s.allow_any_network = 1;

    uint8_t blob[PURR_PX_STORE_MAX_BYTES];
    int n = purr_px_store_save(&s, blob, sizeof(blob));
    CHECK(n > 0 && n <= PURR_PX_STORE_MAX_BYTES);
    CHECK_EQ(purr_px_store_load(&t, blob, n), 0);
    CHECK(t.has_owner && memcmp(t.owner_id, a.id, 8) == 0 && t.allow_any_network == 1);
    CHECK(purr_px_store_find_peer(&t, a.id) != NULL && purr_px_store_find_peer(&t, b.id) != NULL && purr_px_store_find_peer(&t, c.id) == NULL);
    CHECK(purr_px_store_net_trusted(&t, "HomeNet"));
    CHECK_EQ(purr_px_store_save(&s, blob, 10), -1);

    for (int i = 0; i < n; i += 7) {                              /* any damaged byte is refused whole */
        uint8_t bad[PURR_PX_STORE_MAX_BYTES];
        memcpy(bad, blob, (size_t)n);
        bad[i] ^= 0x55;
        CHECK_EQ(purr_px_store_load(&t, bad, n), -1);
        CHECK(!t.has_owner && purr_px_store_find_peer(&t, a.id) == NULL);
    }
    CHECK_EQ(purr_px_store_load(&t, blob, n - 1), -1);
    CHECK_EQ(purr_px_store_load(&t, NULL, 0), -1);

    uint8_t forged[PURR_PX_STORE_MAX_BYTES];                      /* a record whose id is not the hash of its key is refused even with a good checksum */
    memcpy(forged, blob, (size_t)n);
    forged[15 + 1 + 3] ^= 0xFF;                                   /* the first peer's id */
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < n - 4; i++) {
        crc ^= forged[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    crc = ~crc;
    memcpy(forged + n - 4, &crc, 4);
    CHECK_EQ(purr_px_store_load(&t, forged, n), -1);

    CHECK(purr_px_store_forget_peer(&s, a.id));                   /* forgetting the owner clears the owner */
    CHECK(!s.has_owner);
    for (int i = 0; i < PURR_PX_MAX_PEERS; i++) {                 /* a full store */
        dev_t_ d;
        make_dev(&d);
        purr_px_store_add_peer(&s, d.pub, "x", 0);
    }
    dev_t_ extra;
    make_dev(&extra);
    CHECK_EQ(purr_px_store_add_peer(&s, extra.pub, "x", 0), -1);
}

/* ---------------------------------------------------------------- pairing */

typedef struct {
    purr_px_pair_t p;
    dev_t_ d;
} side_t;

static void side_init(side_t *s, const char *name)
{
    make_dev(&s->d);
    purr_px_pair_init(&s->p, &ECC, s->d.pub, s->d.priv, name);
}

/* Runs the exchange up to the point where both show six digits. Returns 0 on success. */
static int run_to_code(side_t *i, side_t *r)
{
    uint8_t m1[128], m2[128], m3[128];
    int n1 = purr_px_pair_start(&i->p, 1000, m1, sizeof(m1));
    if (n1 <= 0) {
        return -1;
    }
    int n2 = purr_px_pair_rx(&r->p, 1010, m1, n1, m2, sizeof(m2));
    if (n2 <= 0) {
        return -1;
    }
    int n3 = purr_px_pair_rx(&i->p, 1020, m2, n2, m3, sizeof(m3));
    if (n3 <= 0) {
        return -1;
    }
    return purr_px_pair_rx(&r->p, 1030, m3, n3, m1, sizeof(m1)) == 0 ? 0 : -1;
}

static void test_pairing(void)
{
    side_t a, b;
    uint8_t fa[64], fb[64];
    for (int order = 0; order < 3; order++) {
        side_init(&a, "T-Deck Plus");
        side_init(&b, "Mini");
        CHECK_EQ(run_to_code(&a, &b), 0);
        CHECK(purr_px_pair_code(&a.p) >= 0 && purr_px_pair_code(&a.p) <= 999999);
        CHECK_EQ(purr_px_pair_code(&a.p), purr_px_pair_code(&b.p));       /* the same six digits on both screens */
        CHECK_EQ(a.p.state, PURR_PX_PAIR_WAIT_USER);
        CHECK_EQ(b.p.state, PURR_PX_PAIR_WAIT_USER);
        side_t *first = order == 1 ? &b : &a, *second = order == 1 ? &a : &b;
        int n1 = purr_px_pair_user(&first->p, 1, fa, sizeof(fa));
        CHECK_EQ(n1, 33);
        CHECK(first->p.state != PURR_PX_PAIR_DONE);                       /* one yes is not enough */
        CHECK_EQ(purr_px_pair_rx(&second->p, 2000, fa, n1, fb, sizeof(fb)), 0);   /* the peer's proof arrives before the local yes (or after) */
        int n2 = purr_px_pair_user(&second->p, 1, fb, sizeof(fb));
        CHECK_EQ(n2, 33);
        CHECK_EQ(second->p.state, PURR_PX_PAIR_DONE);
        CHECK_EQ(purr_px_pair_rx(&first->p, 2010, fb, n2, fa, sizeof(fa)), 0);
        CHECK_EQ(first->p.state, PURR_PX_PAIR_DONE);
        purr_px_peer_rec_t ra, rb;
        CHECK(purr_px_pair_result(&a.p, &ra) && purr_px_pair_result(&b.p, &rb));
        CHECK(memcmp(ra.pub, b.d.pub, 64) == 0 && memcmp(ra.id, b.d.id, 8) == 0 && strcmp(ra.name, "Mini") == 0);
        CHECK(memcmp(rb.pub, a.d.pub, 64) == 0 && memcmp(rb.id, a.d.id, 8) == 0 && strcmp(rb.name, "T-Deck Plus") == 0);
    }

    side_init(&a, "A");                                           /* a refusal at one end ends it at the other */
    side_init(&b, "B");
    CHECK_EQ(run_to_code(&a, &b), 0);
    int n = purr_px_pair_user(&a.p, 0, fa, sizeof(fa));
    CHECK_EQ(n, 2);
    CHECK_EQ(a.p.state, PURR_PX_PAIR_FAILED);
    CHECK_EQ(a.p.fail, PURR_PX_PAIR_E_REFUSED);
    CHECK_EQ(purr_px_pair_rx(&b.p, 3000, fa, n, fb, sizeof(fb)), -1);
    CHECK_EQ(b.p.fail, PURR_PX_PAIR_E_REFUSED);
    purr_px_peer_rec_t rec;
    CHECK(!purr_px_pair_result(&a.p, &rec) && !purr_px_pair_result(&b.p, &rec));

    side_init(&a, "A");                                           /* a timeout */
    side_init(&b, "B");
    CHECK_EQ(run_to_code(&a, &b), 0);
    CHECK_EQ(purr_px_pair_tick(&a.p, 1000 + PURR_PX_PAIR_TIMEOUT_MS - 1), 0);
    CHECK_EQ(purr_px_pair_tick(&a.p, 1000 + PURR_PX_PAIR_TIMEOUT_MS + 1), 1);
    CHECK_EQ(a.p.fail, PURR_PX_PAIR_E_TIMEOUT);
    CHECK_EQ(purr_px_pair_user(&a.p, 1, fa, sizeof(fa)), -1);     /* too late */

    side_init(&a, "A");                                           /* pairing with oneself */
    uint8_t m1[128], m2[128], m3[128];
    memcpy(b.d.pub, a.d.pub, 64);
    memcpy(b.d.priv, a.d.priv, 32);
    purr_px_pair_init(&b.p, &ECC, a.d.pub, a.d.priv, "me");
    int q1 = purr_px_pair_start(&a.p, 0, m1, sizeof(m1));
    int q2 = purr_px_pair_rx(&b.p, 0, m1, q1, m2, sizeof(m2));
    int q3 = purr_px_pair_rx(&a.p, 0, m2, q2, m3, sizeof(m3));
    CHECK_EQ(q3, -1);
    CHECK_EQ(a.p.fail, PURR_PX_PAIR_E_SELF);
}

static void test_pairing_attacks(void)
{
    side_t a, b, m1, m2;
    uint8_t x1[128], x2[128], x3[128], y1[128], y2[128], y3[128];
    /* A device in the middle: it runs one pairing with each end, so each user sees digits from a different exchange. */
    int same = 0;
    for (int trial = 0; trial < 40; trial++) {
        side_init(&a, "Alice");
        side_init(&b, "Bob");
        side_init(&m1, "Mallory-as-Bob");
        side_init(&m2, "Mallory-as-Alice");
        int n1 = purr_px_pair_start(&a.p, 0, x1, sizeof(x1));
        int n2 = purr_px_pair_rx(&m1.p, 0, x1, n1, x2, sizeof(x2));       /* Alice talks to Mallory */
        int n3 = purr_px_pair_rx(&a.p, 0, x2, n2, x3, sizeof(x3));
        purr_px_pair_rx(&m1.p, 0, x3, n3, y1, sizeof(y1));
        int k1 = purr_px_pair_start(&m2.p, 0, y1, sizeof(y1));            /* Mallory talks to Bob */
        int k2 = purr_px_pair_rx(&b.p, 0, y1, k1, y2, sizeof(y2));
        int k3 = purr_px_pair_rx(&m2.p, 0, y2, k2, y3, sizeof(y3));
        (void)k3;
        if (purr_px_pair_code(&a.p) >= 0 && purr_px_pair_code(&b.p) >= 0 && purr_px_pair_code(&a.p) == purr_px_pair_code(&b.p)) {
            same++;
        }
    }
    CHECK(same <= 1);                                             /* the two screens show different digits (one in a million would match) */

    side_init(&a, "A");                                           /* the revealed key is not the one committed to */
    side_init(&b, "B");
    side_init(&m1, "M");
    int n1 = purr_px_pair_start(&a.p, 0, x1, sizeof(x1));
    int n2 = purr_px_pair_rx(&b.p, 0, x1, n1, x2, sizeof(x2));
    int n3 = purr_px_pair_rx(&a.p, 0, x2, n2, x3, sizeof(x3));
    memcpy(x3 + 1, m1.d.pub, 64);                                 /* the attacker swaps in its own key */
    CHECK_EQ(purr_px_pair_rx(&b.p, 0, x3, n3, y1, sizeof(y1)), -1);
    CHECK_EQ(b.p.fail, PURR_PX_PAIR_E_COMMIT);

    side_init(&a, "A");                                           /* a damaged confirmation */
    side_init(&b, "B");
    CHECK_EQ(run_to_code(&a, &b), 0);
    int f = purr_px_pair_user(&a.p, 1, y1, sizeof(y1));
    y1[5] ^= 1;
    CHECK_EQ(purr_px_pair_rx(&b.p, 0, y1, f, y2, sizeof(y2)), -1);
    CHECK_EQ(b.p.fail, PURR_PX_PAIR_E_MAC);

    side_init(&a, "A");                                           /* a key that is not a point on the curve */
    side_init(&b, "B");
    n1 = purr_px_pair_start(&a.p, 0, x1, sizeof(x1));
    n2 = purr_px_pair_rx(&b.p, 0, x1, n1, x2, sizeof(x2));
    memset(x2 + 1, 0, 64);
    CHECK_EQ(purr_px_pair_rx(&a.p, 0, x2, n2, x3, sizeof(x3)), -1);
    CHECK_EQ(a.p.fail, PURR_PX_PAIR_E_CURVE);

    for (int trial = 0; trial < 20000; trial++) {                 /* noise at every stage never crashes and never pairs */
        side_t z;
        make_dev(&z.d);
        purr_px_pair_init(&z.p, &ECC, z.d.pub, z.d.priv, "z");
        if (trial % 3 == 1) {
            purr_px_pair_start(&z.p, 0, y1, sizeof(y1));
        }
        uint8_t junk[128], out[128];
        int len = (int)(rnd() % 120);
        for (int i = 0; i < len; i++) {
            junk[i] = (uint8_t)rnd();
        }
        if (len > 0 && rnd() % 2) {
            junk[0] = (uint8_t)(1 + rnd() % 5);
        }
        purr_px_pair_rx(&z.p, 0, junk, len, out, sizeof(out));
        CHECK(z.p.state != PURR_PX_PAIR_DONE);
    }
}

/* ---------------------------------------------------------------- the session */

static void test_session(void)
{
    dev_t_ c, s, other;
    make_dev(&c);
    make_dev(&s);
    make_dev(&other);
    purr_px_store_t cs, ss;                                       /* each knows the other, as pairing leaves them */
    purr_px_store_init(&cs);
    purr_px_store_init(&ss);
    purr_px_store_add_peer(&cs, s.pub, "server", 0);
    purr_px_store_add_peer(&ss, c.pub, "client", 0);
    purr_px_sess_t sc, sv;
    purr_px_sess_init(&sc, &ECC, c.pub, c.priv);
    purr_px_sess_init(&sv, &ECC, s.pub, s.priv);
    uint8_t h1[PURR_PX_SESS_HELLO1], h2[PURR_PX_SESS_HELLO2];
    CHECK_EQ(purr_px_sess_client_start(&sc, s.id, h1, sizeof(h1)), PURR_PX_SESS_HELLO1);
    CHECK_EQ(purr_px_sess_server_accept(&sv, &ss, h1, PURR_PX_SESS_HELLO1, h2, sizeof(h2)), PURR_PX_SESS_HELLO2);
    CHECK_EQ(purr_px_sess_client_finish(&sc, &cs, h2, PURR_PX_SESS_HELLO2), 0);
    CHECK(sc.ready && sv.ready);

    static uint8_t msg[4096], sealed[4096 + 64], back[4096];
    for (int i = 0; i < 4096; i++) {
        msg[i] = (uint8_t)(i * 13);
    }
    uint8_t ch, fl;
    int lens[] = {0, 1, 100, 4000, 4096};
    for (size_t li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
        int n = purr_px_sess_seal(&sc, (uint8_t)(li % 6), 0x40, msg, lens[li], sealed, sizeof(sealed));
        CHECK_EQ(n, lens[li] + PURR_PX_SESS_OVERHEAD);
        int m = purr_px_sess_open(&sv, sealed, n, &ch, &fl, back, sizeof(back));
        CHECK_EQ(m, lens[li]);
        CHECK(ch == (uint8_t)(li % 6) && fl == 0x40 && memcmp(back, msg, (size_t)lens[li]) == 0);
        n = purr_px_sess_seal(&sv, PURR_PX_CH_DISPLAY, 0, msg, lens[li], sealed, sizeof(sealed));   /* and the other way */
        CHECK_EQ(purr_px_sess_open(&sc, sealed, n, &ch, &fl, back, sizeof(back)), lens[li]);
        CHECK(memcmp(back, msg, (size_t)lens[li]) == 0);
    }

    int n = purr_px_sess_seal(&sc, PURR_PX_CH_TERMINAL, 0, msg, 50, sealed, sizeof(sealed));
    uint8_t copy[200];
    memcpy(copy, sealed, (size_t)n);
    CHECK_EQ(purr_px_sess_open(&sv, sealed, n, &ch, &fl, back, sizeof(back)), 50);
    CHECK_EQ(purr_px_sess_open(&sv, copy, n, &ch, &fl, back, sizeof(back)), -1);      /* a replay */
    int n2 = purr_px_sess_seal(&sc, PURR_PX_CH_TERMINAL, 0, msg, 50, sealed, sizeof(sealed));
    sealed[PURR_PX_SESS_HDR + 3] ^= 0x10;                         /* a damaged message is refused and does not use up its counter */
    CHECK_EQ(purr_px_sess_open(&sv, sealed, n2, &ch, &fl, back, sizeof(back)), -1);
    sealed[PURR_PX_SESS_HDR + 3] ^= 0x10;
    CHECK_EQ(purr_px_sess_open(&sv, sealed, n2, &ch, &fl, back, sizeof(back)), 50);
    int n3 = purr_px_sess_seal(&sc, PURR_PX_CH_CONTROL, 0, msg, 5, sealed, sizeof(sealed));
    sealed[0] ^= 1;                                               /* the channel is covered by the tag */
    CHECK_EQ(purr_px_sess_open(&sv, sealed, n3, &ch, &fl, back, sizeof(back)), -1);
    CHECK_EQ(purr_px_sess_seal(&sc, 0, 0, msg, 100, sealed, 50), -1);                 /* it does not fit */

    purr_px_sess_t sx;                                            /* a device that is not paired is refused by the server */
    purr_px_sess_init(&sx, &ECC, other.pub, other.priv);
    uint8_t hx[PURR_PX_SESS_HELLO1];
    purr_px_sess_t sv2;
    purr_px_sess_init(&sv2, &ECC, s.pub, s.priv);
    purr_px_sess_client_start(&sx, s.id, hx, sizeof(hx));
    CHECK_EQ(purr_px_sess_server_accept(&sv2, &ss, hx, PURR_PX_SESS_HELLO1, h2, sizeof(h2)), -1);
    uint8_t hy[PURR_PX_SESS_HELLO1];                              /* a device that claims another's id has no signature that checks */
    purr_px_sess_init(&sx, &ECC, other.pub, other.priv);
    purr_px_sess_client_start(&sx, s.id, hy, sizeof(hy));
    memcpy(hy, c.id, 8);
    CHECK_EQ(purr_px_sess_server_accept(&sv2, &ss, hy, PURR_PX_SESS_HELLO1, h2, sizeof(h2)), -1);

    purr_px_sess_t sc3, imp;                                      /* a server that is not the one the client meant to reach */
    purr_px_sess_init(&sc3, &ECC, c.pub, c.priv);
    purr_px_sess_init(&imp, &ECC, other.pub, other.priv);
    purr_px_store_t impstore;
    purr_px_store_init(&impstore);
    purr_px_store_add_peer(&impstore, c.pub, "client", 0);
    purr_px_sess_client_start(&sc3, s.id, h1, sizeof(h1));
    CHECK_EQ(purr_px_sess_server_accept(&imp, &impstore, h1, PURR_PX_SESS_HELLO1, h2, sizeof(h2)), -1);   /* an honest other server refuses a hello that was not for it */
    memcpy(imp.self_id, s.id, 8);                                 /* one that pretends to be the server: it signs with its own key */
    CHECK_EQ(purr_px_sess_server_accept(&imp, &impstore, h1, PURR_PX_SESS_HELLO1, h2, sizeof(h2)), PURR_PX_SESS_HELLO2);
    CHECK_EQ(purr_px_sess_client_finish(&sc3, &cs, h2, PURR_PX_SESS_HELLO2), -1);     /* its signature is not the paired server's */
    CHECK(!sc3.ready);

    purr_px_sess_t early;                                         /* nothing opens or seals before the keys are ready */
    purr_px_sess_init(&early, &ECC, c.pub, c.priv);
    CHECK_EQ(purr_px_sess_seal(&early, 0, 0, msg, 5, sealed, sizeof(sealed)), -1);
    CHECK_EQ(purr_px_sess_open(&early, sealed, 40, &ch, &fl, back, sizeof(back)), -1);
}

int main(void)
{
    uECC_set_rng(rng_fn);
    test_policy();
    test_store();
    test_pairing();
    test_pairing_attacks();
    test_session();
    TK_DONE("test_prox_pair");
}
