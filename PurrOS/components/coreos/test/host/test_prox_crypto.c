/* The link's symmetric primitives (Modules/proximity/purr_px_crypto.h) against the published test vectors: FIPS 180-4, RFC 4231, RFC 5869 and RFC 8439. */
#include <stdio.h>
#include <string.h>

#include "purr_px_crypto.h"
#include "testkit.h"

static void hex(const char *h, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static int same(const uint8_t *a, const char *h, int n)
{
    uint8_t b[512];
    hex(h, b, n);
    return memcmp(a, b, (size_t)n) == 0;
}

static void test_sha256(void)
{
    uint8_t d[32];
    purr_px_sha256("abc", 3, d);
    CHECK(same(d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 32));
    purr_px_sha256("", 0, d);
    CHECK(same(d, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", 32));
    purr_px_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
    CHECK(same(d, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", 32));
    uint8_t big[1000];                                            /* one million 'a', in pieces of a thousand */
    memset(big, 'a', sizeof(big));
    purr_px_sha256_t c;
    purr_px_sha256_init(&c);
    for (int i = 0; i < 1000; i++) {
        purr_px_sha256_update(&c, big, sizeof(big));
    }
    purr_px_sha256_final(&c, d);
    CHECK(same(d, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", 32));
    purr_px_sha256_init(&c);                                         /* the same bytes in odd pieces give the same digest */
    const char *m = "The quick brown fox jumps over the lazy dog";
    for (int i = 0; m[i] != '\0'; i += 5) {
        purr_px_sha256_update(&c, m + i, strlen(m + i) < 5 ? strlen(m + i) : 5);
    }
    purr_px_sha256_final(&c, d);
    CHECK(same(d, "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592", 32));
}

static void test_hmac_hkdf(void)
{
    uint8_t d[32], key[131];
    memset(key, 0x0b, 20);
    purr_px_hmac_sha256(key, 20, "Hi There", 8, d);                  /* RFC 4231, case 1 */
    CHECK(same(d, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 32));
    purr_px_hmac_sha256((const uint8_t *)"Jefe", 4, "what do ya want for nothing?", 28, d);   /* case 2 */
    CHECK(same(d, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", 32));
    memset(key, 0xaa, 131);                                       /* case 6: a key longer than a block */
    purr_px_hmac_sha256(key, 131, "Test Using Larger Than Block-Size Key - Hash Key First", 54, d);
    CHECK(same(d, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", 32));

    uint8_t ikm[22], salt[13], info[10], okm[42];                 /* RFC 5869, case 1 */
    memset(ikm, 0x0b, 22);
    hex("000102030405060708090a0b0c", salt, 13);
    hex("f0f1f2f3f4f5f6f7f8f9", info, 10);
    purr_px_hkdf_sha256(salt, 13, ikm, 22, info, 10, okm, 42);
    CHECK(same(okm, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", 42));
    uint8_t okm2[42];                                             /* case 3: no salt, no info */
    purr_px_hkdf_sha256(NULL, 0, ikm, 22, NULL, 0, okm2, 42);
    CHECK(same(okm2, "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8", 42));
}

static void test_chacha_poly(void)
{
    uint8_t key[32], nonce[12], out[512];
    for (int i = 0; i < 32; i++) {
        key[i] = (uint8_t)i;
    }
    hex("000000090000004a00000000", nonce, 12);                   /* RFC 8439 section 2.3.2: block counter 1 */
    uint8_t zeros[64] = {0};
    purr_px_chacha20_xor(key, nonce, 1, zeros, out, 64);
    CHECK(same(out, "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4ed2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e", 64));

    uint8_t pk[32], tag[16];                                      /* section 2.5.2 */
    hex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b", pk, 32);
    purr_px_poly1305(pk, (const uint8_t *)"Cryptographic Forum Research Group", 34, tag);
    CHECK(same(tag, "a8061dc1305136c6c22b8baf0c0127a9", 16));

    const char *pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";   /* section 2.8.2 */
    uint8_t aad[12], k2[32], n2[12], sealed[256], back[256];
    hex("50515253c0c1c2c3c4c5c6c7", aad, 12);
    hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", k2, 32);
    hex("070000004041424344454647", n2, 12);
    purr_px_aead_seal(k2, n2, aad, 12, (const uint8_t *)pt, strlen(pt), sealed);
    CHECK(same(sealed, "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116", (int)strlen(pt)));
    CHECK(same(sealed + strlen(pt), "1ae10b594f09e26a7e902ecbd0600691", 16));
    CHECK_EQ(purr_px_aead_open(k2, n2, aad, 12, sealed, strlen(pt) + 16, back), 0);
    CHECK(memcmp(back, pt, strlen(pt)) == 0);
}

static void test_aead_rejects(void)
{
    uint8_t key[32], nonce[12], sealed[200], back[200];
    memset(key, 7, 32);
    memset(nonce, 3, 12);
    const uint8_t aad[3] = {1, 2, 3};
    uint8_t msg[100];
    for (int i = 0; i < 100; i++) {
        msg[i] = (uint8_t)(i * 5);
    }
    for (int n = 0; n <= 100; n += 11) {                          /* every kind of length, including none */
        purr_px_aead_seal(key, nonce, aad, 3, msg, (size_t)n, sealed);
        memset(back, 0xEE, sizeof(back));
        CHECK_EQ(purr_px_aead_open(key, nonce, aad, 3, sealed, (size_t)n + 16, back), 0);
        CHECK(memcmp(back, msg, (size_t)n) == 0);
        for (int bit = 0; bit < (n + 16) * 8; bit += 13) {        /* any flipped bit in the ciphertext or the tag is refused, and nothing is written */
            sealed[bit / 8] ^= (uint8_t)(1 << (bit % 8));
            memset(back, 0xEE, sizeof(back));
            CHECK_EQ(purr_px_aead_open(key, nonce, aad, 3, sealed, (size_t)n + 16, back), -1);
            CHECK_EQ(back[0], 0xEE);
            sealed[bit / 8] ^= (uint8_t)(1 << (bit % 8));
        }
        const uint8_t aad2[3] = {1, 2, 4};                        /* another header is refused */
        CHECK_EQ(purr_px_aead_open(key, nonce, aad2, 3, sealed, (size_t)n + 16, back), -1);
        uint8_t n2[12];
        memcpy(n2, nonce, 12);
        n2[11] ^= 1;                                              /* another nonce is refused */
        CHECK_EQ(purr_px_aead_open(key, n2, aad, 3, sealed, (size_t)n + 16, back), -1);
    }
    CHECK_EQ(purr_px_aead_open(key, nonce, aad, 3, sealed, 15, back), -1);   /* shorter than a tag */
}

int main(void)
{
    test_sha256();
    test_hmac_hkdf();
    test_chacha_poly();
    test_aead_rejects();
    TK_DONE("test_prox_crypto");
}
