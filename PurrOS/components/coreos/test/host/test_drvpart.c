/* The driver partition's slot format (purr_drvpart.h): ten 8 KB slots, a 64-byte header and the signed .cat in each. */
#include <stdlib.h>
#include <string.h>

#include "purr_drvpart.h"
#include "purr_util.h"
#include "testkit.h"

static uint8_t s_part[PURR_DRVPART_SIZE];

static int part_read(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    (void)ctx;
    if (off + len > sizeof(s_part)) {
        return -1;
    }
    memcpy(buf, s_part + off, len);
    return 0;
}

static void blank(void)
{
    memset(s_part, 0xFF, sizeof(s_part));
}

static void put(int slot, const char *file, const uint8_t *cat, uint32_t len)
{
    CHECK_EQ(purr_drvpart_build_slot(s_part + slot * PURR_DRVPART_SLOT_SIZE, file, cat, len), 0);
}

static void test_sizes(void)
{
    CHECK_EQ(sizeof(purr_drvslot_hdr_t), PURR_DRVPART_HEADER);          /* the header is exactly 64 bytes, as the Python packer writes it */
    CHECK_EQ(PURR_DRVPART_SIZE, 0x14000u);
    CHECK_EQ(PURR_DRVPART_SLOTS, 10);
    CHECK_EQ(PURR_DRVPART_MAX_CAT, 8192u - 64u);
}

static void test_blank_partition_is_empty(void)
{
    blank();
    for (int i = 0; i < PURR_DRVPART_SLOTS; i++) {
        purr_drvslot_info_t info;
        CHECK_EQ(purr_drvpart_slot_info(part_read, NULL, i, &info), PURR_DRVPART_EMPTY);
    }
}

static void test_a_driver_round_trips(void)
{
    blank();
    uint8_t cat[1000];
    for (int i = 0; i < (int)sizeof(cat); i++) {
        cat[i] = (uint8_t)(i * 7 + 3);
    }
    put(2, "gt911.cat", cat, sizeof(cat));
    purr_drvslot_info_t info;
    CHECK_EQ(purr_drvpart_slot_info(part_read, NULL, 2, &info), 0);
    CHECK_EQ(info.slot, 2);
    CHECK_EQ(info.length, 1000u);
    CHECK(strcmp(info.file, "gt911.cat") == 0);
    CHECK_EQ(purr_drvpart_slot_info(part_read, NULL, 1, &info), PURR_DRVPART_EMPTY);       /* the neighbours stay empty */
    CHECK_EQ(purr_drvpart_slot_info(part_read, NULL, 3, &info), PURR_DRVPART_EMPTY);

    uint8_t out[PURR_DRVPART_MAX_CAT];
    uint32_t n = 0;
    CHECK_EQ(purr_drvpart_slot_read(part_read, NULL, 2, out, sizeof(out), &n, &info), 0);
    CHECK_EQ(n, 1000u);
    CHECK(memcmp(out, cat, 1000) == 0);
    CHECK_EQ(purr_drvpart_slot_read(part_read, NULL, 4, out, sizeof(out), &n, &info), PURR_DRVPART_EMPTY);
}

static void test_the_largest_driver_and_the_names(void)
{
    blank();
    static uint8_t big[PURR_DRVPART_MAX_CAT];
    memset(big, 0x5A, sizeof(big));
    put(0, "kbd-backlight.cat", big, sizeof(big));                     /* fills the slot to its last byte */
    uint8_t out[PURR_DRVPART_MAX_CAT];
    uint32_t n = 0;
    purr_drvslot_info_t info;
    CHECK_EQ(purr_drvpart_slot_read(part_read, NULL, 0, out, sizeof(out), &n, &info), 0);
    CHECK_EQ(n, (uint32_t)PURR_DRVPART_MAX_CAT);
    CHECK(strcmp(info.file, "kbd-backlight.cat") == 0);

    uint8_t slot[PURR_DRVPART_SLOT_SIZE];
    CHECK_EQ(purr_drvpart_build_slot(slot, "a.cat", big, PURR_DRVPART_MAX_CAT + 1), -1);       /* too big for a slot */
    CHECK_EQ(purr_drvpart_build_slot(slot, "a.cat", big, 0), -1);
    CHECK_EQ(purr_drvpart_build_slot(slot, "", big, 10), -1);                                    /* no name */
    CHECK_EQ(purr_drvpart_build_slot(slot, "../x.cat", big, 10), -1);                            /* a file name, never a path */
    CHECK_EQ(purr_drvpart_build_slot(slot, "a/b.cat", big, 10), -1);
    char longname[40];
    memset(longname, 'a', sizeof(longname));
    longname[39] = '\0';
    CHECK_EQ(purr_drvpart_build_slot(slot, longname, big, 10), -1);                              /* 39 letters do not fit in 32 */
    CHECK_EQ(purr_drvpart_build_slot(slot, "ok.cat", big, 10), 0);
}

