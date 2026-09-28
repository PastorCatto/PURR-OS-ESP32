#include <string.h>

#include "purr_module.h"
#include "purr_relocate.h"
#include "testkit.h"

/* Builds a payload: entry_offset, data relocs, code relocs, then a code blob of code_size
 * zero bytes. Returns the total size written. buf must be big enough. */
static uint32_t build_payload(uint8_t *buf, uint32_t entry_offset,
                              const uint32_t *data_relocs, uint32_t data_count,
                              const uint32_t *code_relocs, uint32_t code_count,
                              uint32_t code_size)
{
    uint32_t p = 0;
    memcpy(buf + p, &entry_offset, 4); p += 4;
    memcpy(buf + p, &data_count, 4); p += 4;
    for (uint32_t i = 0; i < data_count; i++) {
        memcpy(buf + p, &data_relocs[i], 4);
        p += 4;
    }
    memcpy(buf + p, &code_count, 4); p += 4;
    for (uint32_t i = 0; i < code_count; i++) {
        memcpy(buf + p, &code_relocs[i], 4);
        p += 4;
    }
    memset(buf + p, 0, code_size);
    p += code_size;
    return p;
}

static void test_basic_layout(void)
{
    uint8_t buf[64];
    uint32_t data_relocs[] = {0};
    uint32_t code_relocs[] = {4};
    uint32_t total = build_payload(buf, 4, data_relocs, 1, code_relocs, 1, 16);

    purr_module_layout_t layout;
    purr_module_layout_result_t r = purr_module_parse_layout(buf, total, &layout);
    CHECK_EQ(r, PURR_MOD_LAYOUT_OK);
    CHECK_EQ(layout.entry_offset, 4u);
    CHECK_EQ(layout.data_reloc_count, 1u);
    CHECK_EQ(layout.data_reloc_table_offset, 8u);
    CHECK_EQ(layout.code_reloc_count, 1u);
    CHECK_EQ(layout.code_reloc_table_offset, 16u); /* 8 + 1*4 (data table) + 4 (code count) */
    CHECK_EQ(layout.code_offset, 20u);              /* 16 + 1*4 (code table) */
    CHECK_EQ(layout.code_size, 16u);
}

static void test_no_relocations_at_all(void)
{
    uint8_t buf[64];
    uint32_t total = build_payload(buf, 0, NULL, 0, NULL, 0, 20);
    purr_module_layout_t layout;
    purr_module_layout_result_t r = purr_module_parse_layout(buf, total, &layout);
    CHECK_EQ(r, PURR_MOD_LAYOUT_OK);
    CHECK_EQ(layout.data_reloc_count, 0u);
    CHECK_EQ(layout.code_reloc_count, 0u);
    CHECK_EQ(layout.code_offset, 12u); /* entry(4) + data_count(4) + code_count(4) */
    CHECK_EQ(layout.code_size, 20u);
}

static void test_too_short_for_fixed_header(void)
{
    uint8_t buf[4] = {0};
    purr_module_layout_t layout;
    purr_module_layout_result_t r = purr_module_parse_layout(buf, sizeof(buf), &layout);
    CHECK_EQ(r, PURR_MOD_LAYOUT_TOO_SHORT);
}

static void test_data_reloc_table_past_end_rejected(void)
{
    uint8_t buf[16];
    uint32_t entry = 0, count = 100; /* claims 100 data relocs, nowhere near enough room */
    memcpy(buf + 0, &entry, 4);
    memcpy(buf + 4, &count, 4);
    purr_module_layout_t layout;
    purr_module_layout_result_t r = purr_module_parse_layout(buf, sizeof(buf), &layout);
    CHECK_EQ(r, PURR_MOD_LAYOUT_BAD_SIZE);
}

static void test_too_many_data_relocs_rejected(void)
{
    uint8_t buf[16];
    uint32_t entry = 0, count = PURR_RELOC_MAX + 1;
    memcpy(buf + 0, &entry, 4);
    memcpy(buf + 4, &count, 4);
    purr_module_layout_t layout;
    purr_module_layout_result_t r = purr_module_parse_layout(buf, sizeof(buf), &layout);
    CHECK_EQ(r, PURR_MOD_LAYOUT_TOO_MANY);
}

static void test_code_reloc_table_past_end_rejected(void)
{
    /* Valid (empty) data table, then a code_reloc_count claiming way more than fits. */
    uint8_t buf[16];
    uint32_t entry = 0, data_count = 0, code_count = 100;
    memcpy(buf + 0, &entry, 4);
    memcpy(buf + 4, &data_count, 4);
    memcpy(buf + 8, &code_count, 4);
    purr_module_layout_t layout;
    purr_module_layout_result_t r = purr_module_parse_layout(buf, sizeof(buf), &layout);
    CHECK_EQ(r, PURR_MOD_LAYOUT_BAD_SIZE);
}

static void test_too_many_code_relocs_rejected(void)
{
    uint8_t buf[16];
    uint32_t entry = 0, data_count = 0, code_count = PURR_RELOC_MAX + 1;
    memcpy(buf + 0, &entry, 4);
    memcpy(buf + 4, &data_count, 4);
    memcpy(buf + 8, &code_count, 4);
    purr_module_layout_t layout;
    purr_module_layout_result_t r = purr_module_parse_layout(buf, sizeof(buf), &layout);
    CHECK_EQ(r, PURR_MOD_LAYOUT_TOO_MANY);
}

