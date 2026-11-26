/* Administering a node over the mesh: the signed request, the checks a node makes, and the answer in pieces. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "testkit.h"
#include "purr_mt_admin.h"
#include "uECC.h"

static int rng_fn(uint8_t *d, unsigned n) { for (unsigned i = 0; i < n; i++) d[i] = (uint8_t)rand(); return 1; }
static void e_random(uint8_t *o, int n) { rng_fn(o, (unsigned)n); }
static int e_keypair(uint8_t pub[64], uint8_t priv[32]) { return uECC_make_key(pub, priv, uECC_secp256r1()); }
static int e_shared(const uint8_t pub[64], const uint8_t priv[32], uint8_t o[32]) { return uECC_shared_secret(pub, priv, o, uECC_secp256r1()); }
static int e_sign(const uint8_t priv[32], const uint8_t h[32], uint8_t sig[64]) { return uECC_sign(priv, h, 32, sig, uECC_secp256r1()); }
static int e_verify(const uint8_t pub[64], const uint8_t h[32], const uint8_t sig[64]) { return uECC_verify(pub, h, 32, sig, uECC_secp256r1()); }
static const purr_px_ecc_t ecc = {.random = e_random, .keypair = e_keypair, .shared = e_shared, .sign = e_sign, .verify = e_verify};

static int g_ran;
static int cmd_status(void *ctx, int argc, char **argv, char *out, size_t cap) { (void)ctx; (void)argc; (void)argv; g_ran++; snprintf(out, cap, "up 12 s, heard 5"); return 0; }
static int cmd_radio(void *ctx, int argc, char **argv, char *out, size_t cap) { (void)ctx; g_ran++; snprintf(out, cap, "radio %s%s%s", argv[0], argc > 1 ? " " : "", argc > 1 ? argv[1] : ""); return 0; }
static int cmd_fail(void *ctx, int argc, char **argv, char *out, size_t cap) { (void)ctx; (void)argc; (void)argv; snprintf(out, cap, "could not"); return -1; }
static const purr_mta_cmd_t k_cmds[] = {
    {"status", "how the node is doing", 0, 0, cmd_status},
    {"radio", "radio get|set <what> [value]", 1, 2, cmd_radio},
    {"fail", "always fails", 0, 0, cmd_fail},
};

static void test_dispatch(void)
{
    char out[200];
    CHECK_EQ(purr_mta_dispatch(k_cmds, 3, NULL, "status", out, sizeof(out)), 0);
    CHECK(strcmp(out, "up 12 s, heard 5") == 0 && g_ran == 1);
    CHECK_EQ(purr_mta_dispatch(k_cmds, 3, NULL, "  status  ", out, sizeof(out)), 0);                    /* extra spaces */
    CHECK_EQ(purr_mta_dispatch(k_cmds, 3, NULL, "radio set power", out, sizeof(out)), 0);
    CHECK(strcmp(out, "radio set power") == 0);
    CHECK_EQ(purr_mta_dispatch(k_cmds, 3, NULL, "radio", out, sizeof(out)), PURR_MTA_E_BAD_ARGS);        /* too few words */
    CHECK_EQ(purr_mta_dispatch(k_cmds, 3, NULL, "status now", out, sizeof(out)), PURR_MTA_E_BAD_ARGS);   /* too many */
    int ran = g_ran;
    CHECK_EQ(purr_mta_dispatch(k_cmds, 3, NULL, "reboot", out, sizeof(out)), PURR_MTA_E_UNKNOWN_CMD);    /* nothing outside the table runs */
    CHECK_EQ(purr_mta_dispatch(k_cmds, 3, NULL, "rm -rf /", out, sizeof(out)), PURR_MTA_E_UNKNOWN_CMD);
    CHECK_EQ(purr_mta_dispatch(k_cmds, 3, NULL, "status; wipe", out, sizeof(out)), PURR_MTA_E_UNKNOWN_CMD);  /* "status;" is not "status", and a ; is not a separator here: nothing runs */
    CHECK_EQ(g_ran, ran);
    CHECK_EQ(purr_mta_dispatch(k_cmds, 3, NULL, "help", out, sizeof(out)), 0);
    CHECK(strstr(out, "status - how the node is doing") != NULL && strstr(out, "radio - radio get") != NULL);
    CHECK_EQ(purr_mta_dispatch(k_cmds, 3, NULL, "?", out, sizeof(out)), 0);
    CHECK_EQ(purr_mta_dispatch(k_cmds, 3, NULL, "fail", out, sizeof(out)), -1);                          /* the command's own failure comes back with its text */
    CHECK(strcmp(out, "could not") == 0);
    CHECK_EQ(purr_mta_dispatch(k_cmds, 3, NULL, "", out, sizeof(out)), PURR_MTA_E_BAD_ARGS);
    CHECK_EQ(purr_mta_dispatch(k_cmds, 3, NULL, "status\n", out, sizeof(out)), PURR_MTA_E_BAD_ARGS);     /* a control character */
    CHECK_EQ(purr_mta_dispatch(k_cmds, 3, NULL, "radio aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", out, sizeof(out)), PURR_MTA_E_BAD_ARGS);   /* a word over 40 */
    CHECK_EQ(purr_mta_dispatch(k_cmds, 3, NULL, "radio a b c d e f g h i j", out, sizeof(out)), PURR_MTA_E_BAD_ARGS);
    char small[8];
    CHECK_EQ(purr_mta_dispatch(k_cmds, 3, NULL, "help", small, sizeof(small)), 0);
    CHECK(strlen(small) < sizeof(small));                                                                 /* a small buffer is never overrun */
}

