/* test_move.c - moving the OTA address: the parser and every rule of purr_move_check(). The signed files come from the SDK's Python tooling
 * (gen_move_fixture.py), so the two languages are checked against each other. */
#include <stdio.h>
#include <string.h>

#include "move_fixture.h"
#include "purr_move.h"
#include "purr_repo_trust.h"
#include "testcrypto.h"
#include "testkit.h"

#define OLD "https://github.com/PastorCatto/PURR-OS-ESP32/releases/download/JumpingJaguar1/recovery.manifest"
#define OLD_M "https://github.com/PastorCatto/PURR-OS-ESP32/releases/download/JumpingJaguar2/modules.manifest"

static purr_keybag_t s_bag;

static void setup_bag(uint32_t revoked)
{
    purr_key_t k[2];
    memset(k, 0, sizeof(k));
    k[0].key_id = 1;
    k[0].role = PURR_ROLE_BOOT;
    memcpy(k[0].pub, fxm_boot_pub, sizeof(k[0].pub));
    k[1].key_id = 2;
    k[1].role = PURR_ROLE_DEVELOPER;
    memcpy(k[1].pub, fxm_dev_pub, sizeof(k[1].pub));
    purr_keybag_build(&s_bag, k, 2, NULL);
    s_bag.revoked = revoked;
}

static purr_move_result_t check(const unsigned char *t, unsigned tl, const unsigned char *s, unsigned sl, const char *asked, uint32_t stored, purr_move_t *m)
{
    uint8_t sig[64];
    if (purr_repo_sig_decode((const char *)s, sl, sig) != 0) {
        return PURR_MOVE_BAD_SIGNATURE;
    }
    return purr_move_check(&purr_crypto_uecc, &s_bag, (const char *)t, tl, sig, asked, stored, m);
}

static void test_a_python_made_move_is_followed(void)
{
    purr_move_t m;
    setup_bag(0);
    CHECK_EQ(check(fxm_ok, fxm_ok_len, fxm_ok_sig, fxm_ok_sig_len, OLD, 0, &m), PURR_MOVE_OK);
    CHECK_EQ(m.seq, 2u);
    CHECK_EQ(m.key_id, 1);
    CHECK(strcmp(m.from, OLD) == 0);
    CHECK(strcmp(m.recovery, "https://example.org/hub/os/stable/recovery.manifest") == 0);
    CHECK(strcmp(m.modules, "https://example.org/hub/os/stable/modules.manifest") == 0);
    CHECK(strcmp(m.message, "Updates now come from the PURR hub.") == 0);
    CHECK_EQ(purr_move_is_default(&m, OLD, OLD_M), 0);
}

static void test_the_rules(void)
{
    purr_move_t m;
    setup_bag(0);
    CHECK_EQ(check(fxm_ok, fxm_ok_len, fxm_ok_sig, fxm_ok_sig_len, OLD, 2, &m), PURR_MOVE_OLD_SEQ);               /* the same seq again */
    CHECK_EQ(check(fxm_ok, fxm_ok_len, fxm_ok_sig, fxm_ok_sig_len, OLD, 1, &m), PURR_MOVE_OK);                     /* a lower one before */
    CHECK_EQ(check(fxm_ok, fxm_ok_len, fxm_ok_sig, fxm_ok_sig_len, OLD "x", 0, &m), PURR_MOVE_WRONG_FROM);          /* a file placed beside another manifest */
    CHECK_EQ(check(fxm_wrong_from, fxm_wrong_from_len, fxm_wrong_from_sig, fxm_wrong_from_sig_len, OLD, 0, &m), PURR_MOVE_WRONG_FROM);
    CHECK_EQ(check(fxm_http, fxm_http_len, fxm_http_sig, fxm_http_sig_len, OLD, 0, &m), PURR_MOVE_BAD_URL);        /* signed, but not https */
    CHECK_EQ(check(fxm_bad_name, fxm_bad_name_len, fxm_bad_name_sig, fxm_bad_name_sig_len, OLD, 0, &m), PURR_MOVE_BAD_URL);
    CHECK_EQ(check(fxm_dev_key, fxm_dev_key_len, fxm_dev_key_sig, fxm_dev_key_sig_len, OLD, 0, &m), PURR_MOVE_WRONG_ROLE);   /* only a boot-role key may move the address */
    CHECK_EQ(check(fxm_unknown_key, fxm_unknown_key_len, fxm_unknown_key_sig, fxm_unknown_key_sig_len, OLD, 0, &m), PURR_MOVE_NO_KEY);
    setup_bag(1u << 1);
    CHECK_EQ(check(fxm_ok, fxm_ok_len, fxm_ok_sig, fxm_ok_sig_len, OLD, 0, &m), PURR_MOVE_REVOKED);
    setup_bag(0);
}

