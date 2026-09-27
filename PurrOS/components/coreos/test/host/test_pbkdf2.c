#include <string.h>

#include "purr_pbkdf2.h"
#include "testkit.h"

/* RFC 7914-style PBKDF2-HMAC-SHA256 vectors (cross-checked against Python's
 * hashlib.pbkdf2_hmac, since these are the widely-used reference values but not
 * transcribed from memory here). */
static void expect_hex(const uint8_t *got, const char *hex)
{
    for (int i = 0; i < 32; i++) {
        char hi = hex[i * 2], lo = hex[i * 2 + 1];
        int hv = (hi <= '9') ? hi - '0' : (hi | 0x20) - 'a' + 10;
        int lv = (lo <= '9') ? lo - '0' : (lo | 0x20) - 'a' + 10;
        CHECK_EQ(got[i], (uint8_t)((hv << 4) | lv));
    }
}

static void test_pbkdf2_vectors(void)
{
    uint8_t out[32];

    purr_pbkdf2_hmac_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 1, out);
    expect_hex(out, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");

    purr_pbkdf2_hmac_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 2, out);
    expect_hex(out, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");

    purr_pbkdf2_hmac_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 4096, out);
    expect_hex(out, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");

    purr_pbkdf2_hmac_sha256((const uint8_t *)"passwordPASSWORDpassword", 24, (const uint8_t *)"saltSALTsaltSALTsaltSALTsaltSALTsalt", 36, 4096, out);
    expect_hex(out, "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1");

    purr_pbkdf2_hmac_sha256((const uint8_t *)"", 0, (const uint8_t *)"salt", 4, 1, out);
    expect_hex(out, "f135c27993baf98773c5cdb40a5706ce6a345cde61b000a67858650cd6a324d7");

    purr_pbkdf2_hmac_sha256((const uint8_t *)"password", 8, (const uint8_t *)"", 0, 1, out);
    expect_hex(out, "c1232f10f62715fda06ae7c0a2037ca19b33cf103b727ba56d870c11f290a2ab");
}

static void test_different_inputs_differ(void)
{
    uint8_t a[32], b[32];
    purr_pbkdf2_hmac_sha256((const uint8_t *)"hunter2", 7, (const uint8_t *)"saltsalt", 8, 100, a);
    purr_pbkdf2_hmac_sha256((const uint8_t *)"hunter3", 7, (const uint8_t *)"saltsalt", 8, 100, b);
    CHECK(memcmp(a, b, 32) != 0);

    purr_pbkdf2_hmac_sha256((const uint8_t *)"hunter2", 7, (const uint8_t *)"saltsalt", 8, 100, a);
    purr_pbkdf2_hmac_sha256((const uint8_t *)"hunter2", 7, (const uint8_t *)"pepperpp", 8, 100, b);
    CHECK(memcmp(a, b, 32) != 0);

    purr_pbkdf2_hmac_sha256((const uint8_t *)"hunter2", 7, (const uint8_t *)"saltsalt", 8, 100, a);
    purr_pbkdf2_hmac_sha256((const uint8_t *)"hunter2", 7, (const uint8_t *)"saltsalt", 8, 101, b);
    CHECK(memcmp(a, b, 32) != 0);
}

static void test_deterministic(void)
{
    uint8_t a[32], b[32];
    purr_pbkdf2_hmac_sha256((const uint8_t *)"same", 4, (const uint8_t *)"seed", 4, 50, a);
    purr_pbkdf2_hmac_sha256((const uint8_t *)"same", 4, (const uint8_t *)"seed", 4, 50, b);
    CHECK(memcmp(a, b, 32) == 0);
}

static void test_hmac_key_longer_than_block(void)
{
    /* HMAC's own edge case: a key longer than the block size is hashed down first.
     * Not a published vector, just checking it does not crash and is deterministic. */
    uint8_t key[100], out1[32], out2[32];
    for (int i = 0; i < 100; i++) {
        key[i] = (uint8_t)i;
    }
    purr_hmac_sha256(key, sizeof(key), (const uint8_t *)"data", 4, out1);
    purr_hmac_sha256(key, sizeof(key), (const uint8_t *)"data", 4, out2);
    CHECK(memcmp(out1, out2, 32) == 0);
}

int main(void)
{
    test_pbkdf2_vectors();
    test_different_inputs_differ();
    test_deterministic();
    test_hmac_key_longer_than_block();
    TK_DONE("test_pbkdf2");
}
