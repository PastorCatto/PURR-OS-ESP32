/* test_repo_trust.c - repositories: the release file, signatures, endorsements, tiers, the trusted list, and format-2 indexes.
 * The fixtures come from the SDK's Python tooling (gen_repo_fixture.py), so the two languages are checked against each other. */
#include <stdio.h>
#include <string.h>

#include "purr_repo_trust.h"
#include "purr_util.h"
#include "repo_fixture.h"
#include "testcrypto.h"
#include "testkit.h"

#define CHECK_STR(a, b) CHECK(strcmp((a), (b)) == 0)

static purr_keybag_t s_bag;

static void setup_bag(void)
{
    purr_key_t k;
    memset(&k, 0, sizeof(k));
    k.key_id = 1;
    k.role = PURR_ROLE_BOOT;
    memcpy(k.pub, fx_boot_pub, sizeof(k.pub));
    purr_keybag_build(&s_bag, &k, 1, NULL);
}

static void test_python_repository_verifies_in_c(void)
{
    purr_repo_info_t info;
    uint8_t sig[64], isig[64];
    char err[96];
    CHECK_EQ(purr_repo_info_parse((const char *)fx_comm_repo, fx_comm_repo_len, &info, err, sizeof(err)), 0);
    CHECK_STR(info.name, "Fixture Apps");
    CHECK_STR(info.maintainer, "tests");
    CHECK_EQ(info.flat, 1);
    CHECK_EQ(info.has_endorsement, 0);
    CHECK_EQ(info.updated, 1793300000u);
    CHECK_EQ(purr_repo_sig_decode((const char *)fx_comm_repo_sig, fx_comm_repo_sig_len, sig), 0);
    CHECK_EQ(purr_repo_info_check(&purr_crypto_uecc, &info, fx_comm_repo, fx_comm_repo_len, sig, err, sizeof(err)), 0);

    /* the signed index: the same key vouches for the package */
    CHECK_EQ(purr_repo_sig_decode((const char *)fx_comm_index_sig, fx_comm_index_sig_len, isig), 0);
    CHECK_EQ(purr_repo_check_signed(&purr_crypto_uecc, info.pub, fx_comm_index, fx_comm_index_len, isig), 0);
    purr_repo_index_t idx;
    purr_repo_kind_t store = PURR_REPO_STORE;
    CHECK_EQ(purr_repo_index_parse((const char *)fx_comm_index, fx_comm_index_len, &idx, &store, err, sizeof(err)), 0);
    CHECK_EQ(idx.format, 2);
    CHECK_EQ(idx.count, 2);
    CHECK_STR(idx.id, info.id);
    const purr_repo_app_t *clock = purr_repo_index_find(&idx, "clock");
    CHECK(clock != NULL);
    CHECK_EQ(clock->type, PURR_REPO_ITEM_APP);
    CHECK_EQ(clock->vouched, 1);
    CHECK_EQ(purr_repo_check_package(clock, fx_comm_pkg, fx_comm_pkg_len), PURR_REPO_PKG_OK);
    const purr_repo_app_t *font = purr_repo_index_find(&idx, "mono-font");
    CHECK(font != NULL);
    CHECK_EQ(font->type, PURR_REPO_ITEM_ASSET);
    CHECK_EQ(font->target, PURR_REPO_TARGET_ANY);
    CHECK_STR(font->dest, "fonts/mono.bin");
    uint8_t h[32];
    purr_sha256(fx_comm_pkg, fx_comm_pkg_len, h);
    CHECK(purr_repo_vouches(&idx, "clock", h));
    h[0] ^= 1;
    CHECK(!purr_repo_vouches(&idx, "clock", h));              /* a different package under the same name */
    purr_sha256(fx_comm_pkg, fx_comm_pkg_len, h);
    CHECK(!purr_repo_vouches(&idx, "other", h));
    CHECK(!purr_repo_vouches(&idx, "mono-font", h));         /* an asset is not an app */
}

