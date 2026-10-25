#include <stdlib.h>
#include <string.h>

#include "purr_trailer.h"
#include "purr_util.h"
#include "testcrypto.h"
#include "testkit.h"

/* Keys: 0 boot, 1 system, 2 boot again (a second, different boot key). */
enum { K_BOOT = 0, K_SYSTEM = 1, K_BOOT2 = 2, K_COUNT = 3 };
static uint8_t g_pub[K_COUNT][64], g_priv[K_COUNT][32];
static purr_keybag_t g_bag;
static const uint8_t g_role[K_COUNT] = {PURR_ROLE_BOOT, PURR_ROLE_SYSTEM, PURR_ROLE_BOOT};

static void setup_keys(void)
{
    purr_key_t defaults[K_COUNT];

    tc_seed(0xC0FFEE02u);
    for (int i = 0; i < K_COUNT; i++) {
        tc_make_key(g_pub[i], g_priv[i]);
        defaults[i].key_id = (uint8_t)i;
        defaults[i].role = g_role[i];
        memcpy(defaults[i].pub, g_pub[i], 64);
    }
    purr_keybag_build(&g_bag, defaults, K_COUNT, NULL);
}

/* ---- a fake flash partition, with the reader the trailer verifier is given ---- */
#define PART_SIZE   16384u          /* image area 12288, trailer 4096 */
#define IMAGE_LEN   5000u

typedef struct {
    uint8_t  data[PART_SIZE];
    uint32_t size;
    int      fail_after;            /* fail every read once this many have happened, -1 = never */
    int      reads;
} part_t;

static int part_read(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    part_t *p = ctx;
    if (p->fail_after >= 0 && p->reads++ >= p->fail_after) {
        return -1;
    }
    if ((uint64_t)off + len > p->size) {
        return -1;
    }
    memcpy(buf, p->data + off, len);
    return 0;
}

typedef struct {
    uint16_t chip;
    uint8_t  type, subtype, key;
    const char *version;
    uint32_t payload_len;
} spec_t;

static spec_t kernel_spec(void)
{
    spec_t s = {PURR_CHIP_ESP32S3, PURR_IMG_MODULE, PURR_MOD_KERNEL, K_BOOT, "1.2.3", IMAGE_LEN};
    return s;
}

static void sign_header(purr_image_header_t *h, int key)
{
    uint8_t digest[32];
    purr_sha256(h, PURR_IMAGE_SIGNED_LEN, digest);
    tc_sign(g_priv[key], digest, h->signature);
}

/* The way a release is cut: the ESP image goes at offset 0, the signed container header is
 * copied to the last sector. Everything else is erased flash. */
static void cut(part_t *p, const spec_t *s)
{
    purr_image_header_t h;

    memset(p, 0xFF, sizeof(*p));
    p->size = PART_SIZE;
    p->fail_after = -1;

    p->data[0] = 0xE9;                                    /* an ESP app image */
    for (uint32_t i = 1; i < s->payload_len; i++) {
        p->data[i] = (uint8_t)(i * 31u + 7u);
    }

    memset(&h, 0, sizeof(h));
    h.magic = PURR_IMAGE_MAGIC;
    h.header_version = PURR_IMAGE_HEADER_VERSION;
    h.header_size = sizeof(h);
    h.chip_id = s->chip;
    h.image_type = s->type;
    h.key_id = s->key;
    h.flags = s->subtype;
    strncpy(h.name, "slot", sizeof(h.name));
    strncpy(h.version, s->version, sizeof(h.version));
    h.payload_offset = sizeof(h);
    h.payload_size = s->payload_len;
    purr_sha256(p->data, s->payload_len, h.payload_sha256);
    sign_header(&h, s->key < K_COUNT ? s->key : 0);
    memcpy(p->data + PURR_TRAILER_OFFSET(PART_SIZE), &h, sizeof(h));
}

