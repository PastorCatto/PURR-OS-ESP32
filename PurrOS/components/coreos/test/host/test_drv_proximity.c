/* The proximity driver (Modules/drivers/proximity_module.c) run exactly as it is built for the device, against a fake link bus. The other end is the plain-C core
 * (Modules/proximity) driven by hand, which is what the Mini links in: a pairing with six digits both sides compare, a secure session, a Wi-Fi network handed
 * over and one handed back, a long message in fragments, the things that must be refused, and the pairings surviving a restart. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "testkit.h"
#include "purr_board.h"
#include "uECC.h"
#include "../../../../../Modules/drivers/proximity_module.c"

/* ---------------------------------------------------------------- a fake link bus */

#define QN 64
typedef struct { uint8_t d[PURR_PX_MAX_FRAME]; int n; uint8_t mac[6]; int8_t rssi; } qf_t;
static qf_t to_dev[QN], from_dev[QN];
static int to_dev_r, to_dev_w, from_dev_r, from_dev_w;
static uint64_t t_us;
static uint8_t blob[2048];
static int blob_len;
static char saved_ssid[33], saved_pass[65];
static uint8_t saved_from[8];
static int saved_count;
static char have_ssid[33] = "HomeNet", have_pass[65] = "hunter2hunter2";

static int fb_rand(uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) b[i] = (uint8_t)rand(); return 0; }
static int fb_radio_send(const uint8_t *mac, const uint8_t *d, size_t n)
{
    (void)mac;
    if (from_dev_w - from_dev_r >= QN) return -1;
    qf_t *q = &from_dev[from_dev_w++ % QN];
    memcpy(q->d, d, n);
    q->n = (int)n;
    return 0;
}
static int fb_radio_recv(uint8_t mac[6], int8_t *rssi, uint8_t *buf, size_t cap)
{
    if (to_dev_r == to_dev_w) return 0;
    qf_t *q = &to_dev[to_dev_r++ % QN];
    memcpy(mac, q->mac, 6);
    *rssi = q->rssi;
    if ((size_t)q->n > cap) return -1;
    memcpy(buf, q->d, (size_t)q->n);
    return q->n;
}
static int fb_store_load(uint8_t *b, size_t cap) { if ((size_t)blob_len > cap) return -1; memcpy(b, blob, (size_t)blob_len); return blob_len; }
static int fb_store_save(const uint8_t *d, size_t n) { memcpy(blob, d, n); blob_len = (int)n; return 0; }
static int fb_net_info(uint32_t *ip, char ssid[33], uint8_t *ch) { *ip = 0; ssid[0] = 0; *ch = 1; return 0; }
static int fb_net_saved_get(const char *ssid, char pass[65]) { if (strcmp(ssid, have_ssid) != 0) return -1; strcpy(pass, have_pass); return 0; }
static int fb_net_saved_add(const char *ssid, const char *pass, const uint8_t peer[8])
{
    strcpy(saved_ssid, ssid); strcpy(saved_pass, pass); memcpy(saved_from, peer, 8); saved_count++;
    return 0;
}
static uint64_t fb_now_us(void) { return t_us; }

static purr_driver_bus_table_t bus;

/* ---------------------------------------------------------------- the other end */

static uint8_t p_pub[64], p_priv[32], p_id[8];
static const uint8_t p_mac[6] = {2, 4, 6, 8, 10, 12};
static purr_px_pair_t p_pair;
static purr_px_sess_t p_sess;
static int p_rssi = -70;

static void p_ecc_random(uint8_t *o, int n) { fb_rand(o, (size_t)n); }
static const purr_px_ecc_t p_ecc = {.random = p_ecc_random, .keypair = e_keypair, .shared = e_shared, .sign = e_sign, .verify = e_verify};

static void feed(uint8_t type, const uint8_t *payload, int n)
{
    qf_t *q = &to_dev[to_dev_w++ % QN];
    purr_px_hdr_t h = {.type = type};
    memcpy(h.id, p_id, 8);
    int hl = purr_px_hdr_put(&h, q->d, PURR_PX_MAX_FRAME);
    memcpy(q->d + hl, payload, (size_t)n);
    q->n = hl + n;
    memcpy(q->mac, p_mac, 6);
    q->rssi = (int8_t)p_rssi;
}

static void tick_ms(void *h, int ms)
{
    t_us += (uint64_t)ms * 1000;
    ((const purr_link_ops_t *)s_driver.ops)->tick(h, 0);
}

