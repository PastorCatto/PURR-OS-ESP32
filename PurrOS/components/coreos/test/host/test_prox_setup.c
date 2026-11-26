/* Remote setup: the admin's messages and the node's rules (what it accepts, what it refuses, that an account is never replaced). */
#include <stdio.h>
#include <string.h>

#include "testkit.h"
#include "purr_px_setup.h"

static struct { int has_account; char name[40], user[40], pass[80], ssid[40], wpass[80], app[40]; int done, fail_wifi, installs; } dev;

static int o_has(void *c) { (void)c; return dev.has_account; }
static int o_name(void *c, const char *n) { (void)c; snprintf(dev.name, sizeof(dev.name), "%s", n); return 0; }
static int o_acct(void *c, const char *u, const char *p) { (void)c; snprintf(dev.user, sizeof(dev.user), "%s", u); snprintf(dev.pass, sizeof(dev.pass), "%s", p); dev.has_account = 1; return 0; }
static int o_wifi(void *c, const char *s, const char *p) { (void)c; if (dev.fail_wifi) return -1; snprintf(dev.ssid, sizeof(dev.ssid), "%s", s); snprintf(dev.wpass, sizeof(dev.wpass), "%s", p); return 0; }
static int o_inst(void *c, const char *a) { (void)c; snprintf(dev.app, sizeof(dev.app), "%s", a); dev.installs++; return 0; }
static void o_done(void *c) { (void)c; dev.done++; }
static const purr_pxs_ops_t ops = {o_has, o_name, o_acct, o_wifi, o_inst, o_done};

static int send(purr_pxs_node_t *n, int len, const uint8_t *m, int *status)
{
    uint8_t out[8];
    int al = purr_pxs_node_rx(n, m, (size_t)len, out, sizeof(out));
    int type = -1, st = -1;
    CHECK_EQ(al, 3);
    CHECK_EQ(purr_pxs_parse_ack(out, (size_t)al, &type, &st), 0);
    *status = st;
    return type;
}

