/* The public-key side of Meshtastic direct messages: X25519 against RFC 7748, AES-CCM against RFC 3610, and a sealed packet both ways. */
#include <stdio.h>
#include <string.h>

#include "testkit.h"
#include "purr_mt_pkc.h"

static void hex(const char *s, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static void test_x25519(void)
{
    uint8_t k[32], u[32], want[32], out[32];
    /* RFC 7748 section 5.2, first vector */
    hex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", k, 32);
    hex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u, 32);
    hex("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", want, 32);
    purr_mt_x25519(out, k, u);
    CHECK(memcmp(out, want, 32) == 0);
    /* section 6.1: Alice and Bob agree */
    uint8_t apriv[32], bpriv[32], apub[32], bpub[32], s1[32], s2[32], shared[32];
    hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", apriv, 32);
    hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", bpriv, 32);
    purr_mt_x25519(apub, apriv, NULL);
    purr_mt_x25519(bpub, bpriv, NULL);
    hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", want, 32);
    CHECK(memcmp(apub, want, 32) == 0);
    hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", want, 32);
    CHECK(memcmp(bpub, want, 32) == 0);
    purr_mt_x25519(s1, apriv, bpub);
    purr_mt_x25519(s2, bpriv, apub);
    hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", shared, 32);
    CHECK(memcmp(s1, shared, 32) == 0 && memcmp(s2, shared, 32) == 0);
}

static void test_ccm(void)
{
    /* RFC 3610 packet vector 1: M = 8, L = 2 */
    uint8_t key[16], nonce[13], aad[8], pt[23], want_ct[23], want_tag[8], ct[23], tag[8], back[23];
    hex("c0c1c2c3c4c5c6c7c8c9cacbcccdcecf", key, 16);
    hex("00000003020100a0a1a2a3a4a5", nonce, 13);
    hex("0001020304050607", aad, 8);
    hex("08090a0b0c0d0e0f101112131415161718191a1b1c1d1e", pt, 23);
    hex("588c979a61c663d2f066d0c2c0f989806d5f6b61dac384", want_ct, 23);
    hex("17e8d12cfdf926e0", want_tag, 8);
    CHECK_EQ(purr_mt_ccm_seal(key, 16, nonce, 8, aad, 8, pt, 23, ct, tag), 0);
    CHECK(memcmp(ct, want_ct, 23) == 0 && memcmp(tag, want_tag, 8) == 0);
    CHECK_EQ(purr_mt_ccm_open(key, 16, nonce, 8, aad, 8, ct, 23, tag, back), 0);
    CHECK(memcmp(back, pt, 23) == 0);
    tag[3] ^= 1;
    CHECK_EQ(purr_mt_ccm_open(key, 16, nonce, 8, aad, 8, ct, 23, tag, back), -1);       /* a wrong tag opens nothing */
    tag[3] ^= 1;
    ct[5] ^= 1;
    CHECK_EQ(purr_mt_ccm_open(key, 16, nonce, 8, aad, 8, ct, 23, tag, back), -1);
    CHECK_EQ(purr_mt_ccm_seal(key, 16, nonce, 7, aad, 8, pt, 23, ct, tag), -1);        /* an odd tag length */
}

static void test_packet(void)
{
    uint8_t ra[32], rb[32], apriv[32], apub[32], bpriv[32], bpub[32];
    for (int i = 0; i < 32; i++) {
        ra[i] = (uint8_t)(i * 7 + 1);
        rb[i] = (uint8_t)(i * 11 + 5);
    }
    purr_mt_pkc_keypair(ra, apriv, apub);
    purr_mt_pkc_keypair(rb, bpriv, bpub);
    const char *text = "a private hello";
    uint8_t out[64], back[64];
    int n = purr_mt_pkc_seal(apriv, bpub, 0xAAAA0001, 0x12345678, 0x0BADCAFE, (const uint8_t *)text, strlen(text), out);
    CHECK_EQ(n, (int)strlen(text) + PURR_MT_PKC_OVERHEAD);
    CHECK(out[n - 4] == 0xFE && out[n - 1] == 0x0B);                    /* the extra nonce rides at the end, little-endian */
    int m = purr_mt_pkc_open(bpriv, apub, 0xAAAA0001, 0x12345678, out, (size_t)n, back);
    CHECK(m == (int)strlen(text) && memcmp(back, text, (size_t)m) == 0);
    CHECK_EQ(purr_mt_pkc_open(bpriv, apub, 0xAAAA0002, 0x12345678, out, (size_t)n, back), -1);       /* another sender number */
    CHECK_EQ(purr_mt_pkc_open(bpriv, apub, 0xAAAA0001, 0x12345679, out, (size_t)n, back), -1);       /* another packet id */
    CHECK_EQ(purr_mt_pkc_open(apriv, apub, 0xAAAA0001, 0x12345678, out, (size_t)n, back), -1);       /* not the right pair of keys */
    uint8_t zero[32] = {0};
    CHECK_EQ(purr_mt_pkc_seal(apriv, zero, 1, 2, 3, (const uint8_t *)text, 5, out), -1);             /* a key of low order is refused */
    CHECK_EQ(purr_mt_pkc_open(bpriv, apub, 1, 2, out, 5, back), -1);                                 /* shorter than the overhead */
}

int main(void)
{
    test_x25519();
    test_ccm();
    test_packet();
    TK_DONE("test_mt_pkc");
}