static void test_entry_offset_past_code_rejected(void)
{
    uint8_t buf[64];
    uint32_t total = build_payload(buf, 8, NULL, 0, NULL, 0, 8); /* 8-byte blob, offset 8 is past it */
    purr_module_layout_t layout;
    purr_module_layout_result_t r = purr_module_parse_layout(buf, total, &layout);
    CHECK_EQ(r, PURR_MOD_LAYOUT_BAD_ENTRY);
}

static void test_entry_offset_at_last_byte_ok(void)
{
    uint8_t buf[64];
    uint32_t total = build_payload(buf, 7, NULL, 0, NULL, 0, 8);
    purr_module_layout_t layout;
    purr_module_layout_result_t r = purr_module_parse_layout(buf, total, &layout);
    CHECK_EQ(r, PURR_MOD_LAYOUT_OK);
}

static void test_read_relocs_roundtrip(void)
{
    uint8_t buf[64];
    uint32_t data_relocs[] = {0, 4};
    uint32_t code_relocs[] = {8, 12, 16};
    uint32_t total = build_payload(buf, 0, data_relocs, 2, code_relocs, 3, 20);

    purr_module_layout_t layout;
    purr_module_layout_result_t r = purr_module_parse_layout(buf, total, &layout);
    CHECK_EQ(r, PURR_MOD_LAYOUT_OK);
    CHECK_EQ(layout.data_reloc_count, 2u);
    CHECK_EQ(layout.code_reloc_count, 3u);

    uint32_t data_out[2];
    purr_module_read_data_relocs(buf, &layout, data_out);
    CHECK_EQ(data_out[0], 0u);
    CHECK_EQ(data_out[1], 4u);

    uint32_t code_out[3];
    purr_module_read_code_relocs(buf, &layout, code_out);
    CHECK_EQ(code_out[0], 8u);
    CHECK_EQ(code_out[1], 12u);
    CHECK_EQ(code_out[2], 16u);
}

static void test_end_to_end_with_real_relocate(void)
{
    /* The actual intended use: parse the layout, then relocate data words against one base
     * and code words (e.g. a stored function pointer) against a different one. */
    uint8_t buf[64];
    uint32_t data_relocs[] = {0};   /* e.g. a global's address */
    uint32_t code_relocs[] = {4};  /* e.g. a stored function pointer */
    uint32_t total = build_payload(buf, 8, data_relocs, 1, code_relocs, 1, 16);

    purr_module_layout_t layout;
    purr_module_layout_result_t r = purr_module_parse_layout(buf, total, &layout);
    CHECK_EQ(r, PURR_MOD_LAYOUT_OK);

    uint32_t data_off[1], code_off[1];
    purr_module_read_data_relocs(buf, &layout, data_off);
    purr_module_read_code_relocs(buf, &layout, code_off);

    uint8_t code[32];
    memset(code, 0, sizeof(code));
    memcpy(code, buf + layout.code_offset, layout.code_size);

    purr_reloc_result_t rr1 = purr_relocate(code, sizeof(code), data_off, layout.data_reloc_count, 0x3c000000u);
    CHECK_EQ(rr1, PURR_RELOC_OK);
    purr_reloc_result_t rr2 = purr_relocate(code, sizeof(code), code_off, layout.code_reloc_count, 0x42000000u);
    CHECK_EQ(rr2, PURR_RELOC_OK);

    uint32_t w;
    memcpy(&w, code + 0, 4); CHECK_EQ(w, 0x3c000000u); /* data word got the data base */
    memcpy(&w, code + 4, 4); CHECK_EQ(w, 0x42000000u); /* code word got the code base */
}

static void test_result_names(void)
{
    CHECK(strcmp(purr_module_layout_result_name(PURR_MOD_LAYOUT_OK), "ok") == 0);
    CHECK(strcmp(purr_module_layout_result_name(PURR_MOD_LAYOUT_TOO_SHORT), "too-short") == 0);
    CHECK(strcmp(purr_module_layout_result_name(PURR_MOD_LAYOUT_TOO_MANY), "too-many-relocs") == 0);
    CHECK(strcmp(purr_module_layout_result_name(PURR_MOD_LAYOUT_BAD_SIZE), "bad-size") == 0);
    CHECK(strcmp(purr_module_layout_result_name(PURR_MOD_LAYOUT_BAD_ENTRY), "bad-entry") == 0);
}

int main(void)
{
    test_basic_layout();
    test_no_relocations_at_all();
    test_too_short_for_fixed_header();
    test_data_reloc_table_past_end_rejected();
    test_too_many_data_relocs_rejected();
    test_code_reloc_table_past_end_rejected();
    test_too_many_code_relocs_rejected();
    test_entry_offset_past_code_rejected();
    test_entry_offset_at_last_byte_ok();
    test_read_relocs_roundtrip();
    test_end_to_end_with_real_relocate();
    test_result_names();
    TK_DONE("test_module");
}
