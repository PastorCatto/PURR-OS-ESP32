/* The Meshtastic core (Modules/meshtastic/purr_mt.c) against the AES standard's vectors and the values a default device is known to use. */
#include <stdio.h>
#include <string.h>

#include "testkit.h"
#include "purr_mt.h"

static void hex(const char *s, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static void test_aes(void)
{
    uint8_t key[32], in[16], want[16], out[16];
    purr_mt_aes_t a;
    hex("000102030405060708090a0b0c0d0e0f", key, 16);                   /* FIPS 197 appendix C */
    hex("00112233445566778899aabbccddeeff", in, 16);
    CHECK(purr_mt_aes_init(&a, key, 16) == 0);
    purr_mt_aes_block(&a, in, out);
    hex("69c4e0d86a7b0430d8cdb78070b4c55a", want, 16);
    CHECK(memcmp(out, want, 16) == 0);
    hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key, 32);
    CHECK(purr_mt_aes_init(&a, key, 32) == 0);
    purr_mt_aes_block(&a, in, out);
    hex("8ea2b7ca516745bfeafc49904b496089", want, 16);
    CHECK(memcmp(out, want, 16) == 0);
    CHECK(purr_mt_aes_init(&a, key, 24) == -1);

    /* SP 800-38A F.5.1, CTR-AES128: the counter block runs from ...fcfdfeff through a carry into the next byte */
    uint8_t iv[16], pt[64], ct[64];
    hex("2b7e151628aed2a6abf7158809cf4f3c", key, 16);
    hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", iv, 16);
    hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710", pt, 64);
    hex("874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee", ct, 64);
    CHECK(purr_mt_aes_init(&a, key, 16) == 0);
    uint8_t buf[64];
    memcpy(buf, pt, 64);
    purr_mt_ctr(&a, iv, buf, 64);
    CHECK(memcmp(buf, ct, 64) == 0);
    purr_mt_ctr(&a, iv, buf, 64);                                       /* the same operation undoes it */
    CHECK(memcmp(buf, pt, 64) == 0);
    memcpy(buf, pt, 37);
    purr_mt_ctr(&a, iv, buf, 37);                                       /* a length that is not a whole block */
    CHECK(memcmp(buf, ct, 37) == 0);
}

static void test_channels(void)
{
    uint8_t key[32];
    CHECK_EQ(purr_mt_expand_psk(NULL, 0, key), 0);
    uint8_t one = 1, two = 2, zero = 0;
    CHECK_EQ(purr_mt_expand_psk(&one, 1, key), 16);
    CHECK(memcmp(key, purr_mt_default_psk, 16) == 0);                   /* "AQ==" is the default key itself */
    CHECK_EQ(purr_mt_expand_psk(&two, 1, key), 16);
    CHECK(memcmp(key, purr_mt_default_psk, 15) == 0 && key[15] == 0x02);
    CHECK_EQ(purr_mt_expand_psk(&zero, 1, key), 0);
    uint8_t k5[5] = {1, 2, 3, 4, 5};
    CHECK_EQ(purr_mt_expand_psk(k5, 5, key), 16);
    CHECK(key[4] == 5 && key[5] == 0 && key[15] == 0);
    uint8_t k20[20];
    memset(k20, 7, 20);
    CHECK_EQ(purr_mt_expand_psk(k20, 20, key), 32);
    CHECK_EQ(purr_mt_expand_psk(k20, 33, key), -1);

    /* the default channel's hash: a default device puts 8 in the header of everything it sends on LongFast (VERIFY on air) */
    CHECK_EQ(purr_mt_expand_psk(&one, 1, key), 16);
    CHECK_EQ(purr_mt_channel_hash("LongFast", key, 16), 8);

    /* encrypt and decrypt are the same operation, and depend on the sender and the id */
    uint8_t msg[40], orig[40];
    memset(msg, 0x41, sizeof(msg));
    memcpy(orig, msg, sizeof(msg));
    purr_mt_crypt(key, 16, 0x11223344, 0xAABBCCDD, msg, sizeof(msg));
    CHECK(memcmp(msg, orig, sizeof(msg)) != 0);
    uint8_t other[40];
    memcpy(other, orig, sizeof(other));
    purr_mt_crypt(key, 16, 0x11223344, 0xAABBCCDE, other, sizeof(other));
    CHECK(memcmp(msg, other, sizeof(msg)) != 0);
    purr_mt_crypt(key, 16, 0x11223344, 0xAABBCCDD, msg, sizeof(msg));
    CHECK(memcmp(msg, orig, sizeof(msg)) == 0);
    memcpy(other, orig, sizeof(other));
    purr_mt_crypt(key, 0, 1, 2, other, sizeof(other));                  /* no key: left alone */
    CHECK(memcmp(other, orig, sizeof(other)) == 0);
}

