#include <stdio.h>
#include <string.h>

#include "purr_wifi.h"
#include "testkit.h"

static void test_add_update_remove(void)
{
    purr_wifi_list_t l;
    purr_wifi_list_init(&l);
    CHECK_EQ(l.count, 0);

    CHECK_EQ(purr_wifi_list_add(&l, "home", "hunter2"), PURR_WIFI_ADDED);
    CHECK_EQ(l.count, 1);
    CHECK(purr_wifi_list_find(&l, "home") != NULL);
    CHECK(strcmp(purr_wifi_list_find(&l, "home")->pass, "hunter2") == 0);

    CHECK_EQ(purr_wifi_list_add(&l, "home", "newpass"), PURR_WIFI_UPDATED);
    CHECK_EQ(l.count, 1);                         /* replaced, not duplicated */
    CHECK(strcmp(purr_wifi_list_find(&l, "home")->pass, "newpass") == 0);

    CHECK_EQ(purr_wifi_list_add(&l, "office", ""), PURR_WIFI_ADDED);   /* open network: blank pass */
    CHECK_EQ(l.count, 2);

    CHECK_EQ(purr_wifi_list_remove(&l, "home"), 1);
    CHECK_EQ(l.count, 1);
    CHECK(purr_wifi_list_find(&l, "home") == NULL);
    CHECK(purr_wifi_list_find(&l, "office") != NULL);   /* the other entry survives */
    CHECK_EQ(purr_wifi_list_remove(&l, "home"), 0);      /* already gone */
}

static void test_invalid_and_full(void)
{
    purr_wifi_list_t l;
    purr_wifi_list_init(&l);

    CHECK_EQ(purr_wifi_list_add(&l, "", "x"), PURR_WIFI_INVALID);
    CHECK_EQ(purr_wifi_list_add(&l, "bad\tname", "x"), PURR_WIFI_INVALID);
    CHECK_EQ(purr_wifi_list_add(&l, "name", "bad\npass"), PURR_WIFI_INVALID);
    CHECK_EQ(l.count, 0);

    for (int i = 0; i < PURR_WIFI_MAX_SAVED; i++) {
        char ssid[16];
        snprintf(ssid, sizeof(ssid), "net%d", i);
        CHECK_EQ(purr_wifi_list_add(&l, ssid, "p"), PURR_WIFI_ADDED);
    }
    CHECK_EQ(l.count, PURR_WIFI_MAX_SAVED);
    CHECK_EQ(purr_wifi_list_add(&l, "one_more", "p"), PURR_WIFI_FULL);
    CHECK_EQ(l.count, PURR_WIFI_MAX_SAVED);
    /* Updating one already saved still works when the list is full. */
    CHECK_EQ(purr_wifi_list_add(&l, "net0", "newpass"), PURR_WIFI_UPDATED);
}

static void test_format_and_parse_round_trip(void)
{
    purr_wifi_list_t l, l2;
    purr_wifi_list_init(&l);
    purr_wifi_list_add(&l, "home", "hunter2");
    purr_wifi_list_add(&l, "cafe", "");
    purr_wifi_list_add(&l, "office wifi", "s3cret!");

    char buf[512];
    int n = purr_wifi_list_format(&l, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(strstr(buf, "home\thunter2\n") != NULL);
    CHECK(strstr(buf, "cafe\t\n") != NULL);

    purr_wifi_list_parse(&l2, buf, (size_t)n);
    CHECK_EQ(l2.count, 3);
    CHECK(strcmp(purr_wifi_list_find(&l2, "home")->pass, "hunter2") == 0);
    CHECK(strcmp(purr_wifi_list_find(&l2, "cafe")->pass, "") == 0);
    CHECK(strcmp(purr_wifi_list_find(&l2, "office wifi")->pass, "s3cret!") == 0);
}

static void test_format_too_small(void)
{
    purr_wifi_list_t l;
    purr_wifi_list_init(&l);
    purr_wifi_list_add(&l, "home", "hunter2");
    char buf[4];
    CHECK_EQ(purr_wifi_list_format(&l, buf, sizeof(buf)), -1);
}

static void test_parse_skips_malformed(void)
{
    purr_wifi_list_t l;
    const char *text = "good\tpass\n"
                        "no_tab_here\n"
                        "\n"
                        "another\tok\n"
                        "trailing_no_newline\tp";       /* dropped: no closing newline is fine too */
    purr_wifi_list_parse(&l, text, strlen(text));
    CHECK_EQ(l.count, 3);
    CHECK(purr_wifi_list_find(&l, "good") != NULL);
    CHECK(purr_wifi_list_find(&l, "another") != NULL);
    CHECK(purr_wifi_list_find(&l, "trailing_no_newline") != NULL);
    CHECK(purr_wifi_list_find(&l, "no_tab_here") == NULL);
}

static void test_parse_caps_at_max(void)
{
    char text[PURR_WIFI_MAX_SAVED * 20 + 200];
    int pos = 0;
    for (int i = 0; i < PURR_WIFI_MAX_SAVED + 5; i++) {
        pos += snprintf(text + pos, sizeof(text) - pos, "net%d\tp\n", i);
    }
    purr_wifi_list_t l;
    purr_wifi_list_parse(&l, text, (size_t)pos);
    CHECK_EQ(l.count, PURR_WIFI_MAX_SAVED);
}

static void test_pick(void)
{
    purr_wifi_list_t saved;
    purr_wifi_list_init(&saved);
    purr_wifi_list_add(&saved, "home", "a");
    purr_wifi_list_add(&saved, "office", "b");

    purr_wifi_seen_t seen[] = {
        {"stranger", -40},
        {"office", -70},
        {"home", -55},
    };
    CHECK_EQ(purr_wifi_pick(&saved, seen, 3), 2);      /* home is saved and stronger than office */

    purr_wifi_seen_t none_saved[] = {{"stranger", -40}, {"another", -30}};
    CHECK_EQ(purr_wifi_pick(&saved, none_saved, 2), -1);

    CHECK_EQ(purr_wifi_pick(&saved, seen, 0), -1);

    purr_wifi_seen_t only_office[] = {{"office", -80}};
    CHECK_EQ(purr_wifi_pick(&saved, only_office, 1), 0);
}

int main(void)
{
    test_add_update_remove();
    test_invalid_and_full();
    test_format_and_parse_round_trip();
    test_format_too_small();
    test_parse_skips_malformed();
    test_parse_caps_at_max();
    test_pick();
    TK_DONE("test_wifi");
}