static purr_image_header_t *trailer(part_t *p)
{
    return (purr_image_header_t *)(p->data + PURR_TRAILER_OFFSET(PART_SIZE));
}

static purr_verify_env_t env_for(uint16_t chip)
{
    purr_verify_env_t e = {chip, &g_bag, &purr_crypto_uecc, PURR_VERSION(1, 0, 0), 0, 0, NULL};
    return e;
}

static purr_verify_result_t check(part_t *p, uint16_t chip, uint8_t type, uint8_t subtype,
                                  purr_image_header_t *h)
{
    purr_verify_env_t e = env_for(chip);
    return purr_trailer_verify(&e, part_read, p, p->size, type, subtype, h);
}

static purr_verify_result_t check_kernel(part_t *p)
{
    return check(p, PURR_CHIP_ESP32S3, PURR_IMG_MODULE, PURR_MOD_KERNEL, NULL);
}

static part_t part;

static void test_good_slots(void)
{
    purr_image_header_t h;
    spec_t k = kernel_spec();

    cut(&part, &k);
    CHECK_EQ(check(&part, PURR_CHIP_ESP32S3, PURR_IMG_MODULE, PURR_MOD_KERNEL, &h), PURR_V_OK);
    CHECK_EQ(h.key_id, K_BOOT);
    CHECK_EQ(h.payload_size, IMAGE_LEN);
    CHECK(strcmp(h.version, "1.2.3") == 0);

    /* Each slot kind, signed for what it is. */
    spec_t kitten = {PURR_CHIP_ESP32S3, PURR_IMG_RECOVERY, 0, K_BOOT, "0.2.7", 3000};
    cut(&part, &kitten);
    CHECK_EQ(check(&part, PURR_CHIP_ESP32S3, PURR_IMG_RECOVERY, 0, NULL), PURR_V_OK);

    spec_t loader = {PURR_CHIP_ESP32S3, PURR_IMG_MODULE, PURR_MOD_LOADER, K_BOOT, "0.1.0", 4000};
    cut(&part, &loader);
    CHECK_EQ(check(&part, PURR_CHIP_ESP32S3, PURR_IMG_MODULE, PURR_MOD_LOADER, NULL), PURR_V_OK);

    /* A second boot-role key is just as good; the key id travels in the trailer. */
    k.key = K_BOOT2;
    cut(&part, &k);
    CHECK_EQ(check(&part, PURR_CHIP_ESP32S3, PURR_IMG_MODULE, PURR_MOD_KERNEL, &h), PURR_V_OK);
    CHECK_EQ(h.key_id, K_BOOT2);

    /* The largest image the partition can hold, right up against the trailer. */
    k = kernel_spec();
    k.payload_len = PURR_TRAILER_MAX_IMAGE(PART_SIZE) - 4u;
    cut(&part, &k);
    CHECK_EQ(check_kernel(&part), PURR_V_OK);

    /* hdr_out is optional. */
    k = kernel_spec();
    cut(&part, &k);
    purr_verify_env_t e = env_for(PURR_CHIP_ESP32S3);
    CHECK_EQ(purr_trailer_verify(&e, part_read, &part, PART_SIZE, PURR_IMG_MODULE, PURR_MOD_KERNEL, NULL),
             PURR_V_OK);
}

static void test_no_trailer(void)
{
    spec_t k = kernel_spec();

    /* A slot flashed straight from a build: a real ESP image, erased trailer. */
    cut(&part, &k);
    memset(part.data + PURR_TRAILER_OFFSET(PART_SIZE), 0xFF, PURR_TRAILER_SIZE);
    CHECK_EQ(check_kernel(&part), PURR_V_BAD_MAGIC);

    /* A blank slot. */
    memset(part.data, 0xFF, sizeof(part.data));
    CHECK_EQ(check_kernel(&part), PURR_V_BAD_MAGIC);

    /* Zeroed. */
    memset(part.data, 0x00, sizeof(part.data));
    CHECK_EQ(check_kernel(&part), PURR_V_BAD_MAGIC);
}