static void test_tampering_is_caught(void)
{
    purr_repo_info_t info;
    uint8_t sig[64], isig[64];
    char err[96];
    CHECK_EQ(purr_repo_info_parse((const char *)fx_comm_repo, fx_comm_repo_len, &info, err, sizeof(err)), 0);
    purr_repo_sig_decode((const char *)fx_comm_repo_sig, fx_comm_repo_sig_len, sig);
    purr_repo_sig_decode((const char *)fx_comm_index_sig, fx_comm_index_sig_len, isig);
    static unsigned char index[sizeof(fx_comm_index)];
    memcpy(index, fx_comm_index, fx_comm_index_len);
    index[fx_comm_index_len / 2] ^= 1;                        /* one bit of the index */
    CHECK(purr_repo_check_signed(&purr_crypto_uecc, info.pub, index, fx_comm_index_len, isig) != 0);
    static unsigned char repo[sizeof(fx_comm_repo)];
    memcpy(repo, fx_comm_repo, fx_comm_repo_len);
    repo[fx_comm_repo_len - 5] ^= 1;
    purr_repo_info_t bad;
    int parsed = purr_repo_info_parse((const char *)repo, fx_comm_repo_len, &bad, err, sizeof(err));
    if (parsed == 0) {
        CHECK(purr_repo_info_check(&purr_crypto_uecc, &bad, repo, fx_comm_repo_len, sig, err, sizeof(err)) != 0);
    }
    /* a different repository's signature does not do */
    uint8_t other[64];
    purr_repo_sig_decode((const char *)fx_off_repo_sig, fx_off_repo_sig_len, other);
    CHECK(purr_repo_info_check(&purr_crypto_uecc, &info, fx_comm_repo, fx_comm_repo_len, other, err, sizeof(err)) != 0);
    /* an id that is not the key's fingerprint */
    purr_repo_info_t liar = info;
    liar.id[0] = liar.id[0] == 'a' ? 'b' : 'a';
    CHECK(purr_repo_info_check(&purr_crypto_uecc, &liar, fx_comm_repo, fx_comm_repo_len, sig, err, sizeof(err)) != 0);
    CHECK_STR(err, "the repository's id is not the fingerprint of its key");
}

static void test_endorsement_makes_it_official(void)
{
    setup_bag();
    purr_repo_info_t info;
    char err[96];
    CHECK_EQ(purr_repo_info_parse((const char *)fx_off_repo, fx_off_repo_len, &info, err, sizeof(err)), 0);
    CHECK_EQ(info.has_endorsement, 1);
    CHECK_EQ(info.end_key_id, 1);
    CHECK_EQ(purr_repo_endorsement_tier(&purr_crypto_uecc, &s_bag, &info), PURR_TIER_OFFICIAL);
    /* a device that does not have that key in its bag */
    purr_keybag_t empty;
    purr_keybag_build(&empty, NULL, 0, NULL);
    CHECK_EQ(purr_repo_endorsement_tier(&purr_crypto_uecc, &empty, &info), PURR_TIER_UNTRUSTED);
    /* an endorsement lifted onto another repository's key does not verify */
    purr_repo_info_t other;
    purr_repo_info_parse((const char *)fx_comm_repo, fx_comm_repo_len, &other, err, sizeof(err));
    other.has_endorsement = 1;
    other.end_key_id = info.end_key_id;
    memcpy(other.end_sig, info.end_sig, sizeof(other.end_sig));
    CHECK_EQ(purr_repo_endorsement_tier(&purr_crypto_uecc, &s_bag, &other), PURR_TIER_UNTRUSTED);
    /* no endorsement: community */
    CHECK_EQ(purr_repo_endorsement_tier(&purr_crypto_uecc, &s_bag, &(purr_repo_info_t){.has_endorsement = 0}), PURR_TIER_COMMUNITY);
}

