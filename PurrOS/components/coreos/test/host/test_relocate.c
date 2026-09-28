#include <string.h>

#include "purr_relocate.h"
#include "testkit.h"

static void test_single_relocation(void)
{
    uint8_t buf[8] = {0x10, 0x00, 0x00, 0x00, 0xAA, 0xAA, 0xAA, 0xAA};
    uint32_t offsets[] = {0};
    purr_reloc_result_t r = purr_relocate(buf, sizeof(buf), offsets, 1, 0x40000000);
    CHECK_EQ(r, PURR_RELOC_OK);
    uint32_t word;
    memcpy(&word, buf, 4);
    CHECK_EQ(word, 0x40000010u);
    /* the untouched tail is exactly that -- untouched */
    CHECK_EQ(buf[4], 0xAA);
    CHECK_EQ(buf[7], 0xAA);
}

static void test_multiple_relocations(void)
{
    uint8_t buf[16] = {0};
    /* three words needing the base added, one left alone */
    memcpy(buf + 0, &(uint32_t){0x00000001}, 4);
    memcpy(buf + 4, &(uint32_t){0x12345678}, 4); /* not relocated -- must survive untouched */
    memcpy(buf + 8, &(uint32_t){0x00000002}, 4);
    memcpy(buf + 12, &(uint32_t){0x00000003}, 4);

    uint32_t offsets[] = {0, 8, 12};
    purr_reloc_result_t r = purr_relocate(buf, sizeof(buf), offsets, 3, 0x1000);
    CHECK_EQ(r, PURR_RELOC_OK);

    uint32_t w;
    memcpy(&w, buf + 0, 4);  CHECK_EQ(w, 0x1001u);
    memcpy(&w, buf + 4, 4);  CHECK_EQ(w, 0x12345678u);
    memcpy(&w, buf + 8, 4);  CHECK_EQ(w, 0x1002u);
    memcpy(&w, buf + 12, 4); CHECK_EQ(w, 0x1003u);
}

static void test_empty_list_is_a_no_op(void)
{
    uint8_t buf[4] = {0x01, 0x02, 0x03, 0x04};
    uint8_t before[4];
    memcpy(before, buf, 4);
    purr_reloc_result_t r = purr_relocate(buf, sizeof(buf), NULL, 0, 0xDEADBEEF);
    CHECK_EQ(r, PURR_RELOC_OK);
    CHECK(memcmp(buf, before, 4) == 0);
}

static void test_misaligned_offset_rejected(void)
{
    uint8_t buf[8] = {0};
    uint32_t offsets[] = {1};
    purr_reloc_result_t r = purr_relocate(buf, sizeof(buf), offsets, 1, 0x100);
    CHECK_EQ(r, PURR_RELOC_MISALIGNED);
}

static void test_offset_past_end_rejected(void)
{
    uint8_t buf[8] = {0};
    uint32_t offsets[] = {8}; /* exactly at the end -- no room for a 4-byte word */
    purr_reloc_result_t r = purr_relocate(buf, sizeof(buf), offsets, 1, 0x100);
    CHECK_EQ(r, PURR_RELOC_BAD_OFFSET);
}

static void test_offset_partially_past_end_rejected(void)
{
    uint8_t buf[8] = {0};
    uint32_t offsets[] = {4}; /* offset 4, word needs bytes 4..7 -- exactly fits, should be OK */
    purr_reloc_result_t r = purr_relocate(buf, sizeof(buf), offsets, 1, 0x100);
    CHECK_EQ(r, PURR_RELOC_OK);

    uint32_t offsets2[] = {5}; /* misaligned AND would overrun -- misalignment caught first */
    r = purr_relocate(buf, sizeof(buf), offsets2, 1, 0x100);
    CHECK_EQ(r, PURR_RELOC_MISALIGNED);
}

static void test_huge_offset_does_not_overflow(void)
{
    /* buf_len - off is computed with unsigned wraparound in mind -- an offset bigger than
     * buf_len must not wrap into looking valid. */
    uint8_t buf[8] = {0};
    uint32_t offsets[] = {0xFFFFFFFCu};
    purr_reloc_result_t r = purr_relocate(buf, sizeof(buf), offsets, 1, 0x100);
    CHECK_EQ(r, PURR_RELOC_BAD_OFFSET);
}

static void test_too_many_rejected_before_touching_buffer(void)
{
    uint8_t buf[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    uint8_t before[8];
    memcpy(before, buf, 8);
    /* Doesn't need PURR_RELOC_MAX real entries -- a count over the cap must be rejected
     * before any offset is even looked at, so a bogus (even NULL) pointer here is fine. */
    purr_reloc_result_t r = purr_relocate(buf, sizeof(buf), NULL, PURR_RELOC_MAX + 1, 0x100);
    CHECK_EQ(r, PURR_RELOC_TOO_MANY);
    CHECK(memcmp(buf, before, 8) == 0);
}

static void test_one_bad_offset_leaves_buffer_untouched(void)
{
    /* Two good relocations followed by one bad one: nothing should be written, not even the
     * good ones that come first in the list. */
    uint8_t buf[12];
    memcpy(buf + 0, &(uint32_t){0x00000001}, 4);
    memcpy(buf + 4, &(uint32_t){0x00000002}, 4);
    memcpy(buf + 8, &(uint32_t){0x00000003}, 4);
    uint8_t before[12];
    memcpy(before, buf, 12);

    uint32_t offsets[] = {0, 4, 9}; /* 9 is misaligned */
    purr_reloc_result_t r = purr_relocate(buf, sizeof(buf), offsets, 3, 0x1000);
    CHECK_EQ(r, PURR_RELOC_MISALIGNED);
    CHECK(memcmp(buf, before, 12) == 0);
}

static void test_result_names(void)
{
    CHECK(strcmp(purr_reloc_result_name(PURR_RELOC_OK), "ok") == 0);
    CHECK(strcmp(purr_reloc_result_name(PURR_RELOC_TOO_MANY), "too-many") == 0);
    CHECK(strcmp(purr_reloc_result_name(PURR_RELOC_MISALIGNED), "misaligned") == 0);
    CHECK(strcmp(purr_reloc_result_name(PURR_RELOC_BAD_OFFSET), "bad-offset") == 0);
}

static void test_base_zero_is_a_no_op(void)
{
    /* A module "relocated" to base 0 (its own link base) should come out byte-identical --
     * matches the link-twice method's premise that link base 0 is the reference build. */
    uint8_t buf[4];
    memcpy(buf, &(uint32_t){0xCAFEF00D}, 4);
    uint8_t before[4];
    memcpy(before, buf, 4);
    uint32_t offsets[] = {0};
    purr_reloc_result_t r = purr_relocate(buf, sizeof(buf), offsets, 1, 0);
    CHECK_EQ(r, PURR_RELOC_OK);
    CHECK(memcmp(buf, before, 4) == 0);
}

int main(void)
{
    test_single_relocation();
    test_multiple_relocations();
    test_empty_list_is_a_no_op();
    test_misaligned_offset_rejected();
    test_offset_past_end_rejected();
    test_offset_partially_past_end_rejected();
    test_huge_offset_does_not_overflow();
    test_too_many_rejected_before_touching_buffer();
    test_one_bad_offset_leaves_buffer_untouched();
    test_result_names();
    test_base_zero_is_a_no_op();
    TK_DONE("test_relocate");
}