int main(void)
{
    purr_pxs_node_t node;
    uint8_t m[PURR_PXS_MAX_MSG + 8], out[64];
    int st;
    purr_pxs_node_init(&node, &ops, NULL);

    /* what a device with nothing set up asks for */
    int nl = purr_pxs_node_needs(&node, "HELTEC-3F2A", out, sizeof(out));
    unsigned flags;
    char nm[PURR_PXS_MAX_NAME + 1];
    CHECK(nl > 0 && purr_pxs_parse_needs(out, (size_t)nl, &flags, nm) == 0);
    CHECK(flags == (PURR_PXS_NEED_ACCOUNT | PURR_PXS_NEED_WIFI | PURR_PXS_NEED_NAME) && strcmp(nm, "HELTEC-3F2A") == 0);

    /* closed: everything is refused until the device opens setup */
    int l = purr_pxs_make_name("Ridge relay", m, sizeof(m));
    CHECK_EQ(send(&node, l, m, &st), PURR_PXS_SET_NAME);
    CHECK_EQ(st, PURR_PXS_REFUSED);
    CHECK(dev.name[0] == '\0');

    node.open = 1;
    CHECK_EQ(send(&node, l, m, &st), PURR_PXS_SET_NAME);
    CHECK(st == PURR_PXS_OK && strcmp(dev.name, "Ridge relay") == 0);
    l = purr_pxs_make_account("admin", "correct horse", m, sizeof(m));
    CHECK_EQ(send(&node, l, m, &st), PURR_PXS_SET_ACCT);
    CHECK(st == PURR_PXS_OK && strcmp(dev.user, "admin") == 0 && strcmp(dev.pass, "correct horse") == 0);
    l = purr_pxs_make_wifi("HomeNet", "", m, sizeof(m));                                   /* an open network: an empty password is fine for Wi-Fi */
    CHECK_EQ(send(&node, l, m, &st), PURR_PXS_SET_WIFI);
    CHECK(st == PURR_PXS_OK && strcmp(dev.ssid, "HomeNet") == 0);
    nl = purr_pxs_node_needs(&node, "Ridge relay", out, sizeof(out));
    CHECK(purr_pxs_parse_needs(out, (size_t)nl, &flags, nm) == 0 && flags == 0);           /* nothing missing now */
    l = purr_pxs_make_install("meshtastic", m, sizeof(m));
    CHECK_EQ(send(&node, l, m, &st), PURR_PXS_INSTALL);
    CHECK(st == PURR_PXS_OK && strcmp(dev.app, "meshtastic") == 0 && dev.installs == 1);

    /* a second account is refused, whoever asks: setup cannot take over a device that has one */
    l = purr_pxs_make_account("intruder", "pw", m, sizeof(m));
    CHECK_EQ(send(&node, l, m, &st), PURR_PXS_SET_ACCT);
    CHECK(st == PURR_PXS_REFUSED && strcmp(dev.user, "admin") == 0);

    /* a device that already had an account refuses even the first one */
    memset(&dev, 0, sizeof(dev));
    dev.has_account = 1;
    purr_pxs_node_init(&node, &ops, NULL);
    node.open = 1;
    CHECK_EQ(send(&node, l, m, &st), PURR_PXS_SET_ACCT);
    CHECK(st == PURR_PXS_REFUSED && dev.user[0] == '\0');
    nl = purr_pxs_node_needs(&node, "X", out, sizeof(out));
    purr_pxs_parse_needs(out, (size_t)nl, &flags, nm);
    CHECK((flags & PURR_PXS_NEED_ACCOUNT) == 0);

    /* the device failing is reported, not hidden */
    dev.fail_wifi = 1;
    l = purr_pxs_make_wifi("HomeNet", "pw", m, sizeof(m));
    CHECK_EQ(send(&node, l, m, &st), PURR_PXS_SET_WIFI);
    CHECK_EQ(st, PURR_PXS_FAILED);
    CHECK(!node.got_wifi);

    /* malformed messages are BAD and change nothing */
    uint8_t bad1[] = {PURR_PXS_SET_NAME, 10, 'a', 'b'};                                    /* length runs past the end */
    CHECK_EQ(send(&node, sizeof(bad1), bad1, &st), PURR_PXS_SET_NAME);
    CHECK_EQ(st, PURR_PXS_BAD);
    uint8_t bad2[] = {PURR_PXS_SET_NAME, 2, 'a', 'b', 'c'};                                /* bytes left over */
    CHECK_EQ(send(&node, sizeof(bad2), bad2, &st), PURR_PXS_SET_NAME);
    CHECK_EQ(st, PURR_PXS_BAD);
    uint8_t bad3[] = {PURR_PXS_SET_NAME, 2, 'a', 0x07};                                    /* a control character */
    CHECK_EQ(send(&node, sizeof(bad3), bad3, &st), PURR_PXS_SET_NAME);
    CHECK_EQ(st, PURR_PXS_BAD);
    uint8_t bad4[] = {0x77};
    CHECK_EQ(send(&node, sizeof(bad4), bad4, &st), 0x77);
    CHECK_EQ(st, PURR_PXS_BAD);
    uint8_t empty[1] = {0};
    int al = purr_pxs_node_rx(&node, empty, 0, out, sizeof(out));                          /* an empty message still gets an answer */
    CHECK_EQ(al, 3);
    CHECK(dev.name[0] == '\0');

    /* limits on the builders */
    char big[100];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    CHECK_EQ(purr_pxs_make_name(big, m, sizeof(m)), -1);
    CHECK_EQ(purr_pxs_make_name("", m, sizeof(m)), -1);
    CHECK_EQ(purr_pxs_make_account("", "pw", m, sizeof(m)), -1);
    CHECK_EQ(purr_pxs_make_account("admin", big, m, sizeof(m)), -1);
    CHECK_EQ(purr_pxs_make_wifi("ssid", "pw", m, 4), -1);
    CHECK_EQ(purr_pxs_make_name("a\tb", m, sizeof(m)), -1);

    /* done: applied once, setup closes, nothing more is accepted */
    CHECK_EQ(purr_pxs_make_done(m, sizeof(m)), 1);
    CHECK_EQ(send(&node, 1, m, &st), PURR_PXS_DONE);
    CHECK(st == PURR_PXS_OK && dev.done == 1 && !node.open);
    CHECK_EQ(send(&node, 1, m, &st), PURR_PXS_DONE);
    CHECK(st == PURR_PXS_REFUSED && dev.done == 1);
    TK_DONE("test_prox_setup");
}