static void test_tampering(void)
{
    spec_t k = kernel_spec();

    /* One flipped bit anywhere in the image. */
    cut(&part, &k);
    part.data[IMAGE_LEN - 1] ^= 0x01;
    CHECK_EQ(check_kernel(&part), PURR_V_BAD_HASH);

    cut(&part, &k);
    part.data[0] ^= 0x80;
    CHECK_EQ(check_kernel(&part), PURR_V_BAD_HASH);

    /* Bytes past the image are not covered and may be anything (erased flash, leftovers). */
    cut(&part, &k);
    part.data[IMAGE_LEN] = 0x12;
    part.data[PART_SIZE - PURR_TRAILER_SIZE - 1] = 0x34;
    CHECK_EQ(check_kernel(&part), PURR_V_OK);

    /* A header field changed after signing: the signature no longer matches. */
    cut(&part, &k);
    strncpy(trailer(&part)->name, "evil", sizeof(trailer(&part)->name));
    CHECK_EQ(check_kernel(&part), PURR_V_BAD_SIGNATURE);

    cut(&part, &k);
    strncpy(trailer(&part)->version, "9.9.9", sizeof(trailer(&part)->version));
    CHECK_EQ(check_kernel(&part), PURR_V_BAD_SIGNATURE);

    /* A flipped signature bit. */
    cut(&part, &k);
    trailer(&part)->signature[10] ^= 0x01;
    CHECK_EQ(check_kernel(&part), PURR_V_BAD_SIGNATURE);

    /* Someone else's signature over the same header. */
    cut(&part, &k);
    {
        spec_t other = kernel_spec();
        part_t donor;
        other.key = K_BOOT2;
        cut(&donor, &other);
        /* same key id in the header, different signer */
        memcpy(trailer(&part)->signature, trailer(&donor)->signature, PURR_SIG_LEN);
    }
    CHECK_EQ(check_kernel(&part), PURR_V_BAD_SIGNATURE);
}

static void test_what_the_partition_may_hold(void)
{
    /* A validly signed loader is not a kernel, and the other way round. */
    spec_t loader = {PURR_CHIP_ESP32S3, PURR_IMG_MODULE, PURR_MOD_LOADER, K_BOOT, "0.1.0", 4000};
    cut(&part, &loader);
    CHECK_EQ(check_kernel(&part), PURR_V_BAD_TYPE);

    spec_t k = kernel_spec();
    cut(&part, &k);
    CHECK_EQ(check(&part, PURR_CHIP_ESP32S3, PURR_IMG_MODULE, PURR_MOD_LOADER, NULL), PURR_V_BAD_TYPE);
    CHECK_EQ(check(&part, PURR_CHIP_ESP32S3, PURR_IMG_RECOVERY, 0, NULL), PURR_V_BAD_TYPE);

    /* An app image is not any boot slot. */
    spec_t app = {PURR_CHIP_ANY, PURR_IMG_APP, 0, K_BOOT, "0.1.0", 64};
    cut(&part, &app);
    CHECK_EQ(check_kernel(&part), PURR_V_BAD_TYPE);
}

static void test_keys_and_roles(void)
{
    spec_t k = kernel_spec();

    /* A system-role key may not sign a kernel. */
    k.key = K_SYSTEM;
    cut(&part, &k);
    CHECK_EQ(check_kernel(&part), PURR_V_ROLE_MISMATCH);

    /* A key id the device does not know. */
    k = kernel_spec();
    cut(&part, &k);
    trailer(&part)->key_id = 9;
    sign_header(trailer(&part), K_BOOT);
    CHECK_EQ(check_kernel(&part), PURR_V_NO_KEY);

    /* A revoked key. */
    cut(&part, &k);
    purr_keybag_t saved = g_bag;
    g_bag.revoked = 1u << K_BOOT;
    CHECK_EQ(check_kernel(&part), PURR_V_REVOKED);
    g_bag = saved;
    CHECK_EQ(check_kernel(&part), PURR_V_OK);
}