static void test_damage_is_found_and_skipped(void)
{
    blank();
    uint8_t cat[200];
    memset(cat, 0x42, sizeof(cat));
    uint8_t out[PURR_DRVPART_MAX_CAT];
    uint32_t n = 0;
    purr_drvslot_info_t info;

    put(0, "a.cat", cat, sizeof(cat));
    s_part[0 * PURR_DRVPART_SLOT_SIZE + PURR_DRVPART_HEADER + 10] ^= 0x01;           /* a flipped bit in the driver: the crc says so */
    CHECK_EQ(purr_drvpart_slot_info(part_read, NULL, 0, &info), 0);                  /* the header is fine ... */
    CHECK_EQ(purr_drvpart_slot_read(part_read, NULL, 0, out, sizeof(out), &n, &info), PURR_DRVPART_DAMAGED);    /* ... the content is not */

    put(1, "b.cat", cat, sizeof(cat));
    s_part[1 * PURR_DRVPART_SLOT_SIZE] = 'X';                                         /* the magic */
    CHECK_EQ(purr_drvpart_slot_info(part_read, NULL, 1, &info), PURR_DRVPART_DAMAGED);

    put(2, "c.cat", cat, sizeof(cat));
    s_part[2 * PURR_DRVPART_SLOT_SIZE + 4] = 9;                                       /* an unknown version */
    CHECK_EQ(purr_drvpart_slot_info(part_read, NULL, 2, &info), PURR_DRVPART_DAMAGED);

    put(3, "d.cat", cat, sizeof(cat));
    s_part[3 * PURR_DRVPART_SLOT_SIZE + 8] = 0xFF;                                    /* a length longer than a slot */
    s_part[3 * PURR_DRVPART_SLOT_SIZE + 9] = 0xFF;
    CHECK_EQ(purr_drvpart_slot_info(part_read, NULL, 3, &info), PURR_DRVPART_DAMAGED);

    put(4, "e.cat", cat, sizeof(cat));
    memset(s_part + 4 * PURR_DRVPART_SLOT_SIZE + 16, 'z', 32);                        /* a file name with no end */
    CHECK_EQ(purr_drvpart_slot_info(part_read, NULL, 4, &info), PURR_DRVPART_DAMAGED);

    put(5, "f.cat", cat, sizeof(cat));
    CHECK_EQ(purr_drvpart_slot_read(part_read, NULL, 5, out, 100, &n, &info), PURR_DRVPART_DAMAGED);   /* the caller's buffer is too small */

    CHECK_EQ(purr_drvpart_slot_info(part_read, NULL, 10, &info), PURR_DRVPART_DAMAGED);   /* there is no slot 10 */
    CHECK_EQ(purr_drvpart_slot_info(part_read, NULL, -1, &info), PURR_DRVPART_DAMAGED);
    CHECK_EQ(purr_drvpart_slot_info(part_read, NULL, 9, &info), PURR_DRVPART_EMPTY);       /* the ones around the damage are not affected */
    put(6, "g.cat", cat, sizeof(cat));
    CHECK_EQ(purr_drvpart_slot_read(part_read, NULL, 6, out, sizeof(out), &n, &info), 0);
}

/* The bytes the Python packer (purrstrap/scripts/drvpart.py) must write for this driver: a golden slot, so the two sides cannot drift apart. */
static void test_the_slot_bytes(void)
{
    uint8_t slot[PURR_DRVPART_SLOT_SIZE];
    const uint8_t cat[4] = {1, 2, 3, 4};
    CHECK_EQ(purr_drvpart_build_slot(slot, "x.cat", cat, 4), 0);
    CHECK_EQ(slot[0], 'P');
    CHECK_EQ(slot[1], 'D');
    CHECK_EQ(slot[2], 'R');
    CHECK_EQ(slot[3], 'V');
    CHECK_EQ(slot[4], 1);                            /* version, little-endian u16 */
    CHECK_EQ(slot[5], 0);
    CHECK_EQ(slot[6], 0);                            /* flags */
    CHECK_EQ(slot[8], 4);                            /* length */
    CHECK_EQ(slot[9], 0);
    uint32_t crc = purr_crc32(0, cat, 4);
    CHECK_EQ(slot[12], (uint8_t)crc);
    CHECK_EQ(slot[15], (uint8_t)(crc >> 24));
    CHECK(strcmp((const char *)slot + 16, "x.cat") == 0);
    CHECK_EQ(slot[16 + 6], 0);                       /* the name is padded with zeros */
    CHECK_EQ(slot[48], 0xFF);                        /* reserved */
    CHECK_EQ(slot[63], 0xFF);
    CHECK_EQ(slot[64], 1);                           /* the .cat follows the header */
    CHECK_EQ(slot[67], 4);
    CHECK_EQ(slot[68], 0xFF);                        /* and then the erased state */
    CHECK_EQ(slot[PURR_DRVPART_SLOT_SIZE - 1], 0xFF);
    printf("golden crc32 of 01 02 03 04: %08x\n", crc);
}

int main(void)
{
    test_sizes();
    test_blank_partition_is_empty();
    test_a_driver_round_trips();
    test_the_largest_driver_and_the_names();
    test_damage_is_found_and_skipped();
    test_the_slot_bytes();
    TK_DONE("test_drvpart");
}
