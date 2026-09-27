#include <stdio.h>
#include <string.h>

#include "purr_manifest.h"
#include "testkit.h"

/* 64 hex characters: 32 bytes of 0xAB. */
#define HASH64 "abababababababababababababababababababababababababababababababab"

static void expect_hash(const uint8_t *got)
{
    for (int i = 0; i < 32; i++) {
        CHECK_EQ(got[i], 0xAB);
    }
}

static void test_basic_parse(void)
{
    const char *text =
        "release=1.2.0\n"
        "released=2026-09-27\n"
        "\n"
        "component=coreos\n"
        "version=1.2.0\n"
        "chip=esp32s3\n"
        "board=tdeck_plus\n"
        "file=coreos-tdeck_plus-1.2.0.kitt\n"
        "size=245760\n"
        "sha256=" HASH64 "\n"
        "key=system\n"
        "min_bootloader=1.0.0\n"
        "min_coreos=1.0.0\n"
        "\n"
        "component=kittenos\n"
        "version=1.2.0\n"
        "file=kittenos-tdeck_plus-1.2.0.kitt\n"
        "size=999424\n"
        "sha256=" HASH64 "\n"
        "key=system\n";

    purr_manifest_t m;
    int n = purr_manifest_parse(&m, text, strlen(text));
    CHECK_EQ(n, 2);
    CHECK_EQ(m.count, 2);
    CHECK_EQ(m.dropped, 0);
    CHECK_EQ(m.truncated, 0);
    CHECK(strcmp(m.release, "1.2.0") == 0);
    CHECK(strcmp(m.released, "2026-09-27") == 0);

    const purr_manifest_entry_t *e = purr_manifest_find(&m, "coreos", "esp32s3", "tdeck_plus");
    CHECK(e != NULL);
    CHECK(strcmp(e->version, "1.2.0") == 0);
    CHECK(strcmp(e->file, "coreos-tdeck_plus-1.2.0.kitt") == 0);
    CHECK_EQ(e->size, 245760u);
    expect_hash(e->sha256);
    CHECK(strcmp(e->key_role, "system") == 0);
    CHECK(strcmp(e->min_bootloader, "1.0.0") == 0);

    /* Fields left out default to "any" (matches every chip and board). */
    const purr_manifest_entry_t *k = purr_manifest_find(&m, "kittenos", "esp32", "cyd_24c");
    CHECK(k != NULL);
    CHECK(strcmp(k->chip, "any") == 0);
    CHECK(strcmp(k->board, "any") == 0);
}

static void test_find_misses(void)
{
    const char *text =
        "component=coreos\nversion=1.0.0\nchip=esp32s3\nboard=tdeck_plus\n"
        "file=a.kitt\nsize=10\nsha256=" HASH64 "\n";
    purr_manifest_t m;
    purr_manifest_parse(&m, text, strlen(text));
    CHECK(purr_manifest_find(&m, "coreos", "esp32", "tdeck_plus") == NULL);    /* wrong chip */
    CHECK(purr_manifest_find(&m, "coreos", "esp32s3", "cyd_24c") == NULL);     /* wrong board */
    CHECK(purr_manifest_find(&m, "kernel", "esp32s3", "tdeck_plus") == NULL);  /* not listed */
    CHECK(purr_manifest_find(&m, "coreos", NULL, NULL) != NULL);              /* NULL matches anything */
}

static void test_comments_and_blank_runs(void)
{
    const char *text =
        "# a release manifest\n"
        "release=1.0.0\n"
        "\n"
        "\n"
        "# the kernel\n"
        "component=kernel\n"
        "version=1.0.0\n"
        "file=k.kitt\n"
        "size=1000\n"
        "sha256=" HASH64 "\n"
        "\n";
    purr_manifest_t m;
    int n = purr_manifest_parse(&m, text, strlen(text));
    CHECK_EQ(n, 1);
    CHECK(strcmp(m.entries[0].component, "kernel") == 0);
}