static void test_header(void)
{
    purr_mt_hdr_t h = {.to = PURR_MT_BROADCAST, .from = 0x0A1B2C3D, .id = 0x01020304, .hop_limit = 3, .want_ack = 1, .via_mqtt = 0, .hop_start = 3, .channel = 8, .next_hop = 0x55, .relay_node = 0x3D};
    uint8_t b[PURR_MT_HDR_LEN];
    CHECK_EQ(purr_mt_hdr_put(&h, b), 16);
    CHECK(b[0] == 0xFF && b[4] == 0x3D && b[5] == 0x2C && b[8] == 0x04 && b[11] == 0x01);     /* little-endian fields */
    CHECK_EQ(b[12], 3 | (1 << 3) | (3 << 5));
    purr_mt_hdr_t g;
    CHECK_EQ(purr_mt_hdr_get(b, 16, &g), 16);
    CHECK(g.to == h.to && g.from == h.from && g.id == h.id && g.hop_limit == 3 && g.want_ack == 1 && g.hop_start == 3 && g.channel == 8 && g.next_hop == 0x55 && g.relay_node == 0x3D);
    CHECK_EQ(purr_mt_hdr_get(b, 15, &g), -1);
}

static void test_pb(void)
{
    uint8_t b[64];
    purr_mt_data_t d = {.portnum = PURR_MT_PORT_TEXT, .payload = (const uint8_t *)"hi", .payload_len = 2};
    int n = purr_mt_data_put(&d, b, sizeof(b));
    CHECK(n == 6 && b[0] == 0x08 && b[1] == 0x01 && b[2] == 0x12 && b[3] == 0x02 && b[4] == 'h' && b[5] == 'i');
    purr_mt_data_t g;
    CHECK_EQ(purr_mt_data_get(b, (size_t)n, &g), 0);
    CHECK(g.portnum == 1 && g.payload_len == 2 && memcmp(g.payload, "hi", 2) == 0 && g.dest == 0);
    d.dest = 0x12345678;
    d.source = 0x0BADF00D;
    d.request_id = 77;
    d.want_response = 1;
    d.has_bitfield = 1;
    d.bitfield = 1;
    n = purr_mt_data_put(&d, b, sizeof(b));
    CHECK(n > 0);
    CHECK_EQ(purr_mt_data_get(b, (size_t)n, &g), 0);
    CHECK(g.dest == 0x12345678 && g.source == 0x0BADF00D && g.request_id == 77 && g.want_response == 1 && g.has_bitfield && g.bitfield == 1);
    CHECK_EQ(purr_mt_data_put(&d, b, 5), -1);                           /* does not fit */
    b[0] = 0x0A;                                                        /* a field with a bad wire format */
    b[1] = 0xFF;
    CHECK_EQ(purr_mt_data_get(b, 2, &g), -1);

    uint32_t err = 9;
    n = purr_mt_routing_put(0, b, sizeof(b));
    CHECK(n == 2 && b[0] == 0x18 && b[1] == 0x00);                      /* an acknowledgement */
    CHECK_EQ(purr_mt_routing_get(b, 2, &err), 0);
    CHECK_EQ(err, 0);
    n = purr_mt_routing_put(5, b, sizeof(b));
    CHECK_EQ(purr_mt_routing_get(b, (size_t)n, &err), 0);
    CHECK_EQ(err, 5);

    purr_mt_user_t u, v;
    memset(&u, 0, sizeof(u));
    strcpy(u.id, "!0a1b2c3d");
    strcpy(u.long_name, "PURR T-Deck");
    strcpy(u.short_name, "PURR");
    u.hw_model = 50;
    u.role = 0;
    memset(u.public_key, 0xAB, 32);
    u.has_key = 1;
    n = purr_mt_user_put(&u, b, sizeof(b) < 80 ? sizeof(b) : 64);
    CHECK(n < 0);                                                       /* 64 bytes is not enough with a key: the failure is reported, not truncated */
    uint8_t big[128];
    n = purr_mt_user_put(&u, big, sizeof(big));
    CHECK(n > 0);
    CHECK_EQ(purr_mt_user_get(big, (size_t)n, &v), 0);
    CHECK(strcmp(v.id, "!0a1b2c3d") == 0 && strcmp(v.long_name, "PURR T-Deck") == 0 && strcmp(v.short_name, "PURR") == 0 && v.hw_model == 50 && v.has_key && v.public_key[31] == 0xAB);

    purr_mt_pos_t p = {.lat_e7 = -337000000, .lon_e7 = 1511000000, .alt_m = -12, .has_alt = 1, .time = 1700000000u}, q;
    n = purr_mt_pos_put(&p, b, sizeof(b));
    CHECK(n > 0);
    CHECK_EQ(purr_mt_pos_get(b, (size_t)n, &q), 0);
    CHECK(q.lat_e7 == p.lat_e7 && q.lon_e7 == p.lon_e7 && q.alt_m == -12 && q.has_alt && q.time == p.time);
}