static void test_a_changed_file_or_signature_is_refused(void)
{
    purr_move_t m;
    unsigned char t[512], s[160];
    setup_bag(0);
    memcpy(t, fxm_ok, fxm_ok_len);
    t[fxm_ok_len - 3] ^= 1;                                                                                            /* one character of the message */
    CHECK_EQ(check(t, fxm_ok_len, fxm_ok_sig, fxm_ok_sig_len, OLD, 0, &m), PURR_MOVE_BAD_SIGNATURE);
    memcpy(s, fxm_ok_sig, fxm_ok_sig_len);
    s[5] = s[5] == '0' ? '1' : '0';
    CHECK_EQ(check(fxm_ok, fxm_ok_len, s, fxm_ok_sig_len, OLD, 0, &m), PURR_MOVE_BAD_SIGNATURE);
    CHECK_EQ(check(fxm_ok, fxm_ok_len, fxm_boot_pub, 64, OLD, 0, &m), PURR_MOVE_BAD_SIGNATURE);                       /* not hex at all */
}

static void test_going_back_to_the_default(void)
{
    purr_move_t m;
    setup_bag(0);
    CHECK_EQ(check(fxm_back, fxm_back_len, fxm_back_sig, fxm_back_sig_len, "https://example.org/hub/os/stable/recovery.manifest", 2, &m), PURR_MOVE_OK);
    CHECK_EQ(purr_move_is_default(&m, OLD, OLD_M), 1);
}

static void test_the_parser_is_strict(void)
{
    purr_move_t m;
    const char *good = "moved=1\nseq=1\nkey_id=1\nfrom=https://a/recovery.manifest\nrecovery=https://b/recovery.manifest\nmodules=https://b/modules.manifest\nmessage=hi\n";
    CHECK_EQ(purr_move_parse(good, strlen(good), &m), 0);
    CHECK_EQ(purr_move_parse("", 0, &m), -1);
    CHECK_EQ(purr_move_parse("moved=2\nseq=1\nkey_id=1\nfrom=a\nrecovery=b\nmodules=c\n", 50, &m), -1);                 /* an unknown format */
    const char *no_seq = "moved=1\nkey_id=1\nfrom=a\nrecovery=b\nmodules=c\n";
    CHECK_EQ(purr_move_parse(no_seq, strlen(no_seq), &m), -1);
    const char *big = "moved=1\nseq=99999999999\nkey_id=1\nfrom=a\nrecovery=b\nmodules=c\n";
    CHECK_EQ(purr_move_parse(big, strlen(big), &m), -1);                                                                /* seq too large */
    const char *zero_key = "moved=1\nseq=1\nkey_id=0\nfrom=a\nrecovery=b\nmodules=c\n";
    CHECK_EQ(purr_move_parse(zero_key, strlen(zero_key), &m), -1);
    const char *ctl = "moved=1\nseq=1\nkey_id=1\nfrom=a\nrecovery=b\nmodules=c\nmessage=a\x01z\n";
    CHECK_EQ(purr_move_parse(ctl, strlen(ctl), &m), -1);                                                                /* a control character in the message */
    char longurl[400];
    memset(longurl, 'a', sizeof(longurl));
    memcpy(longurl, "moved=1\nseq=1\nkey_id=1\nfrom=", 28);
    strcpy(longurl + 300, "\nrecovery=b\nmodules=c\n");
    CHECK_EQ(purr_move_parse(longurl, strlen(longurl), &m), -1);                                                        /* an address of 270 characters */
    const char *extra = "unknown=1\nmoved=1\nseq=1\nkey_id=1\nfrom=a\nrecovery=b\nmodules=c\r\n";
    CHECK_EQ(purr_move_parse(extra, strlen(extra), &m), 0);                                                              /* unknown keys and CRLF are fine */
}

int main(void)
{
    test_a_python_made_move_is_followed();
    test_the_rules();
    test_a_changed_file_or_signature_is_refused();
    test_going_back_to_the_default();
    test_the_parser_is_strict();
    TK_DONE("test_move");
}