static void test_missing_required_fields_are_dropped(void)
{
    const char *cases[] = {
        "component=a\nversion=1\nfile=f\nsize=1\n",                        /* no sha256 */
        "component=a\nversion=1\nfile=f\nsha256=" HASH64 "\n",             /* no size */
        "component=a\nversion=1\nsize=1\nsha256=" HASH64 "\n",             /* no file */
        "component=a\nfile=f\nsize=1\nsha256=" HASH64 "\n",                /* no version */
        "component=a\nversion=1\nfile=f\nsize=0\nsha256=" HASH64 "\n",     /* size zero */
        "component=a\nversion=1\nfile=f\nsize=1\nsha256=tooshort\n",       /* bad hash */
        "component=a\nversion=1\nfile=f\nsize=notanumber\nsha256=" HASH64 "\n",  /* bad size */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        purr_manifest_t m;
        purr_manifest_parse(&m, cases[i], strlen(cases[i]));
        CHECK_EQ(m.count, 0);
        CHECK_EQ(m.dropped, 1);
    }
}

static void test_header_only_is_not_an_entry(void)
{
    const char *text = "release=1.0.0\nreleased=2026-01-01\n";
    purr_manifest_t m;
    purr_manifest_parse(&m, text, strlen(text));
    CHECK_EQ(m.count, 0);
    CHECK_EQ(m.dropped, 0);                          /* a header stanza, not a bad entry */
}

static void test_unknown_keys_ignored(void)
{
    const char *text =
        "component=a\nversion=1\nfile=f\nsize=1\nsha256=" HASH64 "\n"
        "channel=preview\nsomething_from_the_future=42\n";
    purr_manifest_t m;
    int n = purr_manifest_parse(&m, text, strlen(text));
    CHECK_EQ(n, 1);
    CHECK_EQ(m.dropped, 0);
}

static void test_crlf_and_no_trailing_blank_line(void)
{
    const char *text = "component=a\r\nversion=1\r\nfile=f\r\nsize=1\r\nsha256=" HASH64 "\r\n";
    purr_manifest_t m;
    int n = purr_manifest_parse(&m, text, strlen(text));
    CHECK_EQ(n, 1);
    CHECK(strcmp(m.entries[0].file, "f") == 0);       /* no stray \r left in the value */
}

static void test_lines_without_equals_are_ignored(void)
{
    const char *text =
        "not a key value line\n"
        "component=a\nversion=1\nfile=f\nsize=1\nsha256=" HASH64 "\n"
        "also garbage\n";
    purr_manifest_t m;
    int n = purr_manifest_parse(&m, text, strlen(text));
    CHECK_EQ(n, 1);
}

static void test_too_many_entries(void)
{
    char text[8000];
    int pos = 0;
    for (int i = 0; i < PURR_MANIFEST_MAX_ENTRIES + 3; i++) {
        pos += snprintf(text + pos, sizeof(text) - pos,
                        "component=c%d\nversion=1\nfile=f%d\nsize=1\nsha256=" HASH64 "\n\n", i, i);
    }
    purr_manifest_t m;
    int n = purr_manifest_parse(&m, text, (size_t)pos);
    CHECK_EQ(n, PURR_MANIFEST_MAX_ENTRIES);
    CHECK_EQ(m.truncated, 1);
    CHECK_EQ(m.dropped, 0);
}

static void test_empty_input(void)
{
    purr_manifest_t m;
    int n = purr_manifest_parse(&m, "", 0);
    CHECK_EQ(n, 0);
    CHECK_EQ(m.count, 0);
    CHECK_EQ(m.dropped, 0);
}

int main(void)
{
    test_basic_parse();
    test_find_misses();
    test_comments_and_blank_runs();
    test_missing_required_fields_are_dropped();
    test_header_only_is_not_an_entry();
    test_unknown_keys_ignored();
    test_crlf_and_no_trailing_blank_line();
    test_lines_without_equals_are_ignored();
    test_too_many_entries();
    test_empty_input();
    TK_DONE("test_manifest");
}