static void test_radio(void)
{
    const purr_mt_region_t *us = purr_mt_region_by_name("US");
    const purr_mt_preset_t *lf = purr_mt_preset_by_name("LongFast");
    CHECK(us != NULL && lf != NULL);
    CHECK(purr_mt_region_by_name("Nowhere") == NULL && purr_mt_preset_by_name("Nope") == NULL);
    CHECK(lf->sf == 11 && lf->bw_hz == 250000 && lf->cr == 5 && lf->preamble == 16);
    int slot = purr_mt_slot(us, lf, "LongFast", 0);
    CHECK_EQ(slot, 19);                                                 /* the default device's channel on the US band (VERIFY on air) */
    CHECK_EQ(purr_mt_freq_hz(us, lf, slot), 906875000u);
    CHECK_EQ(purr_mt_slot(us, lf, "", 0), 19);                          /* an unnamed channel takes the preset's name */
    CHECK_EQ(purr_mt_slot(us, lf, "LongFast", 1), 0);
    CHECK_EQ(purr_mt_freq_hz(us, lf, 0), 902125000u);
    CHECK_EQ(purr_mt_slot(us, lf, "LongFast", 105), 0);                 /* a slot past the end wraps */
    const purr_mt_region_t *eu = purr_mt_region_by_name("EU_868");
    CHECK_EQ(purr_mt_slot(eu, lf, "LongFast", 0), 0);                   /* a band with room for one channel of that width */
    CHECK(purr_mt_preset_count() >= 8 && purr_mt_region_count() >= 10);
}

static void test_url(void)
{
    purr_mt_url_t u;
    /* the default channel's link: one channel with the one byte key 1 and no name (LongFast) */
    CHECK_EQ(purr_mt_url_parse("https://meshtastic.org/e/#CgMSAQE", &u), 0);
    CHECK(u.count == 1 && u.ch[0].psk_len == 1 && u.ch[0].psk[0] == 1 && u.ch[0].name[0] == '\0');
    CHECK_EQ(purr_mt_url_parse("CgMSAQE", &u), 0);                      /* only what follows the # */
    CHECK_EQ(purr_mt_url_parse("CgMSAQE=", &u), 0);                     /* padded, as some apps write it */
    CHECK_EQ(purr_mt_url_parse("https://meshtastic.org/e/#", &u), -1);
    CHECK_EQ(purr_mt_url_parse("https://meshtastic.org/e/#!!!", &u), -1);

    /* made and read back: two channels (a private group with a 32 byte key) and the radio settings */
    purr_mt_url_t a, b;
    memset(&a, 0, sizeof(a));
    strcpy(a.ch[0].name, "LongFast");
    a.ch[0].psk[0] = 1;
    a.ch[0].psk_len = 1;
    strcpy(a.ch[1].name, "Cats");
    for (int i = 0; i < 32; i++) a.ch[1].psk[i] = (uint8_t)(i * 5 + 1);
    a.ch[1].psk_len = 32;
    a.count = 2;
    strcpy(a.region, "US");
    strcpy(a.preset, "LongFast");
    char link[300];
    int n = purr_mt_url_make(&a, link, sizeof(link));
    CHECK(n > 40 && strncmp(link, "https://meshtastic.org/e/#", 26) == 0);
    CHECK_EQ(purr_mt_url_parse(link, &b), 0);
    CHECK(b.count == 2 && strcmp(b.ch[1].name, "Cats") == 0 && b.ch[1].psk_len == 32 && memcmp(b.ch[1].psk, a.ch[1].psk, 32) == 0);
    CHECK(strcmp(b.region, "US") == 0 && strcmp(b.preset, "LongFast") == 0);
    CHECK_EQ(purr_mt_url_make(&a, link, 20), -1);                       /* too small */
    strcpy(a.region, "ANZ");
    strcpy(a.preset, "MediumFast");
    purr_mt_url_make(&a, link, sizeof(link));
    purr_mt_url_parse(link, &b);
    CHECK(strcmp(b.region, "ANZ") == 0 && strcmp(b.preset, "MediumFast") == 0);
}

int main(void)
{
    test_url();
    test_aes();
    test_channels();
    test_header();
    test_pb();
    test_radio();
    TK_DONE("test_mt");
}
