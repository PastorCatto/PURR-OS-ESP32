/* The Meshtastic node (Modules/meshtastic/purr_mt_node.c) on a simulated mesh: a few nodes in a line, each hearing only its neighbours. Frames are delivered between them with the
 * airtime the node itself reports; the checks are on what a person would see (texts, acknowledgements, failures), on duplicates, and on what a node passes on. */
#include <stdio.h>
#include <string.h>

#include "testkit.h"
#include "purr_mt_node.h"
#include "purr_mt_pkc.h"

#define MAXN 8
static purr_mt_t node[MAXN];
static int nnodes;
static uint8_t link_up[MAXN][MAXN];                      /* link_up[a][b]: b hears a */
static uint32_t now;
static uint32_t rngs[MAXN];
static int cur;
static int tx_count[MAXN];
static int drop_all;                                     /* the radio channel is dead */
static int texts[MAXN], delivered[MAXN], failed[MAXN], heard[MAXN];
static char last_text[MAXN][PURR_MT_TEXT_MAX + 1];
static uint32_t last_text_from[MAXN];

static uint32_t rand_cb(void)
{
    rngs[cur] = rngs[cur] * 1664525u + 1013904223u;
    return rngs[cur] >> 8;
}

static int make_keys;                                    /* 1: the nodes added next have key pairs and send sealed direct messages */
static int pkc_seen[MAXN];

static void add_node(uint32_t num, const char *name, const uint8_t *psk, int psk_len)
{
    int i = nnodes++;
    purr_mt_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.my_num = num;
    snprintf(c.long_name, sizeof(c.long_name), "%s", name);
    snprintf(c.short_name, sizeof(c.short_name), "%.4s", name);
    c.hop_limit = 3;
    c.rebroadcast = 1;
    c.region = purr_mt_region_by_name("US");
    c.preset = purr_mt_preset_by_name("LongFast");
    c.rand32 = rand_cb;
    rngs[i] = num * 2654435761u;
    cur = i;
    if (make_keys) {
        uint8_t rr[32];
        for (int k = 0; k < 32; k++) rr[k] = (uint8_t)(num * 31u + (uint32_t)k * 17u + 3u);
        purr_mt_pkc_keypair(rr, c.priv, c.pub);
        c.has_pkc = 1;
        c.pkc_send = 1;
    }
    purr_mt_node_init(&node[i], &c, now);
    purr_mt_channel_set(&node[i], 0, "LongFast", psk, psk_len, 1);
}

static void reset(void)
{
    nnodes = 0;
    memset(link_up, 0, sizeof(link_up));
    memset(texts, 0, sizeof(texts));
    memset(delivered, 0, sizeof(delivered));
    memset(failed, 0, sizeof(failed));
    memset(heard, 0, sizeof(heard));
    memset(tx_count, 0, sizeof(tx_count));
    memset(last_text, 0, sizeof(last_text));
    drop_all = 0;
    memset(pkc_seen, 0, sizeof(pkc_seen));
    make_keys = 0;
    now = 1000;
}

static void line(int a, int b)
{
    link_up[a][b] = link_up[b][a] = 1;
}

static void drain_events(int i)
{
    purr_mt_event_t e;
    cur = i;
    while (purr_mt_node_event(&node[i], &e)) {
        switch (e.kind) {
        case PURR_MT_EV_TEXT:
            texts[i]++;
            snprintf(last_text[i], sizeof(last_text[i]), "%s", e.text);
            last_text_from[i] = e.from;
            pkc_seen[i] += e.pkc;
            break;
        case PURR_MT_EV_DELIVERED: delivered[i]++; break;
        case PURR_MT_EV_FAILED: failed[i]++; break;
        case PURR_MT_EV_HEARD: heard[i]++; break;
        default: break;
        }
    }
}

static void run(uint32_t ms)
{
    uint32_t end = now + ms;
    while (now < end) {
        now += 10;
        for (int i = 0; i < nnodes; i++) {
            uint8_t f[255];
            size_t n;
            cur = i;
            if (purr_mt_node_tick(&node[i], now, f, sizeof(f), &n)) {
                tx_count[i]++;
                if (!drop_all) {
                    for (int j = 0; j < nnodes; j++) {
                        if (link_up[i][j]) {
                            cur = j;
                            purr_mt_node_rx(&node[j], now, f, n, -90, 5);
                        }
                    }
                }
            }
        }
        for (int i = 0; i < nnodes; i++) {
            drain_events(i);
        }
    }
}

static const uint8_t K1 = 1;