/* The next frame the device sent of this type, or -1. Frames of other types are skipped. */
static int take(uint8_t type, uint8_t *payload, int cap)
{
    while (from_dev_r < from_dev_w) {
        qf_t *q = &from_dev[from_dev_r++ % QN];
        purr_px_hdr_t h;
        int hl = purr_px_hdr_get(q->d, q->n, &h);
        if (hl > 0 && h.type == type) {
            int n = q->n - hl;
            if (n > cap) return -1;
            memcpy(payload, q->d + hl, (size_t)n);
            return n;
        }
    }
    return -1;
}

static void drain(void) { from_dev_r = from_dev_w; }

/* The device's secure frames, put back together and opened. */
static int take_secure(uint8_t *ch, uint8_t *pt, int cap)
{
    purr_px_reasm_t *r = calloc(1, sizeof(*r));
    purr_px_reasm_init(r);
    uint8_t piece[PURR_PX_MAX_FRAME];
    int n, got = 0, res = -1;
    while ((n = take(PURR_PX_T_SECURE, piece, sizeof(piece))) >= 0) {
        if (purr_px_reasm_push(r, 0, piece, n) == 1) { got = 1; break; }
    }
    if (got) {
        uint8_t fl;
        res = purr_px_sess_open(&p_sess, r->buf, r->len, ch, &fl, pt, cap);
    }
    free(r);
    return res;
}

static void p_send_secure(uint8_t ch, const uint8_t *d, int n)
{
    uint8_t rec[PURR_PX_MSG_MAX];
    int rl = purr_px_sess_seal(&p_sess, ch, 0, d, n, rec, sizeof(rec));
    int pieces = purr_px_frag_count(rl);
    static uint16_t mid = 100;
    mid++;
    for (int i = 0; i < pieces; i++) {
        uint8_t pc[PURR_PX_MAX_PAYLOAD];
        int pl = purr_px_frag_put(mid, rec, rl, i, pc, sizeof(pc));
        feed(PURR_PX_T_SECURE, pc, pl);
    }
}

static void p_beacon(void)
{
    purr_px_beacon_t b;
    memset(&b, 0, sizeof(b));
    b.roles = PURR_PX_ROLE_SCREEN;
    b.klass = PURR_PX_CLASS_MINI;
    strcpy(b.name, "Mini-test");
    uint8_t p[PURR_PX_BEACON_LEN];
    int n = purr_px_beacon_put(&b, p, sizeof(p));
    feed(PURR_PX_T_BEACON, p, n);
}

/* Runs a pairing started by the other end; `device_says`/`peer_says` are the two people's answers. Returns the six digits the device showed. */
static int run_pairing(void *h, int device_says, int peer_says, int *peer_code)
{
    uint8_t m[PURR_PX_PAIR_MAX_MSG + 4], out[PURR_PX_PAIR_MAX_MSG + 4];
    const purr_link_ops_t *ops = s_driver.ops;
    purr_px_pair_init(&p_pair, &p_ecc, p_pub, p_priv, "Mini-test");
    int n = purr_px_pair_start(&p_pair, 0, m, sizeof(m));
    feed(PURR_PX_T_PAIR, m, n);
    int code = -2, answered_dev = 0, answered_peer = 0;
    for (int round = 0; round < 40; round++) {
        tick_ms(h, 20);
        while ((n = take(PURR_PX_T_PAIR, m, sizeof(m))) >= 0) {
            int w = purr_px_pair_rx(&p_pair, 0, m, n, out, sizeof(out));
            if (w > 0) feed(PURR_PX_T_PAIR, out, w);
        }
        purr_link_prompt_t pr;
        if (!answered_dev && ops->prompt(h, &pr) == 1) {
            code = pr.code;
            ops->answer(h, device_says);
            answered_dev = 1;
        }
        if (!answered_peer && p_pair.state == PURR_PX_PAIR_WAIT_USER) {
            if (peer_code) *peer_code = purr_px_pair_code(&p_pair);
            int w = purr_px_pair_user(&p_pair, peer_says, out, sizeof(out));
            if (w > 0) feed(PURR_PX_T_PAIR, out, w);
            answered_peer = 1;
        }
    }
    return code;
}

