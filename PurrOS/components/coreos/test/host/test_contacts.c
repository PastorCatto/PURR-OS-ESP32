/* test_contacts.c - the address book (purr_contacts.c). */
#include <stdlib.h>
#include <string.h>

#include "purr_contacts.h"
#include "testkit.h"

static void test_book(void)
{
    purr_contacts_t b;
    purr_ct_init(&b);
    int32_t sam = purr_ct_add(&b, "Sam Rivera"), alex = purr_ct_add(&b, "alex Chen"), dana = purr_ct_add(&b, "Dana | Whitlock;x=y");
    CHECK(sam > 0 && alex > 0 && dana > 0);
    CHECK_EQ(purr_ct_add(&b, ""), -1);
    CHECK(!strcmp(purr_ct_get(&b, (uint32_t)dana)->name, "Dana   Whitlock x y"));              /* | ; = replaced by spaces */
    CHECK_EQ(purr_ct_set_addr(&b, (uint32_t)sam, "meshtastic", "!a1b2c3d4"), 0);
    CHECK_EQ(purr_ct_set_addr(&b, (uint32_t)sam, "reticulum", "7f3e9a21"), 0);
    CHECK_EQ(purr_ct_set_addr(&b, (uint32_t)sam, "meshtastic", "!ffffffff"), 0);               /* replaces */
    CHECK_EQ(purr_ct_get(&b, (uint32_t)sam)->naddr, 2);
    CHECK_EQ(purr_ct_set_addr(&b, (uint32_t)sam, "Bad Key", "x"), -1);
    CHECK_EQ(purr_ct_set_addr(&b, 999, "meshcore", "x"), -1);
    CHECK_EQ(purr_ct_find_addr(&b, "meshtastic", "!ffffffff"), (uint32_t)sam);
    CHECK_EQ(purr_ct_find_addr(&b, "meshtastic", "!a1b2c3d4"), 0u);                            /* the old one is gone */
    CHECK_EQ(purr_ct_set_fav(&b, (uint32_t)alex, 1), 0);

    uint32_t ids[8];
    CHECK_EQ(purr_ct_order(&b, ids, 8, NULL), 3);
    CHECK(ids[0] == (uint32_t)alex && ids[1] == (uint32_t)dana && ids[2] == (uint32_t)sam);    /* by name, any case */
    CHECK_EQ(purr_ct_order(&b, ids, 8, "RIV"), 1);
    CHECK_EQ(ids[0], (uint32_t)sam);
    CHECK_EQ(purr_ct_order(&b, ids, 8, "zzz"), 0);

    char text[1024];
    size_t n = purr_ct_format(&b, text, sizeof(text));
    CHECK(n > 40 && strstr(text, "C|1|0|Sam Rivera|meshtastic=!ffffffff;reticulum=7f3e9a21\n") != NULL);
    purr_contacts_t b2;
    purr_ct_parse(&b2, text);
    CHECK_EQ(b2.n, 3);
    CHECK_EQ(purr_ct_find_addr(&b2, "reticulum", "7f3e9a21"), (uint32_t)sam);
    CHECK_EQ(purr_ct_get(&b2, (uint32_t)alex)->fav, 1);
    CHECK_EQ(purr_ct_add(&b2, "New"), 4);                                                      /* ids carry on */

    CHECK_EQ(purr_ct_remove_addr(&b, (uint32_t)sam, "reticulum"), 0);
    CHECK_EQ(purr_ct_get(&b, (uint32_t)sam)->naddr, 1);
    CHECK_EQ(purr_ct_remove(&b, (uint32_t)alex), 0);
    CHECK(purr_ct_get(&b, (uint32_t)alex) == NULL && b.n == 2);
    CHECK_EQ(purr_ct_remove(&b, (uint32_t)alex), -1);

    purr_ct_parse(&b2, "garbage\nC|x\nC|7|1|Ok|svc=addr\nC|0|0|Zero|\n");                      /* hostile input is skipped */
    CHECK_EQ(b2.n, 1);
    CHECK_EQ(purr_ct_get(&b2, 7)->naddr, 1);
}

static void test_limits(void)
{
    purr_contacts_t b;
    purr_ct_init(&b);
    int32_t id = purr_ct_add(&b, "Many");
    char svc[8];
    int ok = 0;
    for (int i = 0; i < 8; i++) {
        snprintf(svc, sizeof(svc), "s%d", i);
        if (purr_ct_set_addr(&b, (uint32_t)id, svc, "a") == 0) {
            ok++;
        }
    }
    CHECK_EQ(ok, PURR_CT_ADDRS);
    for (int i = 0; i < 100; i++) {
        purr_ct_add(&b, "x");
    }
    CHECK_EQ(b.n, PURR_CT_MAX);
}

int main(void)
{
    test_book();
    test_limits();
    TK_DONE("test_contacts");
}