static void test_broadcast(void)
{
    reset();
    add_node(0x1111, "Alpha", &K1, 1);
    add_node(0x2222, "Bravo", &K1, 1);
    add_node(0x3333, "Charlie", &K1, 1);
    line(0, 1);
    line(1, 2);                                                         /* A hears B, B hears C, A and C cannot hear each other */
    run(60000);                                                         /* the nodes announce themselves meanwhile */
    memset(texts, 0, sizeof(texts));
    memset(tx_count, 0, sizeof(tx_count));
    cur = 0;
    uint32_t id = purr_mt_node_send_text(&node[0], now, PURR_MT_BROADCAST, 0, "hello mesh");
    CHECK(id != 0);
    run(20000);
    CHECK_EQ(texts[1], 1);                                              /* B heard it */
    CHECK_EQ(texts[2], 1);                                              /* C heard it, by way of B */
    CHECK_EQ(texts[0], 0);                                              /* A does not get its own text back */
    CHECK(strcmp(last_text[2], "hello mesh") == 0 && last_text_from[2] == 0x1111);
    CHECK_EQ(heard[0], 1);                                              /* A heard B pass it on: the implicit acknowledgement */
    CHECK(tx_count[0] == 1);                                            /* A sent it once, and not again after hearing it back */
    CHECK(tx_count[1] == 1);                                            /* B passed it on once */
    CHECK(tx_count[2] <= 1);                                            /* C may pass it on once more, never loops */
}

static void test_direct_and_ack(void)
{
    reset();
    add_node(0x1111, "Alpha", &K1, 1);
    add_node(0x2222, "Bravo", &K1, 1);
    add_node(0x3333, "Charlie", &K1, 1);
    line(0, 1);
    line(1, 2);
    run(40000);
    memset(texts, 0, sizeof(texts));
    uint32_t id = purr_mt_node_send_text(&node[0], now, 0x3333, 0, "for charlie");
    CHECK(id != 0);
    run(30000);
    CHECK_EQ(texts[2], 1);
    CHECK(strcmp(last_text[2], "for charlie") == 0);
    CHECK_EQ(texts[1], 0);                                              /* B carried it but it was not for B */
    CHECK_EQ(delivered[0], 1);                                          /* the acknowledgement came back across two hops */
    CHECK_EQ(failed[0], 0);
    run(120000);
    CHECK_EQ(texts[2], 1);                                              /* and nothing was sent again after it was acknowledged */
    CHECK_EQ(delivered[0], 1);
}

static void test_retry_and_failure(void)
{
    reset();
    add_node(0x1111, "Alpha", &K1, 1);
    add_node(0x2222, "Bravo", &K1, 1);
    line(0, 1);
    run(40000);
    memset(tx_count, 0, sizeof(tx_count));
    drop_all = 1;                                                       /* the channel dies */
    uint32_t id = purr_mt_node_send_text(&node[0], now, 0x2222, 0, "anyone there");
    CHECK(id != 0);
    run(180000);
    CHECK_EQ(failed[0], 1);                                             /* it was tried again, then given up on, and the person is told */
    CHECK(tx_count[0] >= 4);                                            /* the first send and three more */
    CHECK_EQ(texts[1], 0);

    /* the channel comes back before the last retry: delivered once, shown once */
    reset();
    add_node(0x1111, "Alpha", &K1, 1);
    add_node(0x2222, "Bravo", &K1, 1);
    line(0, 1);
    run(40000);
    drop_all = 1;
    id = purr_mt_node_send_text(&node[0], now, 0x2222, 0, "second try");
    run(9000);
    drop_all = 0;
    run(120000);
    CHECK_EQ(texts[1], 1);
    CHECK_EQ(delivered[0], 1);
    CHECK_EQ(failed[0], 0);
}

static void test_foreign_channel(void)
{
    reset();
    static const uint8_t other[16] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6};
    add_node(0x1111, "Alpha", &K1, 1);
    add_node(0x2222, "Bravo", other, 16);                               /* a node on a private channel in the middle */
    add_node(0x3333, "Charlie", &K1, 1);
    line(0, 1);
    line(1, 2);
    run(5000);
    memset(texts, 0, sizeof(texts));
    purr_mt_node_send_text(&node[0], now, PURR_MT_BROADCAST, 0, "public");
    run(20000);
    CHECK_EQ(texts[1], 0);                                              /* B cannot read it ... */
    CHECK_EQ(texts[2], 1);                                              /* ... but passed it on, and C could */
}