static void setup(void **h)
{
    memset(&bus, 0, sizeof(bus));
    bus.abi_version = PURR_DRIVER_BUS_ABI_VERSION;
    bus.now_us = fb_now_us;
    bus.rand_bytes = fb_rand;
    bus.radio_send = fb_radio_send;
    bus.radio_recv = fb_radio_recv;
    bus.store_load = fb_store_load;
    bus.store_save = fb_store_save;
    bus.net_info = fb_net_info;
    bus.net_saved_get = fb_net_saved_get;
    bus.net_saved_add = fb_net_saved_add;
    proximity_entry(&bus);
    memset(&s_link, 0, sizeof(s_link));
    *h = NULL;
    purr_px_ecc_t unused = p_ecc;
    (void)unused;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    srand(7);
    uECC_set_rng(rng_fn);
    void *h = NULL;
    setup(&h);
    CHECK(s_driver.class_id == PURR_DRV_CLASS_LINK);
    CHECK(s_driver.probe(NULL, NULL, &h) == 0 && h != NULL);
    const purr_link_ops_t *ops = s_driver.ops;
    CHECK(ops->version == 2);
    uint8_t dev_id[8];
    char dev_name[17];
    CHECK(ops->self(h, dev_id, dev_name) == 0);
    CHECK(strncmp(dev_name, "PURR-", 5) == 0);
    CHECK(blob_len > 100);                                  /* the identity was saved on first start */

    /* The other end has a key of its own. */
    CHECK(e_keypair(p_pub, p_priv) == 1);
    purr_px_id_from_pub(p_pub, p_id);

    /* 1. Beacons go out, and one coming in puts the other device in the nearby list. */
    tick_ms(h, 1100);
    uint8_t m[400];
    int n = take(PURR_PX_T_BEACON, m, sizeof(m));
    CHECK(n == PURR_PX_BEACON_LEN);
    purr_px_beacon_t b;
    CHECK(purr_px_beacon_get(m, n, &b) > 0 && strncmp(b.name, "PURR-", 5) == 0 && (b.roles & PURR_PX_ROLE_SCREEN));
    purr_link_peer_t peers[8];
    CHECK(ops->nearby(h, peers, 8) == 0);
    p_beacon();
    tick_ms(h, 20);
    CHECK(ops->nearby(h, peers, 8) == 1);
    CHECK(memcmp(peers[0].id, p_id, 8) == 0 && !peers[0].paired && peers[0].online && peers[0].via == PURR_LINK_VIA_RADIO);
    CHECK(strcmp(peers[0].name, "Mini-test") == 0 && peers[0].klass == PURR_PX_CLASS_MINI && !peers[0].close);
    CHECK(ops->paired(h, peers, 8) == 0);

    /* 2. A session with a device that is not paired goes nowhere. */
    {
        purr_px_sess_t cs;
        purr_px_sess_init(&cs, &p_ecc, p_pub, p_priv);
        uint8_t hello[PURR_PX_SESS_HELLO1 + 1];
        CHECK(purr_px_sess_client_start(&cs, dev_id, hello + 1, sizeof(hello) - 1) > 0);
        hello[0] = HELLO1_TAG;
        feed(PURR_PX_T_PAIR, hello, sizeof(hello));
        tick_ms(h, 20);
        CHECK(take(PURR_PX_T_PAIR, m, sizeof(m)) < 0);       /* no answer to a stranger */
    }

    /* 2b. A stranger that is not close is not even answered, unless the person opened the pairing window. */
    {
        drain();
        purr_px_pair_init(&p_pair, &p_ecc, p_pub, p_priv, "Mini-test");
        uint8_t req[PURR_PX_PAIR_MAX_MSG];
        int rn = purr_px_pair_start(&p_pair, 0, req, sizeof(req));
        feed(PURR_PX_T_PAIR, req, rn);
        tick_ms(h, 20);
        CHECK(take(PURR_PX_T_PAIR, m, sizeof(m)) < 0);
        CHECK(s_link.pair.state == PURR_PX_PAIR_IDLE);
        CHECK(ops->option(h, PURR_LINK_OPT_PAIR_WINDOW, 30) >= 29);
        CHECK(ops->option(h, PURR_LINK_OPT_PAIR_WINDOW, -1000) >= 29);
        ops->option(h, PURR_LINK_OPT_PAIR_WINDOW, 0);                       /* the test opens it again below, through the real flow */
        CHECK(ops->option(h, PURR_LINK_OPT_PAIR_WINDOW, -1000) == 0);
        drain();
    }
    ops->option(h, PURR_LINK_OPT_PAIR_WINDOW, 60);

    /* 3. Pairing, started by the other end, with the six digits compared and both people saying yes. */
    int peer_code = -1;
    int dev_code = run_pairing(h, 1, 1, &peer_code);
    CHECK(dev_code >= 0 && dev_code <= 999999 && dev_code == peer_code);
    CHECK(ops->paired(h, peers, 8) == 1 && memcmp(peers[0].id, p_id, 8) == 0 && peers[0].paired);
    CHECK(p_pair.state == PURR_PX_PAIR_DONE);
    CHECK(s_link.pair.state == PURR_PX_PAIR_IDLE);           /* the device is ready for the next one */
    {
        purr_link_prompt_t pr;
        CHECK(ops->prompt(h, &pr) == 0);
    }

    /* 4. A pairing the person at the device refuses leaves nothing behind. */
    CHECK(ops->forget(h, p_id) == 1);
    CHECK(ops->paired(h, peers, 8) == 0);
    drain();
    run_pairing(h, 0, 1, NULL);
    CHECK(ops->paired(h, peers, 8) == 0);
    CHECK(p_pair.state == PURR_PX_PAIR_FAILED);
    drain();
    run_pairing(h, 1, 0, NULL);                              /* and the other person refusing */
    CHECK(ops->paired(h, peers, 8) == 0);
    drain();
    CHECK(run_pairing(h, 1, 1, NULL) >= 0);
    CHECK(ops->paired(h, peers, 8) == 1);

    /* 5. A secure session, opened by the paired device. */
    drain();
    purr_px_store_t peer_store;
    purr_px_store_init(&peer_store);
    uint8_t dev_pub[64];
    memcpy(dev_pub, s_link.pub, 64);
    CHECK(purr_px_store_add_peer(&peer_store, dev_pub, "T-Deck", 0) >= 0);
    purr_px_sess_init(&p_sess, &p_ecc, p_pub, p_priv);
    uint8_t hello[PURR_PX_SESS_HELLO1 + 1];
    CHECK(purr_px_sess_client_start(&p_sess, dev_id, hello + 1, sizeof(hello) - 1) > 0);
    hello[0] = HELLO1_TAG;
    feed(PURR_PX_T_PAIR, hello, sizeof(hello));
    tick_ms(h, 20);
    n = take(PURR_PX_T_PAIR, m, sizeof(m));
    CHECK(n == PURR_PX_SESS_HELLO2 + 1 && m[0] == HELLO2_TAG);
    CHECK(purr_px_sess_client_finish(&p_sess, &peer_store, m + 1, n - 1) == 0 && p_sess.ready);
    CHECK(ops->paired(h, peers, 8) == 1 && peers[0].session);

    /* 6. The paired device hands over a Wi-Fi network: saved, trusted, announced once, listed. */
    {
        uint8_t off[80];
        int k = 0;
        off[k++] = CTL_WIFI_OFFER;
        off[k++] = 7; memcpy(off + k, "CafeNet", 7); k += 7;
        off[k++] = 10; memcpy(off + k, "latte-1234", 10); k += 10;
        p_send_secure(PURR_PX_CH_CONTROL, off, k);
        tick_ms(h, 20);
        CHECK(saved_count == 1 && strcmp(saved_ssid, "CafeNet") == 0 && strcmp(saved_pass, "latte-1234") == 0 && memcmp(saved_from, p_id, 8) == 0);
        uint8_t ch, pt[64];
        int pl = take_secure(&ch, pt, sizeof(pt));
        CHECK(pl == 2 && ch == PURR_PX_CH_CONTROL && pt[0] == CTL_WIFI_ACK);
        char line[64], ssids[4][33];
        CHECK(ops->notice(h, line) == 1 && strstr(line, "CafeNet") != NULL);
        CHECK(ops->notice(h, line) == 0);                    /* once */
        CHECK(ops->nets(h, ssids, 4) == 1 && strcmp(ssids[0], "CafeNet") == 0);
        CHECK(purr_px_store_net_trusted(&s_link.store, "CafeNet") == 1);
        CHECK(ops->forget_net(h, "CafeNet") == 1 && ops->nets(h, ssids, 4) == 0);
    }

    /* 7. And the other way: the device hands one of its networks over. The password only ever travels inside the session. */
    {
        drain();
        CHECK(ops->share_wifi(h, p_id, "HomeNet") == 0);
        uint8_t ch, pt[128];
        int pl = take_secure(&ch, pt, sizeof(pt));
        CHECK(pl > 3 && ch == PURR_PX_CH_CONTROL && pt[0] == CTL_WIFI_OFFER && pt[1] == 7 && memcmp(pt + 2, "HomeNet", 7) == 0);
        CHECK(pt[9] == 14 && memcmp(pt + 10, "hunter2hunter2", 14) == 0);
        CHECK(ops->share_wifi(h, p_id, "NotSaved") < 0);
        /* the password is in none of the frames the device put on the air in the clear */
        int leaked = 0;
        for (int i = 0; i < from_dev_w; i++) {
            const qf_t *q = &from_dev[i % QN];
            for (int j = 0; j + 14 <= q->n; j++) if (memcmp(q->d + j, "hunter2hunter2", 14) == 0) leaked = 1;
        }
        CHECK(!leaked);
    }

    /* 8. A long message on a data channel, in pieces both ways. */
    {
        static uint8_t big[3000], back[3100];
        for (int i = 0; i < 3000; i++) big[i] = (uint8_t)(i * 7 + 3);
        p_send_secure(PURR_PX_CH_DISPLAY, big, 3000);
        for (int k = 0; k < 3; k++) tick_ms(h, 20);                         /* thirteen pieces: the driver takes a bounded number per tick */
        uint8_t ch = 0;
        int got = ops->recv(h, p_id, &ch, back, sizeof(back));
        CHECK(got == 3000 && ch == PURR_PX_CH_DISPLAY && memcmp(big, back, 3000) == 0);
        CHECK(ops->recv(h, p_id, &ch, back, sizeof(back)) == 0);
        drain();
        for (int i = 0; i < 2000; i++) big[i] = (uint8_t)(i ^ 0x5A);
        CHECK(ops->send(h, p_id, PURR_PX_CH_DISPLAY, big, 2000) == 0);
        uint8_t pt[2100];
        int pl = take_secure(&ch, pt, sizeof(pt));
        CHECK(pl == 2000 && ch == PURR_PX_CH_DISPLAY && memcmp(pt, big, 2000) == 0);
        CHECK(ops->send(h, p_id, PURR_PX_CH_DISPLAY, big, 5000) < 0);       /* over the limit */
    }

    /* 9. A message that was tampered with, or replayed, is dropped without a word. */
    {
        uint8_t small[16] = "hello there";
        uint8_t rec[PURR_PX_MSG_MAX];
        int rl = purr_px_sess_seal(&p_sess, PURR_PX_CH_DISPLAY, 0, small, 12, rec, sizeof(rec));
        uint8_t pc[PURR_PX_MAX_PAYLOAD];
        int pl = purr_px_frag_put(900, rec, rl, 0, pc, sizeof(pc));
        pc[pl - 1] ^= 1;
        feed(PURR_PX_T_SECURE, pc, pl);
        tick_ms(h, 20);
        uint8_t ch, buf[64];
        CHECK(ops->recv(h, p_id, &ch, buf, sizeof(buf)) == 0);
        pl = purr_px_frag_put(901, rec, rl, 0, pc, sizeof(pc));            /* the untampered one still works */
        feed(PURR_PX_T_SECURE, pc, pl);
        tick_ms(h, 20);
        CHECK(ops->recv(h, p_id, &ch, buf, sizeof(buf)) == 12);
        feed(PURR_PX_T_SECURE, pc, pl);                                     /* the same record again: a replay */
        tick_ms(h, 20);
        CHECK(ops->recv(h, p_id, &ch, buf, sizeof(buf)) == 0);
    }

    /* 10. The options. */
    CHECK(ops->option(h, PURR_LINK_OPT_AUTO_ANY_NET, -1000) == 0);          /* off until someone turns it on */
    CHECK(ops->option(h, PURR_LINK_OPT_AUTO_ANY_NET, 1) == 1);
    CHECK(ops->option(h, PURR_LINK_OPT_CLOSE_DBM, -1000) == PURR_PX_CLOSE_DBM_DEFAULT);
    CHECK(ops->option(h, PURR_LINK_OPT_CLOSE_DBM, 5) == PURR_PX_CLOSE_DBM_DEFAULT);   /* out of range: unchanged */

    /* 10b. A secure stream, as a TCP connection carries it: the handshake, then one sealed record per message, in both directions; a device that is not paired is refused. */
    {
        purr_px_sess_t cs;
        purr_px_sess_init(&cs, &p_ecc, p_pub, p_priv);
        uint8_t h1[PURR_PX_SESS_HELLO1], h2[PURR_PX_SESS_HELLO2], who[8];
        CHECK(purr_px_sess_client_start(&cs, dev_id, h1, sizeof(h1)) == PURR_PX_SESS_HELLO1);
        int st = ops->stream_accept(h, h1, sizeof(h1), h2, sizeof(h2), who);
        CHECK(st >= 1 && memcmp(who, p_id, 8) == 0);
        CHECK(purr_px_sess_client_finish(&cs, &peer_store, h2, sizeof(h2)) == 0);
        uint8_t rec[600], pt[600], ch, fl;
        uint8_t msg[300];
        for (int i = 0; i < 300; i++) msg[i] = (uint8_t)(i * 3);
        int rl = purr_px_sess_seal(&cs, PURR_PX_CH_DISPLAY, 0, msg, 300, rec, sizeof(rec));
        CHECK(rl == 326);
        CHECK(ops->stream_open(h, st, rec, (size_t)rl, &ch, pt, sizeof(pt)) == 300 && ch == PURR_PX_CH_DISPLAY && memcmp(pt, msg, 300) == 0);
        CHECK(ops->stream_open(h, st, rec, (size_t)rl, &ch, pt, sizeof(pt)) < 0);                       /* a replay */
        int dl = ops->stream_seal(h, st, PURR_PX_CH_DISPLAY, msg, 300, rec, sizeof(rec));
        CHECK(dl == 326 && purr_px_sess_open(&cs, rec, dl, &ch, &fl, pt, sizeof(pt)) == 300 && memcmp(pt, msg, 300) == 0);
        rec[40] ^= 1;                                                                                    /* a flipped bit */
        CHECK(purr_px_sess_open(&cs, rec, dl, &ch, &fl, pt, sizeof(pt)) < 0);
        ops->stream_close(h, st);
        CHECK(ops->stream_seal(h, st, 1, msg, 10, rec, sizeof(rec)) < 0);
        /* a client the device has not paired with */
        uint8_t o_pub[64], o_priv[32];
        CHECK(e_keypair(o_pub, o_priv) == 1);
        purr_px_sess_t os;
        purr_px_sess_init(&os, &p_ecc, o_pub, o_priv);
        purr_px_store_t os_store;
        purr_px_store_init(&os_store);
        purr_px_store_add_peer(&os_store, dev_pub, "T-Deck", 0);
        CHECK(purr_px_sess_client_start(&os, dev_id, h1, sizeof(h1)) == PURR_PX_SESS_HELLO1);
        CHECK(ops->stream_accept(h, h1, sizeof(h1), h2, sizeof(h2), who) < 0);
        CHECK(ops->has_peers(h) == 1);
    }

    /* 11. Brought close: a strong signal held for a moment. */
    p_rssi = -35;
    for (int i = 0; i < 12; i++) { p_beacon(); tick_ms(h, 100); }
    CHECK(ops->nearby(h, peers, 8) == 1 && peers[0].close);

    /* 12. Everything survives a restart: the same identity, the same pairings, the setting. */
    {
        uint8_t id_before[8];
        memcpy(id_before, dev_id, 8);
        tick_ms(h, 50);                                                     /* a save is made on the tick after a change */
        s_driver.remove(h);
        void *h2 = NULL;
        memset(&s_link, 0, sizeof(s_link));
        CHECK(s_driver.probe(NULL, NULL, &h2) == 0);
        uint8_t id_after[8];
        char nm[17];
        ops->self(h2, id_after, nm);
        CHECK(memcmp(id_before, id_after, 8) == 0);
        CHECK(ops->paired(h2, peers, 8) == 1 && memcmp(peers[0].id, p_id, 8) == 0);
        CHECK(ops->option(h2, PURR_LINK_OPT_AUTO_ANY_NET, -1000) == 1);
        CHECK(!peers[0].session);                                           /* a session does not outlive the restart */
    }

    /* 13. Forgetting a device ends its session and its pairing. */
    {
        void *h3 = s_link.in_use ? (void *)&s_link : NULL;
        CHECK(ops->forget(h3, p_id) == 1);
        CHECK(ops->paired(h3, peers, 8) == 0);
        CHECK(ops->connect(h3, p_id) < 0);
    }

    TK_DONE("drv_proximity");
}