int main(void)
{
    test_dispatch();
    srand(11);
    uECC_set_rng(rng_fn);
    uint8_t apub[64], apriv[32], opub[64], opriv[32];
    CHECK(e_keypair(apub, apriv) == 1);
    CHECK(e_keypair(opub, opriv) == 1);                                 /* some other key */
    purr_mta_node_t node;
    purr_mta_node_init(&node);
    CHECK_EQ(purr_mta_add_admin(&node, apub), 0);
    CHECK_EQ(purr_mta_add_admin(&node, apub), 0);                       /* the same key twice is kept once */
    int used = 0;
    for (int i = 0; i < PURR_MTA_MAX_ADMINS; i++) used += node.admin[i].used;
    CHECK_EQ(used, 1);
    const uint32_t me = 0xF6FCDFE0;

    uint8_t req[PURR_MTA_REQ_MAX];
    char cmd[PURR_MTA_CMD_MAX + 1];
    uint16_t seq = 0;
    int n = purr_mta_make_request(&ecc, apriv, me, 5, 77, "status", req, sizeof(req));
    CHECK_EQ(n, 9 + 6 + 64);
    CHECK_EQ(purr_mta_check_request(&node, &ecc, me, req, (size_t)n, cmd, sizeof(cmd), &seq), 6);
    CHECK(strcmp(cmd, "status") == 0 && seq == 77);

    /* the same request again is a replay, and so is an older counter */
    CHECK_EQ(purr_mta_check_request(&node, &ecc, me, req, (size_t)n, cmd, sizeof(cmd), &seq), PURR_MTA_E_REPLAY);
    n = purr_mta_make_request(&ecc, apriv, me, 4, 78, "status", req, sizeof(req));
    CHECK_EQ(purr_mta_check_request(&node, &ecc, me, req, (size_t)n, cmd, sizeof(cmd), &seq), PURR_MTA_E_REPLAY);
    n = purr_mta_make_request(&ecc, apriv, me, 6, 79, "lora status", req, sizeof(req));
    CHECK_EQ(purr_mta_check_request(&node, &ecc, me, req, (size_t)n, cmd, sizeof(cmd), &seq), 11);

    /* meant for another node: refused here, whatever its signature */
    n = purr_mta_make_request(&ecc, apriv, me + 1, 7, 80, "reboot", req, sizeof(req));
    CHECK_EQ(purr_mta_check_request(&node, &ecc, me, req, (size_t)n, cmd, sizeof(cmd), &seq), PURR_MTA_E_SIGNATURE);

    /* signed by a key the node was not given */
    n = purr_mta_make_request(&ecc, opriv, me, 100, 81, "reboot", req, sizeof(req));
    CHECK_EQ(purr_mta_check_request(&node, &ecc, me, req, (size_t)n, cmd, sizeof(cmd), &seq), PURR_MTA_E_SIGNATURE);

    /* the command altered after signing, or one bit of the signature */
    n = purr_mta_make_request(&ecc, apriv, me, 8, 82, "reboot", req, sizeof(req));
    req[10] ^= 1;
    CHECK_EQ(purr_mta_check_request(&node, &ecc, me, req, (size_t)n, cmd, sizeof(cmd), &seq), PURR_MTA_E_SIGNATURE);
    req[10] ^= 1;
    req[n - 1] ^= 1;
    CHECK_EQ(purr_mta_check_request(&node, &ecc, me, req, (size_t)n, cmd, sizeof(cmd), &seq), PURR_MTA_E_SIGNATURE);
    req[n - 1] ^= 1;
    /* a forged request does not use up a counter: the real one with the same counter still works */
    CHECK_EQ(purr_mta_check_request(&node, &ecc, me, req, (size_t)n, cmd, sizeof(cmd), &seq), 6);

    /* malformed */
    CHECK_EQ(purr_mta_check_request(&node, &ecc, me, req, 5, cmd, sizeof(cmd), &seq), PURR_MTA_E_NOT_FOR_US);
    uint8_t other[20] = {9, 9, 9};
    CHECK_EQ(purr_mta_check_request(&node, &ecc, me, other, sizeof(other), cmd, sizeof(cmd), &seq), PURR_MTA_E_NOT_FOR_US);
    n = purr_mta_make_request(&ecc, apriv, me, 9, 83, "status", req, sizeof(req));
    CHECK_EQ(purr_mta_check_request(&node, &ecc, me, req, (size_t)n - 1, cmd, sizeof(cmd), &seq), PURR_MTA_E_FORMAT);   /* a length that does not match */
    char longcmd[PURR_MTA_CMD_MAX + 20];
    memset(longcmd, 'x', sizeof(longcmd) - 1);
    longcmd[sizeof(longcmd) - 1] = 0;
    CHECK_EQ(purr_mta_make_request(&ecc, apriv, me, 10, 1, longcmd, req, sizeof(req)), -1);
    CHECK_EQ(purr_mta_make_request(&ecc, apriv, me, 10, 1, "", req, sizeof(req)), -1);

    /* removing the key stops its commands */
    n = purr_mta_make_request(&ecc, apriv, me, 11, 84, "status", req, sizeof(req));
    CHECK_EQ(purr_mta_remove_admin(&node, apub), 1);
    CHECK_EQ(purr_mta_check_request(&node, &ecc, me, req, (size_t)n, cmd, sizeof(cmd), &seq), PURR_MTA_E_SIGNATURE);
    CHECK_EQ(purr_mta_remove_admin(&node, apub), 0);
    for (int i = 0; i < PURR_MTA_MAX_ADMINS; i++) {
        uint8_t k[64], p[32];
        CHECK(e_keypair(k, p) == 1);
        CHECK_EQ(purr_mta_add_admin(&node, k), 0);
    }
    CHECK_EQ(purr_mta_add_admin(&node, apub), PURR_MTA_E_FULL);

    /* an answer in pieces: short, empty, exactly one piece, several, out of order and repeated, another exchange's piece ignored */
    static char text[3000], back[3000];
    for (int i = 0; i < 2800; i++) text[i] = (char)('a' + i % 26);
    int sizes[] = {0, 40, PURR_MTA_RESP_DATA, PURR_MTA_RESP_DATA + 1, 2800};
    for (int s = 0; s < 5; s++) {
        size_t len = (size_t)sizes[s];
        int parts = purr_mta_answer_parts(len);
        CHECK(parts >= 1 && parts <= PURR_MTA_RESP_PARTS);
        purr_mta_answer_t a;
        purr_mta_answer_init(&a, 500 + (uint16_t)s);
        uint8_t piece[PURR_MTA_RESP_DATA + 5];
        int complete = 0;
        for (int k = parts - 1; k >= 0; k--) {                          /* the last piece first */
            int pl = purr_mta_make_answer(500 + (uint16_t)s, text, len, k, piece, sizeof(piece));
            CHECK(pl >= 5);
            complete = purr_mta_answer_push(&a, piece, (size_t)pl);
            if (k > 0) {
                CHECK_EQ(purr_mta_answer_push(&a, piece, (size_t)pl), 0);   /* a repeat changes nothing */
            }
        }
        CHECK_EQ(complete, 1);
        memcpy(back, a.text, a.len + 1u);
        CHECK(a.len == len && memcmp(back, text, len) == 0);
    }
    uint8_t piece[PURR_MTA_RESP_DATA + 5];
    int pl = purr_mta_make_answer(1, "hi", 2, 0, piece, sizeof(piece));
    purr_mta_answer_t a2;
    purr_mta_answer_init(&a2, 2);
    CHECK_EQ(purr_mta_answer_push(&a2, piece, (size_t)pl), 0);          /* another exchange */
    CHECK_EQ(purr_mta_answer_push(&a2, req, 3), -1);                    /* not an answer */
    CHECK_EQ(purr_mta_make_answer(1, text, PURR_MTA_RESP_PARTS * PURR_MTA_RESP_DATA + 1, 0, piece, sizeof(piece)), -1);   /* too long to send */
    TK_DONE("test_mt_admin");
}