static void test_nodeinfo_and_range(void)
{
    reset();
    for (int i = 0; i < 6; i++) {
        char nm[8];
        snprintf(nm, sizeof(nm), "Node%d", i);
        add_node(0x100u + (uint32_t)i, nm, &K1, 1);
    }
    for (int i = 0; i < 5; i++) {
        line(i, i + 1);
    }
    run(5000);
    memset(texts, 0, sizeof(texts));
    purr_mt_node_send_text(&node[0], now, PURR_MT_BROADCAST, 0, "far");
    run(30000);
    CHECK_EQ(texts[1], 1);
    CHECK_EQ(texts[3], 1);
    CHECK_EQ(texts[4], 1);                                              /* three hops of passing on: the fourth node hears it */
    CHECK_EQ(texts[5], 0);                                              /* and the fifth does not */
    run(70000);
    const purr_mt_nodeinfo_t *n = purr_mt_node_find(&node[0], 0x101);
    CHECK(n != NULL && strcmp(n->long_name, "Node1") == 0 && n->hops_away == 0);
    n = purr_mt_node_find(&node[0], 0x102);
    CHECK(n != NULL && strcmp(n->short_name, "Node") == 0);             /* learned from its announcement, passed along by Node1 */
    CHECK(n != NULL && n->hops_away >= 1);
    CHECK(purr_mt_node_count(&node[0]) >= 3);
}

static void test_sealed_direct(void)
{
    reset();
    make_keys = 1;
    add_node(0x1111, "Alpha", &K1, 1);
    add_node(0x2222, "Bravo", &K1, 1);
    add_node(0x3333, "Charlie", &K1, 1);
    line(0, 1);
    line(1, 2);
    run(100000);                                                        /* they announce themselves: the keys are exchanged in the node info */
    const purr_mt_nodeinfo_t *c = purr_mt_node_find(&node[0], 0x3333);
    CHECK(c != NULL && c->has_key && memcmp(c->public_key, node[2].cfg.pub, 32) == 0);
    memset(texts, 0, sizeof(texts));
    cur = 0;
    uint32_t id = purr_mt_node_send_text(&node[0], now, 0x3333, 0, "sealed for charlie");
    CHECK(id != 0);
    run(30000);
    CHECK_EQ(texts[2], 1);
    CHECK(strcmp(last_text[2], "sealed for charlie") == 0);
    CHECK_EQ(pkc_seen[2], 1);                                           /* it arrived sealed */
    CHECK_EQ(texts[1], 0);                                              /* B carried it and could not read it (the channel key does not open it) */
    CHECK_EQ(delivered[0], 1);                                          /* and the acknowledgement came back, sealed as well */

    /* the same with sealing off: it goes out under the channel key, as older firmware does, and is still delivered */
    node[0].cfg.pkc_send = 0;
    memset(texts, 0, sizeof(texts));
    memset(pkc_seen, 0, sizeof(pkc_seen));
    delivered[0] = 0;
    cur = 0;
    id = purr_mt_node_send_text(&node[0], now, 0x3333, 0, "plain for charlie");
    run(30000);
    CHECK_EQ(texts[2], 1);
    CHECK_EQ(pkc_seen[2], 0);
    CHECK_EQ(delivered[0], 1);

    /* a node with no key for the other falls back to the channel key even when sealing is on */
    reset();
    make_keys = 1;
    add_node(0x1111, "Alpha", &K1, 1);
    add_node(0x2222, "Bravo", &K1, 1);
    line(0, 1);
    node[0].cfg.pkc_send = 1;
    cur = 0;
    purr_mt_node_send_text(&node[0], now, 0x2222, 0, "no key known yet");
    run(10000);
    CHECK_EQ(texts[1], 1);
    CHECK_EQ(pkc_seen[1], 0);
}

static void test_rules(void)
{
    reset();
    add_node(0x1111, "Alpha", &K1, 1);
    cur = 0;
    CHECK_EQ(purr_mt_node_send_text(&node[0], now, PURR_MT_BROADCAST, 0, ""), 0);
    char big[300];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    CHECK_EQ(purr_mt_node_send_text(&node[0], now, PURR_MT_BROADCAST, 0, big), 0);          /* over the limit */
    CHECK_EQ(purr_mt_node_send_text(&node[0], now, PURR_MT_BROADCAST, 5, "no such channel"), 0);
    uint8_t garbage[40] = {1, 2, 3};
    purr_mt_node_rx(&node[0], now, garbage, sizeof(garbage), -100, 0);
    purr_mt_node_rx(&node[0], now, garbage, 5, -100, 0);                                    /* a short frame */
    purr_mt_event_t e;
    CHECK_EQ(purr_mt_node_event(&node[0], &e), 0);
    /* airtime of a 40 byte LongFast frame is about a third of a second to a second: the Semtech calculator gives 0.37 s for 20 bytes at SF11 BW250 */
    uint32_t t = purr_mt_airtime_ms(purr_mt_preset_by_name("LongFast"), 36);
    CHECK(t > 300 && t < 1500);
    CHECK(purr_mt_airtime_ms(purr_mt_preset_by_name("LongSlow"), 36) > 2 * t);
}

int main(void)
{
    test_broadcast();
    test_direct_and_ack();
    test_retry_and_failure();
    test_foreign_channel();
    test_nodeinfo_and_range();
    test_sealed_direct();
    test_rules();
    TK_DONE("test_mt_node");
}
