#include <string.h>

#include "purr_shellhdr.h"
#include "shellhdr_fixture.h"
#include "testkit.h"

static uint32_t word(const uint8_t *p, uint32_t off)
{
    uint32_t v;
    memcpy(&v, p + off, 4);
    return v;
}

static void test_parse_reads_what_the_build_tool_wrote(void)
{
    purr_shellhdr_t h;
    CHECK_EQ(purr_shellhdr_parse(shell_fixture, sizeof(shell_fixture), FIXTURE_ABI, 183, &h), PURR_SHELLHDR_OK);
    CHECK_EQ(h.abi_version, (uint32_t)FIXTURE_ABI);
    CHECK_EQ(h.export_count, (uint32_t)FIXTURE_EXPORTS);
    CHECK_EQ(h.bss_size, 0x1200u);
    CHECK_EQ(h.import_count, 2u);
    CHECK_EQ(h.header_size, (uint32_t)FIXTURE_HEAD);
}

static void test_patch_writes_real_addresses_over_the_placeholders_only(void)
{
    uint8_t code[sizeof(shell_fixture)];
    memcpy(code, shell_fixture, sizeof(code));
    purr_shellhdr_t h;
    CHECK_EQ(purr_shellhdr_parse(code, sizeof(code), FIXTURE_ABI, 183, &h), PURR_SHELLHDR_OK);
    const void *exports[4] = {(void *)(uintptr_t)0x42000010u, (void *)(uintptr_t)0x42000020u, (void *)(uintptr_t)0x42000030u, (void *)(uintptr_t)0x42000040u};
    CHECK_EQ(purr_shellhdr_patch(code, sizeof(code), &h, exports, 4), PURR_SHELLHDR_OK);
    CHECK_EQ(word(code, FIXTURE_HEAD + 4 * 2), 0x42000040u);     /* export 3 */
    CHECK_EQ(word(code, FIXTURE_HEAD + 4 * 5), 0x42000010u);     /* export 0 */
    CHECK_EQ(word(code, FIXTURE_HEAD + 0), 0x11111111u);         /* the ordinary constants are untouched */
    CHECK_EQ(word(code, FIXTURE_HEAD + 4 * 3), 0x33333333u);
    CHECK_EQ(word(code, FIXTURE_HEAD + 4 * 7), 0u);
}

static void test_a_shell_for_another_abi_or_a_longer_list_or_not_a_shell_is_refused(void)
{
    purr_shellhdr_t h;
    CHECK_EQ(purr_shellhdr_parse(shell_fixture, sizeof(shell_fixture), FIXTURE_ABI + 1, 183, &h), PURR_SHELLHDR_BAD_ABI);
    CHECK_EQ(purr_shellhdr_parse(shell_fixture, sizeof(shell_fixture), FIXTURE_ABI, 3, &h), PURR_SHELLHDR_NEEDS_NEWER);
    uint8_t bad[sizeof(shell_fixture)];
    memcpy(bad, shell_fixture, sizeof(bad));
    bad[0] ^= 0xFF;
    CHECK_EQ(purr_shellhdr_parse(bad, sizeof(bad), FIXTURE_ABI, 183, &h), PURR_SHELLHDR_BAD_MAGIC);
    CHECK_EQ(purr_shellhdr_parse(shell_fixture, 20, FIXTURE_ABI, 183, &h), PURR_SHELLHDR_TOO_SHORT);
    CHECK_EQ(purr_shellhdr_parse(shell_fixture, FIXTURE_HEAD - 1, FIXTURE_ABI, 183, &h), PURR_SHELLHDR_TOO_SHORT);
}

static void test_a_bad_import_site_writes_nothing(void)
{
    uint8_t code[sizeof(shell_fixture)];
    memcpy(code, shell_fixture, sizeof(code));
    purr_shellhdr_t h;
    CHECK_EQ(purr_shellhdr_parse(code, sizeof(code), FIXTURE_ABI, 183, &h), PURR_SHELLHDR_OK);
    const void *exports[4] = {0, 0, 0, 0};
    uint32_t bad;
    /* second site: past the end of the code */
    bad = (uint32_t)sizeof(code);
    memcpy(code + h.imports_offset + 8, &bad, 4);
    CHECK_EQ(purr_shellhdr_patch(code, sizeof(code), &h, exports, 4), PURR_SHELLHDR_BAD_IMPORT);
    CHECK_EQ(word(code, FIXTURE_HEAD + 4 * 2), word(shell_fixture, FIXTURE_HEAD + 4 * 2));      /* the first site was not patched either */
    /* misaligned */
    memcpy(code, shell_fixture, sizeof(code));
    bad = FIXTURE_HEAD + 5;
    memcpy(code + h.imports_offset + 8, &bad, 4);
    CHECK_EQ(purr_shellhdr_patch(code, sizeof(code), &h, exports, 4), PURR_SHELLHDR_BAD_IMPORT);
    /* inside the header itself */
    memcpy(code, shell_fixture, sizeof(code));
    bad = 8;
    memcpy(code + h.imports_offset + 8, &bad, 4);
    CHECK_EQ(purr_shellhdr_patch(code, sizeof(code), &h, exports, 4), PURR_SHELLHDR_BAD_IMPORT);
    /* an export index that does not exist */
    memcpy(code, shell_fixture, sizeof(code));
    bad = 9;
    memcpy(code + h.imports_offset + 12, &bad, 4);
    CHECK_EQ(purr_shellhdr_patch(code, sizeof(code), &h, exports, 4), PURR_SHELLHDR_BAD_IMPORT);
}

int main(void)
{
    test_parse_reads_what_the_build_tool_wrote();
    test_patch_writes_real_addresses_over_the_placeholders_only();
    test_a_shell_for_another_abi_or_a_longer_list_or_not_a_shell_is_refused();
    test_a_bad_import_site_writes_nothing();
    TK_DONE("test_shellhdr");
}