static void test_release_file_refusals(void)
{
    purr_repo_info_t info;
    char err[96];
    CHECK(purr_repo_info_parse("", 0, &info, err, sizeof(err)) != 0);
    CHECK(purr_repo_info_parse("[1]", 3, &info, err, sizeof(err)) != 0);
    const char *no_key = "{\"format\":2,\"id\":\"0123456789abcdef\",\"name\":\"x\"}";
    CHECK(purr_repo_info_parse(no_key, strlen(no_key), &info, err, sizeof(err)) != 0);
    CHECK_STR(err, "the release file has no usable key");
    const char *old = "{\"format\":1,\"name\":\"x\",\"key\":{\"alg\":\"p256\",\"pub\":\"00\"}}";
    CHECK(purr_repo_info_parse(old, strlen(old), &info, err, sizeof(err)) != 0);
    char big[5000];
    memset(big, ' ', sizeof(big));
    CHECK(purr_repo_info_parse(big, sizeof(big), &info, err, sizeof(err)) != 0);
    uint8_t sig[64];
    CHECK(purr_repo_sig_decode("abcd", 4, sig) != 0);
    CHECK(purr_repo_sig_decode("zz", 2, sig) != 0);
}

static void test_the_trusted_list(void)
{
    purr_repo_info_t a, b;
    char err[96];
    purr_repo_info_parse((const char *)fx_comm_repo, fx_comm_repo_len, &a, err, sizeof(err));
    purr_repo_info_parse((const char *)fx_off_repo, fx_off_repo_len, &b, err, sizeof(err));
    purr_repos_t r;
    memset(&r, 0, sizeof(r));
    CHECK_EQ(purr_repos_classify(&r, &a, "https://example.org/a"), PURR_RA_NEW);
    CHECK_EQ(purr_repos_add(&r, &a, "https://example.org/a", PURR_TIER_COMMUNITY, 1000), 0);
    CHECK_EQ(purr_repos_add(&r, &b, "https://example.org/official", PURR_TIER_OFFICIAL, 1001), 0);
    CHECK_EQ(r.count, 2);
    CHECK_EQ(purr_repos_classify(&r, &a, "https://example.org/a"), PURR_RA_KNOWN);

    char text[1500];
    int n = purr_repos_format(&r, text, sizeof(text));
    CHECK(n > 0);
    purr_repos_t back;
    CHECK_EQ(purr_repos_parse(text, (size_t)n, &back), 2);
    CHECK_EQ(back.dropped, 0);
    CHECK_STR(back.e[0].id, a.id);
    CHECK_STR(back.e[1].name, "Fixture Apps");
    CHECK_EQ(back.e[1].tier, PURR_TIER_OFFICIAL);
    CHECK_EQ(back.e[0].added, 1000u);
    CHECK(memcmp(back.e[0].pub, a.pub, 64) == 0);
    CHECK(purr_repos_format(&r, text, 40) < 0);              /* does not fit: said so, not truncated */

    /* the same address under a different key is a changed key, not a second repository */
    purr_repos_remove(&r, b.id);
    CHECK_EQ(r.count, 1);
    CHECK_EQ(purr_repos_classify(&r, &b, "https://example.org/a"), PURR_RA_REKEYED);
    CHECK_EQ(purr_repos_add(&r, &b, "https://example.org/a", PURR_TIER_COMMUNITY, 2000), 0);
    CHECK_EQ(r.count, 1);                                      /* the old key's entry was replaced */
    CHECK(purr_repos_find(&r, a.id) == NULL);
    CHECK(purr_repos_find(&r, b.id) != NULL);
    CHECK(purr_repos_remove(&r, b.id) == 0);
    CHECK_EQ(r.count, 0);
    CHECK(purr_repos_remove(&r, "ffffffffffffffff") != 0);
}

