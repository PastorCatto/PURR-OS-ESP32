#include <stdlib.h>
#include <string.h>

#include "purr_util.h"
#include "purr_verify.h"
#include "testcrypto.h"
#include "testkit.h"

/* ---- test keys: id 0 boot, 1 system, 2 developer, 3 vendor, 4 boot again (other key) ---- */
enum { K_BOOT = 0, K_SYSTEM = 1, K_DEV = 2, K_VENDOR = 3, K_OTHER = 4, K_COUNT = 5 };
static uint8_t g_pub[K_COUNT][64], g_priv[K_COUNT][32];
static purr_keybag_t g_bag;
static const uint8_t g_role[K_COUNT] = {PURR_ROLE_BOOT, PURR_ROLE_SYSTEM, PURR_ROLE_DEVELOPER,
                                        PURR_ROLE_VENDOR, PURR_ROLE_BOOT};

static void setup_keys(void)
{
    purr_key_t defaults[K_COUNT];

    tc_seed(0xC0FFEE01u);
    for (int i = 0; i < K_COUNT; i++) {
        tc_make_key(g_pub[i], g_priv[i]);
        defaults[i].key_id = (uint8_t)i;
        defaults[i].role = g_role[i];
        memcpy(defaults[i].pub, g_pub[i], 64);
    }
    purr_keybag_build(&g_bag, defaults, K_COUNT, NULL);
}

/* ---- an in-memory image and its reader ---- */
typedef struct {
    uint8_t *data;
    uint32_t size;
    int fail_after;          /* fail every read once this many have happened, -1 = never */
    int reads;
} mem_t;

static int mem_read(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    mem_t *m = ctx;
    if (m->fail_after >= 0 && m->reads++ >= m->fail_after) {
        return -1;
    }
    if ((uint64_t)off + len > m->size) {
        return -1;
    }
    memcpy(buf, m->data + off, len);
    return 0;
}

typedef struct {
    uint16_t chip;
    uint8_t  type, key, subtype;
    const char *version, *min_boot;
    uint32_t payload_len;
} spec_t;

static spec_t good_os(void)
{
    spec_t s = {PURR_CHIP_ESP32S3, PURR_IMG_OS, K_BOOT, 0, "1.2.3", "1.0.0", 1500};
    return s;
}

/* Header, then payload, signed. Returns the image size. */
static uint32_t build(const spec_t *s, uint8_t *out)
{
    purr_image_header_t h;
    uint8_t digest[32];

    memset(&h, 0, sizeof(h));
    h.magic = PURR_IMAGE_MAGIC;
    h.header_version = PURR_IMAGE_HEADER_VERSION;
    h.header_size = sizeof(h);
    h.chip_id = s->chip;
    h.image_type = s->type;
    h.key_id = s->key;
    h.flags = s->subtype;
    strncpy(h.name, "test", sizeof(h.name));
    strncpy(h.version, s->version, sizeof(h.version));
    strncpy(h.min_boot_version, s->min_boot, sizeof(h.min_boot_version));
    h.payload_offset = sizeof(h);
    h.payload_size = s->payload_len;

    for (uint32_t i = 0; i < s->payload_len; i++) {
        out[sizeof(h) + i] = (uint8_t)(i * 31u + 7u);
    }
    purr_sha256(out + sizeof(h), s->payload_len, h.payload_sha256);
    purr_sha256(&h, PURR_IMAGE_SIGNED_LEN, digest);
    tc_sign(g_priv[s->key < K_COUNT ? s->key : 0], digest, h.signature);
    memcpy(out, &h, sizeof(h));
    return (uint32_t)sizeof(h) + s->payload_len;
}

static purr_verify_env_t env_for(uint16_t chip)
{
    purr_verify_env_t e = {chip, &g_bag, &purr_crypto_uecc, PURR_VERSION(1, 0, 0), 0, 0};
    return e;
}

/* Re-sign after editing the header, so a test can target one check. */
static void resign(uint8_t *img, int key)
{
    purr_image_header_t h;
    uint8_t digest[32];
    memcpy(&h, img, sizeof(h));
    purr_sha256(&h, PURR_IMAGE_SIGNED_LEN, digest);
    tc_sign(g_priv[key], digest, h.signature);
    memcpy(img, &h, sizeof(h));
}

#define VERIFY(env, buf, size, hdr) \
    purr_image_verify(&(env), mem_read, &(mem_t){(buf), (size), -1, 0}, (size), (hdr))

static uint8_t img[4096];

