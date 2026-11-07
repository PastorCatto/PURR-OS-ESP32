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

/* a kernel that has the shell's four names plus others, in another order than the shell's list, with the layout facts the shell carries and one more */
static const char *const kernel_names[] = {"zeta", "delta", "extra", "beta", "alpha", "gamma"};
static const void *const kernel_exports[] = {(void *)(uintptr_t)0x42000001u, (void *)(uintptr_t)0x42000002u, (void *)(uintptr_t)0x42000003u,
                                             (void *)(uintptr_t)0x42000004u, (void *)(uintptr_t)0x42000005u, (void *)(uintptr_t)0x42000006u};
static const purr_shellhdr_pair_t kernel_layout[] = {{0, 0x00090200}, {1, 7}, {2, 24}, {3, 99}};
#define NN ((uint32_t)(sizeof(kernel_names) / sizeof(kernel_names[0])))
#define NL ((uint32_t)(sizeof(kernel_layout) / sizeof(kernel_layout[0])))

static void test_parse_reads_what_the_build_tool_wrote(void)
{
    purr_shellhdr_t h;
    CHECK_EQ(purr_shellhdr_parse(shell_fixture, sizeof(shell_fixture), FIXTURE_ABI, &h), PURR_SHELLHDR_OK);
    CHECK_EQ(h.abi_version, (uint32_t)FIXTURE_ABI);
    CHECK_EQ(h.bss_size, 0x1200u);
    CHECK_EQ(h.import_count, 2u);
    CHECK_EQ(h.layout_count, 3u);
    CHECK_EQ(h.header_size, (uint32_t)FIXTURE_HEAD);
    CHECK(strcmp(h.built_for, "1.0.32") == 0);
}

static void test_a_kernel_with_other_exports_and_a_newer_layout_list_accepts_the_shell(void)
{
    purr_shellhdr_t h;
    const char *problem = (const char *)1;
    CHECK_EQ(purr_shellhdr_parse(shell_fixture, sizeof(shell_fixture), FIXTURE_ABI, &h), PURR_SHELLHDR_OK);
    CHECK_EQ(purr_shellhdr_check(shell_fixture, sizeof(shell_fixture), &h, kernel_names, NN, kernel_layout, NL, &problem), PURR_SHELLHDR_OK);
    CHECK(problem == NULL);
}

static void test_patch_finds_each_name_by_name_not_by_position(void)
{
    uint8_t code[sizeof(shell_fixture)];
    memcpy(code, shell_fixture, sizeof(code));
    purr_shellhdr_t h;
    CHECK_EQ(purr_shellhdr_parse(code, sizeof(code), FIXTURE_ABI, &h), PURR_SHELLHDR_OK);
    CHECK_EQ(purr_shellhdr_patch(code, sizeof(code), &h, kernel_names, NN, kernel_exports), PURR_SHELLHDR_OK);
    CHECK_EQ(word(code, FIXTURE_HEAD + 4 * 2), 0x42000002u);     /* "delta" is entry 1 in this kernel (it was entry 3 in the shell's list) */
    CHECK_EQ(word(code, FIXTURE_HEAD + 4 * 5), 0x42000005u);     /* "alpha" is entry 4 here */
    CHECK_EQ(word(code, FIXTURE_HEAD + 0), 0x11111111u);         /* the ordinary constants are untouched */
    CHECK_EQ(word(code, FIXTURE_HEAD + 4 * 3), 0x33333333u);
    CHECK_EQ(word(code, FIXTURE_HEAD + 4 * 7), 0u);
}

static void test_a_missing_name_means_the_shell_needs_a_newer_kernel_and_nothing_is_written(void)
{
    uint8_t code[sizeof(shell_fixture)];
    memcpy(code, shell_fixture, sizeof(code));
    purr_shellhdr_t h;
    const char *problem = NULL;
    static const char *const no_delta[] = {"zeta", "extra", "beta", "alpha", "gamma"};
    CHECK_EQ(purr_shellhdr_parse(code, sizeof(code), FIXTURE_ABI, &h), PURR_SHELLHDR_OK);
    CHECK_EQ(purr_shellhdr_check(code, sizeof(code), &h, no_delta, 5, kernel_layout, NL, &problem), PURR_SHELLHDR_NEEDS_NEWER);
    CHECK(problem != NULL && strcmp(problem, "delta") == 0);
    CHECK_EQ(purr_shellhdr_patch(code, sizeof(code), &h, no_delta, 5, kernel_exports), PURR_SHELLHDR_NEEDS_NEWER);
    CHECK_EQ(memcmp(code, shell_fixture, sizeof(code)), 0);
}

