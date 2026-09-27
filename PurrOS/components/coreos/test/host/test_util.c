#include <string.h>

#include "purr_util.h"
#include "testkit.h"

static void hex(const uint8_t *p, int n, char *out)
{
    static const char *d = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[2 * i] = d[p[i] >> 4];
        out[2 * i + 1] = d[p[i] & 15];
    }
    out[2 * n] = '\0';
}

static void test_crc(void)
{
    CHECK_EQ(purr_crc32(0, "123456789", 9), 0xCBF43926u);   /* the standard check value */
    CHECK_EQ(purr_crc32(0, "", 0), 0);
    /* Chained calls equal one call. */
    uint32_t c = purr_crc32(0, "1234", 4);
    c = purr_crc32(c, "56789", 5);
    CHECK_EQ(c, 0xCBF43926u);
}

static void test_sha256(void)
{
    uint8_t d[32];
    char h[65];

    purr_sha256("", 0, d);
    hex(d, 32, h);
    CHECK(strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);

    purr_sha256("abc", 3, d);
    hex(d, 32, h);
    CHECK(strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    purr_sha256(two, strlen(two), d);
    hex(d, 32, h);
    CHECK(strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);

    /* One million 'a', fed in odd-sized pieces, crosses many block boundaries. */
    purr_sha256_t ctx;
    char piece[997];
    memset(piece, 'a', sizeof(piece));
    purr_sha256_init(&ctx);
    long left = 1000000;
    while (left > 0) {
        long n = left < (long)sizeof(piece) ? left : (long)sizeof(piece);
        purr_sha256_update(&ctx, piece, (size_t)n);
        left -= n;
    }
    purr_sha256_final(&ctx, d);
    hex(d, 32, h);
    CHECK(strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
}

static void test_version(void)
{
    uint32_t v;

    CHECK_EQ(purr_version_parse("1.2.3", 12, &v), 0);
    CHECK_EQ(v, PURR_VERSION(1, 2, 3));
    CHECK_EQ(purr_version_parse("1.0.0-dp10", 12, &v), 0);
    CHECK_EQ(v, PURR_VERSION(1, 0, 0));
    CHECK_EQ(purr_version_parse("2", 12, &v), 0);
    CHECK_EQ(v, PURR_VERSION(2, 0, 0));
    CHECK_EQ(purr_version_parse("2.5", 12, &v), 0);
    CHECK_EQ(v, PURR_VERSION(2, 5, 0));
    CHECK_EQ(purr_version_parse("10.20.30+build", 16, &v), 0);
    CHECK_EQ(v, PURR_VERSION(10, 20, 30));

    CHECK(purr_version_parse("", 12, &v) != 0);
    CHECK(purr_version_parse("abc", 12, &v) != 0);
    CHECK(purr_version_parse("1.x", 12, &v) != 0);
    CHECK(purr_version_parse("1.2.3x", 12, &v) != 0);
    CHECK(purr_version_parse("256.0.0", 12, &v) != 0);
    CHECK(purr_version_parse(NULL, 12, &v) != 0);

    /* A field that is not NUL-terminated is cut at max_len. */
    char raw[5] = {'1', '.', '2', '.', '9'};     /* five bytes, no terminator */
    CHECK_EQ(purr_version_parse(raw, 5, &v), 0);
    CHECK_EQ(v, PURR_VERSION(1, 2, 9));
    CHECK_EQ(purr_version_parse(raw, 3, &v), 0);  /* cut at max_len: "1.2" */
    CHECK_EQ(v, PURR_VERSION(1, 2, 0));

    /* Ordering follows the packed value. */
    uint32_t a, b;
    purr_version_parse("1.9.0", 12, &a);
    purr_version_parse("1.10.0", 12, &b);
    CHECK(a < b);
}

int main(void)
{
    test_crc();
    test_sha256();
    test_version();
    TK_DONE("test_util");
}