static void test_good_images(void)
{
    purr_verify_env_t e = env_for(PURR_CHIP_ESP32S3);
    purr_image_header_t h;

    spec_t os = good_os();
    uint32_t n = build(&os, img);
    CHECK_EQ(VERIFY(e, img, n, &h), PURR_V_OK);
    CHECK_EQ(h.image_type, PURR_IMG_OS);
    CHECK(strcmp(h.version, "1.2.3") == 0);

    spec_t rec = {PURR_CHIP_ESP32S3, PURR_IMG_RECOVERY, K_BOOT, 0, "1.0.0", "", 800};
    n = build(&rec, img);
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_OK);   /* no min_boot_version, no header out */

    spec_t am = {PURR_CHIP_ESP32S3, PURR_IMG_MODULE, K_SYSTEM, PURR_MOD_APPMANAGER, "2.0.0-dp10", "1.0.0", 300};
    n = build(&am, img);
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_OK);

    /* An app carries its own payload table, so its chip id is "any". */
    spec_t app = {PURR_CHIP_ANY, PURR_IMG_APP, K_DEV, 0, "0.1.0", "1.0.0", 64};
    n = build(&app, img);
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_OK);

    spec_t empty = {PURR_CHIP_ESP32S3, PURR_IMG_OS, K_BOOT, 0, "1.0.0", "1.0.0", 0};
    n = build(&empty, img);                          /* an empty payload is still valid */
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_OK);
}

static void test_header_checks(void)
{
    purr_verify_env_t e = env_for(PURR_CHIP_ESP32S3);
    spec_t os = good_os();
    uint32_t n = build(&os, img);
    purr_image_header_t h;

    /* bad magic */
    memcpy(&h, img, sizeof(h));
    h.magic ^= 1;
    memcpy(img, &h, sizeof(h));
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_BAD_MAGIC);

    /* bad header version */
    n = build(&os, img);
    memcpy(&h, img, sizeof(h));
    h.header_version = 2;
    memcpy(img, &h, sizeof(h));
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_BAD_VERSION);

    /* header_size smaller than the struct, or larger than the image */
    n = build(&os, img);
    memcpy(&h, img, sizeof(h));
    h.header_size = 10;
    memcpy(img, &h, sizeof(h));
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_BAD_LAYOUT);
    h.header_size = (uint16_t)(n + 1);
    memcpy(img, &h, sizeof(h));
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_BAD_LAYOUT);

    /* an image shorter than the header */
    n = build(&os, img);
    CHECK_EQ(VERIFY(e, img, 100, NULL), PURR_V_BAD_LAYOUT);
    CHECK_EQ(VERIFY(e, img, 0, NULL), PURR_V_BAD_LAYOUT);

    /* type out of range */
    for (int t = 0; t < 8; t++) {
        spec_t s = os;
        s.type = (uint8_t)t;
        n = build(&s, img);
        if (t >= 1 && t <= 4) {
            continue;
        }
        CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_BAD_TYPE);
    }

    /* wrong chip; "any" is only for apps */
    spec_t s = os;
    s.chip = PURR_CHIP_ESP32;
    n = build(&s, img);
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_BAD_CHIP);
    s.chip = PURR_CHIP_ANY;
    n = build(&s, img);
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_BAD_CHIP);
}

static void test_versions(void)
{
    purr_verify_env_t e = env_for(PURR_CHIP_ESP32S3);
    spec_t s = good_os();
    uint32_t n;

    /* the image needs a newer bootloader than this one */
    s.min_boot = "1.1.0";
    n = build(&s, img);
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_TOO_OLD_BOOTLOADER);
    e.bootloader_version = PURR_VERSION(1, 1, 0);
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_OK);

    /* a version string that is not a version */
    s = good_os();
    s.version = "banana";
    n = build(&s, img);
    purr_verify_env_t fresh = env_for(PURR_CHIP_ESP32S3);
    CHECK_EQ(VERIFY(fresh, img, n, NULL), PURR_V_BAD_VERSION);

    /* the floor only counts in enforce mode */
    s = good_os();                                   /* version 1.2.3 */
    n = build(&s, img);
    e = env_for(PURR_CHIP_ESP32S3);
    e.version_floor = PURR_VERSION(1, 3, 0);
    e.enforce_floor = 0;
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_OK);
    e.enforce_floor = 1;
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_BELOW_FLOOR);
    e.version_floor = PURR_VERSION(1, 2, 3);         /* equal to the floor is fine */
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_OK);
    e.version_floor = PURR_VERSION(1, 2, 2);
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_OK);
}

static void test_layout_of_payload(void)
{
    purr_verify_env_t e = env_for(PURR_CHIP_ESP32S3);
    spec_t os = good_os();
    uint32_t n = build(&os, img);
    purr_image_header_t h, orig;
    memcpy(&orig, img, sizeof(orig));

    /* payload runs past the end of the image */
    h = orig;
    h.payload_size += 1;
    memcpy(img, &h, sizeof(h));
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_BAD_LAYOUT);

    /* payload starts inside the header */
    h = orig;
    h.payload_offset = 20;
    memcpy(img, &h, sizeof(h));
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_BAD_LAYOUT);

    /* offset + size overflows 32 bits */
    h = orig;
    h.payload_offset = 0xFFFFFF00u;
    h.payload_size = 0x200;
    memcpy(img, &h, sizeof(h));
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_BAD_LAYOUT);
    h.payload_offset = sizeof(h);
    h.payload_size = 0xFFFFFFFFu;
    memcpy(img, &h, sizeof(h));
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_BAD_LAYOUT);
}