static void test_the_trusted_list_is_untrusted_input(void)
{
    purr_repo_info_t a;
    char err[96];
    purr_repo_info_parse((const char *)fx_comm_repo, fx_comm_repo_len, &a, err, sizeof(err));
    purr_repos_t r;
    memset(&r, 0, sizeof(r));
    purr_repos_add(&r, &a, "https://example.org/a", PURR_TIER_COMMUNITY, 5);
    char text[1500];
    int n = purr_repos_format(&r, text, sizeof(text));
    /* flip a hex digit of the key: the id no longer matches it, so the stanza is dropped */
    char *pub = strstr(text, "pub=") + 4;
    *pub = *pub == '0' ? '1' : '0';
    purr_repos_t back;
    CHECK_EQ(purr_repos_parse(text, (size_t)n, &back), 0);
    CHECK_EQ(back.dropped, 1);
    /* a stanza with no tier */
    const char *nt = "id=0123456789abcdef\nname=x\nurl=https://x\n";
    CHECK_EQ(purr_repos_parse(nt, strlen(nt), &back), 0);
    /* garbage */
    CHECK_EQ(purr_repos_parse("\x01\x02garbage\n\n=\n", 14, &back), 0);
    CHECK_EQ(purr_repos_parse("", 0, &back), 0);
}

static void test_format2_index_rules(void)
{
    purr_repo_index_t idx;
    char err[96];
    purr_repo_kind_t store = PURR_REPO_STORE, sdk = PURR_REPO_SDK;
    const char *two =
        "{\"format\":2,\"repo\":{\"id\":\"x\",\"name\":\"n\",\"kind\":\"store\"},\"apps\":["
        "{\"name\":\"a\",\"version\":\"1.0\",\"size\":10,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"url\":\"a-1.0.cat\"},"
        "{\"name\":\"f\",\"version\":\"1.0\",\"type\":\"asset\",\"size\":10,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"url\":\"f.bin\",\"dest\":\"fonts/f.bin\"},"
        "{\"name\":\"nodest\",\"version\":\"1.0\",\"type\":\"asset\",\"size\":10,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"url\":\"g.bin\"},"
        "{\"name\":\"climb\",\"version\":\"1.0\",\"type\":\"extra\",\"size\":10,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"url\":\"h.bin\",\"dest\":\"../x\"},"
        "{\"name\":\"appdest\",\"version\":\"1.0\",\"size\":10,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"url\":\"i.cat\",\"dest\":\"x\"},"
        "{\"name\":\"weird\",\"version\":\"1.0\",\"type\":\"firmware\",\"size\":10,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"url\":\"j.bin\"}"
        "]}";
    CHECK_EQ(purr_repo_index_parse(two, strlen(two), &idx, &store, err, sizeof(err)), 0);
    CHECK_EQ(idx.count, 2);                                   /* the unsigned app and the asset: a signed format-2 index vouches for them */
    CHECK_EQ(idx.dropped, 4);                                 /* no dest, a climbing dest, a dest on an app, an unknown type */
    CHECK(purr_repo_index_find(&idx, "a") != NULL);
    CHECK_EQ(purr_repo_index_find(&idx, "a")->vouched, 1);
    /* format 2 is only for store repositories */
    CHECK(purr_repo_index_parse(two, strlen(two), &idx, &sdk, err, sizeof(err)) != 0);
    /* format 1 keeps its rule: a store offers signed apps only */
    const char *one =
        "{\"format\":1,\"repo\":{\"id\":\"x\",\"name\":\"n\",\"kind\":\"store\"},\"apps\":["
        "{\"name\":\"a\",\"version\":\"1.0\",\"size\":10,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"url\":\"a.cat\"}]}";
    CHECK_EQ(purr_repo_index_parse(one, strlen(one), &idx, &store, err, sizeof(err)), 0);
    CHECK_EQ(idx.count, 0);
}

int main(void)
{
    test_python_repository_verifies_in_c();
    test_tampering_is_caught();
    test_endorsement_makes_it_official();
    test_release_file_refusals();
    test_the_trusted_list();
    test_the_trusted_list_is_untrusted_input();
    test_format2_index_rules();
    TK_DONE("test_repo_trust");
}