static void test_chip_version_layout(void)
{
    spec_t k = kernel_spec();
    cut(&part, &k);

    /* An image for another chip. */
    CHECK_EQ(check(&part, PURR_CHIP_ESP32, PURR_IMG_MODULE, PURR_MOD_KERNEL, NULL), PURR_V_BAD_CHIP);

    /* Version floor, enforced only when asked to be. */
    purr_verify_env_t e = env_for(PURR_CHIP_ESP32S3);
    e.version_floor = PURR_VERSION(1, 2, 4);
    CHECK_EQ(purr_trailer_verify(&e, part_read, &part, PART_SIZE, PURR_IMG_MODULE, PURR_MOD_KERNEL, NULL),
             PURR_V_OK);
    e.enforce_floor = 1;
    CHECK_EQ(purr_trailer_verify(&e, part_read, &part, PART_SIZE, PURR_IMG_MODULE, PURR_MOD_KERNEL, NULL),
             PURR_V_BELOW_FLOOR);

    /* A payload that would run into the trailer, even though it is correctly signed. */
    cut(&part, &k);
    trailer(&part)->payload_size = PURR_TRAILER_MAX_IMAGE(PART_SIZE) + 1u;
    sign_header(trailer(&part), K_BOOT);
    CHECK_EQ(check_kernel(&part), PURR_V_BAD_LAYOUT);

    /* A payload offset that is inside the header, or outside the trailer sector. */
    cut(&part, &k);
    trailer(&part)->payload_offset = 10;
    sign_header(trailer(&part), K_BOOT);
    CHECK_EQ(check_kernel(&part), PURR_V_BAD_LAYOUT);

    cut(&part, &k);
    trailer(&part)->payload_offset = PURR_TRAILER_SIZE + 1u;
    sign_header(trailer(&part), K_BOOT);
    CHECK_EQ(check_kernel(&part), PURR_V_BAD_LAYOUT);

    /* A payload offset that is legal and non-default must still verify (the gap between the
     * header and the payload is never read). */
    cut(&part, &k);
    trailer(&part)->payload_offset = 256;
    sign_header(trailer(&part), K_BOOT);
    CHECK_EQ(check_kernel(&part), PURR_V_OK);

    /* A partition too small to hold a trailer and an image. */
    cut(&part, &k);
    e = env_for(PURR_CHIP_ESP32S3);
    CHECK_EQ(purr_trailer_verify(&e, part_read, &part, PURR_TRAILER_SIZE, PURR_IMG_MODULE,
                                 PURR_MOD_KERNEL, NULL), PURR_V_BAD_LAYOUT);
    CHECK_EQ(purr_trailer_verify(&e, part_read, &part, 0, PURR_IMG_MODULE, PURR_MOD_KERNEL, NULL),
             PURR_V_BAD_LAYOUT);
}

static void test_io(void)
{
    spec_t k = kernel_spec();

    /* Flash that stops answering: at the trailer, and part-way through the hash. */
    for (int fail_at = 0; fail_at < 4; fail_at++) {
        cut(&part, &k);
        part.fail_after = fail_at;
        CHECK_EQ(check_kernel(&part), PURR_V_IO_ERROR);
    }

    /* Every read it makes stays inside the partition. */
    cut(&part, &k);
    part.size = PART_SIZE;
    CHECK_EQ(check_kernel(&part), PURR_V_OK);
}

int main(void)
{
    setup_keys();
    test_good_slots();
    test_no_trailer();
    test_tampering();
    test_what_the_partition_may_hold();
    test_keys_and_roles();
    test_chip_version_layout();
    test_io();
    TK_DONE("test_trailer");
}