static void test_keys(void)
{
    purr_verify_env_t e = env_for(PURR_CHIP_ESP32S3);
    spec_t s = good_os();
    uint32_t n;

    /* a key id that is not in the bag */
    s.key = 9;
    n = build(&s, img);
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_NO_KEY);

    /* a revoked key */
    s = good_os();
    n = build(&s, img);
    g_bag.revoked = 1u << K_BOOT;
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_REVOKED);
    g_bag.revoked = 0;
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_OK);

    /* a valid signature from a key whose role may not sign this */
    s = good_os();
    s.key = K_SYSTEM;                                /* system cannot sign an OS image */
    n = build(&s, img);
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_ROLE_MISMATCH);
    spec_t k = {PURR_CHIP_ESP32S3, PURR_IMG_MODULE, K_BOOT, PURR_MOD_APPMANAGER, "1.0.0", "", 10};
    n = build(&k, img);                              /* boot cannot sign AppManager */
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_ROLE_MISMATCH);
    spec_t d = {PURR_CHIP_ESP32S3, PURR_IMG_MODULE, K_DEV, PURR_MOD_DEVBUNDLE, "1.0.0", "", 10};
    n = build(&d, img);                              /* developer cannot sign the device bundle */
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_ROLE_MISMATCH);

    /* the role check runs before the hash, so it wins over a bad payload */
    s = good_os();
    s.key = K_SYSTEM;
    n = build(&s, img);
    img[sizeof(purr_image_header_t) + 5] ^= 0xFF;
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_ROLE_MISMATCH);
}

static void test_hash_and_signature(void)
{
    purr_verify_env_t e = env_for(PURR_CHIP_ESP32S3);
    spec_t s = good_os();
    uint32_t n = build(&s, img);

    /* one flipped payload byte */
    img[sizeof(purr_image_header_t) + 700] ^= 1;
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_BAD_HASH);
    img[sizeof(purr_image_header_t) + 700] ^= 1;
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_OK);

    /* a header byte the signature covers, changed without re-signing */
    img[offsetof(purr_image_header_t, name) + 1] ^= 1;
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_BAD_SIGNATURE);
    img[offsetof(purr_image_header_t, name) + 1] ^= 1;
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_OK);

    /* a flipped signature bit */
    img[offsetof(purr_image_header_t, signature) + 10] ^= 0x80;
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_BAD_SIGNATURE);
    img[offsetof(purr_image_header_t, signature) + 10] ^= 0x80;

    /* signed by a different key that claims to be key 0 */
    resign(img, K_OTHER);
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_BAD_SIGNATURE);
    resign(img, K_BOOT);
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_OK);

    /* changing the signed header and re-signing with the right key works */
    purr_image_header_t h;
    memcpy(&h, img, sizeof(h));
    strncpy(h.name, "renamed", sizeof(h.name));
    memcpy(img, &h, sizeof(h));
    resign(img, K_BOOT);
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_OK);
}

static void test_io_and_unterminated(void)
{
    purr_verify_env_t e = env_for(PURR_CHIP_ESP32S3);
    spec_t s = good_os();
    uint32_t n = build(&s, img);

    /* a read error at each point of the process is reported, never ignored */
    /* Reading this image takes 4 reads: the header, then 3 chunks of payload. */
    for (int fail = 0; fail < 4; fail++) {
        mem_t m = {img, n, fail, 0};
        CHECK_EQ(purr_image_verify(&e, mem_read, &m, n, NULL), PURR_V_IO_ERROR);
    }
    mem_t enough = {img, n, 4, 0};
    CHECK_EQ(purr_image_verify(&e, mem_read, &enough, n, NULL), PURR_V_OK);
    mem_t ok = {img, n, -1, 0};
    CHECK_EQ(purr_image_verify(&e, mem_read, &ok, n, NULL), PURR_V_OK);

    /* name and version fields filled to the last byte, with no NUL */
    purr_image_header_t h;
    memcpy(&h, img, sizeof(h));
    memset(h.name, 'n', sizeof(h.name));
    memcpy(h.version, "1.2.3-abcde", 11);
    h.version[11] = 'x';                             /* not NUL-terminated */
    memcpy(img, &h, sizeof(h));
    resign(img, K_BOOT);
    CHECK_EQ(VERIFY(e, img, n, NULL), PURR_V_OK);    /* "1.2.3-abcdex" still parses as 1.2.3 */
}

int main(void)
{
    setup_keys();
    test_good_images();
    test_header_checks();
    test_versions();
    test_layout_of_payload();
    test_keys();
    test_hash_and_signature();
    test_io_and_unterminated();
    TK_DONE("test_verify");
}