static void test_a_layout_difference_is_refused(void)
{
    purr_shellhdr_t h;
    CHECK_EQ(purr_shellhdr_parse(shell_fixture, sizeof(shell_fixture), FIXTURE_ABI, &h), PURR_SHELLHDR_OK);
    purr_shellhdr_pair_t other_size[] = {{0, 0x00090200}, {1, 7}, {2, 28}};                  /* a structure grew */
    CHECK_EQ(purr_shellhdr_check(shell_fixture, sizeof(shell_fixture), &h, kernel_names, NN, other_size, 3, NULL), PURR_SHELLHDR_LAYOUT_MISMATCH);
    purr_shellhdr_pair_t other_lvgl[] = {{0, 0x00090300}, {1, 7}, {2, 24}};                  /* another LVGL */
    CHECK_EQ(purr_shellhdr_check(shell_fixture, sizeof(shell_fixture), &h, kernel_names, NN, other_lvgl, 3, NULL), PURR_SHELLHDR_LAYOUT_MISMATCH);
    purr_shellhdr_pair_t other_rev[] = {{0, 0x00090200}, {1, 8}, {2, 24}};                   /* the manual revision was bumped */
    CHECK_EQ(purr_shellhdr_check(shell_fixture, sizeof(shell_fixture), &h, kernel_names, NN, other_rev, 3, NULL), PURR_SHELLHDR_LAYOUT_MISMATCH);
    purr_shellhdr_pair_t fewer[] = {{0, 0x00090200}, {1, 7}};                                /* the shell carries an id this kernel does not know */
    CHECK_EQ(purr_shellhdr_check(shell_fixture, sizeof(shell_fixture), &h, kernel_names, NN, fewer, 2, NULL), PURR_SHELLHDR_LAYOUT_MISMATCH);
}

static void test_another_abi_or_not_a_shell_or_a_short_file_is_refused(void)
{
    purr_shellhdr_t h;
    CHECK_EQ(purr_shellhdr_parse(shell_fixture, sizeof(shell_fixture), FIXTURE_ABI + 1, &h), PURR_SHELLHDR_BAD_ABI);
    uint8_t bad[sizeof(shell_fixture)];
    memcpy(bad, shell_fixture, sizeof(bad));
    bad[0] ^= 0xFF;
    CHECK_EQ(purr_shellhdr_parse(bad, sizeof(bad), FIXTURE_ABI, &h), PURR_SHELLHDR_BAD_MAGIC);
    CHECK_EQ(purr_shellhdr_parse(shell_fixture, 20, FIXTURE_ABI, &h), PURR_SHELLHDR_TOO_SHORT);
    CHECK_EQ(purr_shellhdr_parse(shell_fixture, FIXTURE_HEAD - 1, FIXTURE_ABI, &h), PURR_SHELLHDR_TOO_SHORT);
    memcpy(bad, shell_fixture, sizeof(bad));
    bad[12] = 0xFF;                                                                          /* more import sites than capacity */
    CHECK_EQ(purr_shellhdr_parse(bad, sizeof(bad), FIXTURE_ABI, &h), PURR_SHELLHDR_DAMAGED);
}

static void test_a_bad_import_site_or_name_writes_nothing(void)
{
    uint8_t code[sizeof(shell_fixture)];
    memcpy(code, shell_fixture, sizeof(code));
    purr_shellhdr_t h;
    CHECK_EQ(purr_shellhdr_parse(code, sizeof(code), FIXTURE_ABI, &h), PURR_SHELLHDR_OK);
    uint32_t bad;
    bad = (uint32_t)sizeof(code);                                                            /* the second site: past the end */
    memcpy(code + h.imports_offset + 8, &bad, 4);
    CHECK_EQ(purr_shellhdr_patch(code, sizeof(code), &h, kernel_names, NN, kernel_exports), PURR_SHELLHDR_BAD_IMPORT);
    CHECK_EQ(word(code, FIXTURE_HEAD + 4 * 2), word(shell_fixture, FIXTURE_HEAD + 4 * 2));   /* the first site was not patched either */
    memcpy(code, shell_fixture, sizeof(code));
    bad = FIXTURE_HEAD + 5;                                                                  /* misaligned */
    memcpy(code + h.imports_offset + 8, &bad, 4);
    CHECK_EQ(purr_shellhdr_patch(code, sizeof(code), &h, kernel_names, NN, kernel_exports), PURR_SHELLHDR_BAD_IMPORT);
    memcpy(code, shell_fixture, sizeof(code));
    bad = 8;                                                                                 /* inside the header itself */
    memcpy(code + h.imports_offset + 8, &bad, 4);
    CHECK_EQ(purr_shellhdr_patch(code, sizeof(code), &h, kernel_names, NN, kernel_exports), PURR_SHELLHDR_BAD_IMPORT);
    memcpy(code, shell_fixture, sizeof(code));
    bad = 0x7FFF;                                                                            /* a name offset that is not in the names block */
    memcpy(code + h.imports_offset + 12, &bad, 4);
    CHECK_EQ(purr_shellhdr_patch(code, sizeof(code), &h, kernel_names, NN, kernel_exports), PURR_SHELLHDR_DAMAGED);
}

int main(void)
{
    test_parse_reads_what_the_build_tool_wrote();
    test_a_kernel_with_other_exports_and_a_newer_layout_list_accepts_the_shell();
    test_patch_finds_each_name_by_name_not_by_position();
    test_a_missing_name_means_the_shell_needs_a_newer_kernel_and_nothing_is_written();
    test_a_layout_difference_is_refused();
    test_another_abi_or_not_a_shell_or_a_short_file_is_refused();
    test_a_bad_import_site_or_name_writes_nothing();
    TK_DONE("test_shellhdr");
}
