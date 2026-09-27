#include <stdio.h>
#include <string.h>

#include "purr_users.h"
#include "testkit.h"

/* ---------------------------------------------------------------- accounts */

static void test_add_find_remove(void)
{
    purr_user_list_t l;
    purr_user_list_init(&l);
    CHECK_EQ(l.count, 0);
    CHECK_EQ(purr_user_next_uid(&l), 1);

    CHECK_EQ(purr_user_list_add(&l, "root_person", 1, PURR_ROLE_USER_ADMIN), PURR_USER_ADDED);
    CHECK_EQ(l.count, 1);
    CHECK_EQ(purr_user_next_uid(&l), 2);

    CHECK_EQ(purr_user_list_add(&l, "guest", 2, PURR_ROLE_USER_STANDARD), PURR_USER_ADDED);
    CHECK_EQ(purr_user_list_add(&l, "guest", 3, PURR_ROLE_USER_STANDARD), PURR_USER_EXISTS);
    CHECK_EQ(l.count, 2);

    purr_user_t *u = purr_user_list_find(&l, "guest");
    CHECK(u != NULL);
    CHECK_EQ(u->uid, 2);
    CHECK_EQ(u->role, PURR_ROLE_USER_STANDARD);
    CHECK(purr_user_list_find(&l, "nobody") == NULL);

    u = purr_user_list_find_uid(&l, 1);
    CHECK(u != NULL);
    CHECK(strcmp(u->name, "root_person") == 0);
    CHECK(purr_user_list_find_uid(&l, 99) == NULL);

    CHECK_EQ(purr_user_list_remove(&l, "guest"), 1);
    CHECK_EQ(l.count, 1);
    CHECK(purr_user_list_find(&l, "guest") == NULL);
    CHECK(purr_user_list_find(&l, "root_person") != NULL);   /* the other entry survives */
    CHECK_EQ(purr_user_list_remove(&l, "guest"), 0);
}

static void test_add_rejects(void)
{
    purr_user_list_t l;
    purr_user_list_init(&l);
    CHECK_EQ(purr_user_list_add(&l, "", 1, PURR_ROLE_USER_STANDARD), PURR_USER_INVALID);
    CHECK_EQ(purr_user_list_add(&l, "bad\tname", 1, PURR_ROLE_USER_STANDARD), PURR_USER_INVALID);
    CHECK_EQ(purr_user_list_add(&l, "root", 0, PURR_ROLE_USER_ADMIN), PURR_USER_INVALID);
    CHECK_EQ(l.count, 0);

    for (int i = 0; i < PURR_USER_MAX; i++) {
        char name[16];
        snprintf(name, sizeof(name), "u%d", i);
        CHECK_EQ(purr_user_list_add(&l, name, (uint8_t)(i + 1), PURR_ROLE_USER_STANDARD), PURR_USER_ADDED);
    }
    CHECK_EQ(purr_user_list_add(&l, "one_more", 200, PURR_ROLE_USER_STANDARD), PURR_USER_FULL);
}

static void test_next_uid_fills_gaps(void)
{
    purr_user_list_t l;
    purr_user_list_init(&l);
    purr_user_list_add(&l, "a", 1, PURR_ROLE_USER_ADMIN);
    purr_user_list_add(&l, "b", 3, PURR_ROLE_USER_STANDARD);
    CHECK_EQ(purr_user_next_uid(&l), 2);            /* the gap, not 4 */
    purr_user_list_remove(&l, "a");
    CHECK_EQ(purr_user_next_uid(&l), 1);
}

static void test_last_admin(void)
{
    purr_user_list_t l;
    purr_user_list_init(&l);
    purr_user_list_add(&l, "admin1", 1, PURR_ROLE_USER_ADMIN);
    purr_user_list_add(&l, "user1", 2, PURR_ROLE_USER_STANDARD);
    CHECK(purr_user_is_last_admin(&l, "admin1"));
    CHECK(!purr_user_is_last_admin(&l, "user1"));    /* not even an admin */

    purr_user_list_add(&l, "admin2", 3, PURR_ROLE_USER_ADMIN);
    CHECK(!purr_user_is_last_admin(&l, "admin1"));   /* two admins now */
    CHECK(!purr_user_is_last_admin(&l, "admin2"));
}

static void test_user_format_parse_round_trip(void)
{
    purr_user_list_t l, l2;
    purr_user_list_init(&l);
    purr_user_list_add(&l, "root_person", 1, PURR_ROLE_USER_ADMIN);
    purr_user_list_add(&l, "guest", 2, PURR_ROLE_USER_STANDARD);
    purr_user_list_find(&l, "guest")->fail_count = 3;
    purr_user_list_find(&l, "guest")->next_allowed_time = 1234567;

    char buf[512];
    int n = purr_user_list_format(&l, buf, sizeof(buf));
    CHECK(n > 0);

    purr_user_list_parse(&l2, buf, (size_t)n);
    CHECK_EQ(l2.count, 2);
    purr_user_t *u = purr_user_list_find(&l2, "guest");
    CHECK(u != NULL);
    CHECK_EQ(u->uid, 2);
    CHECK_EQ(u->role, PURR_ROLE_USER_STANDARD);
    CHECK_EQ(u->fail_count, 3u);
    CHECK_EQ(u->next_allowed_time, 1234567u);
    u = purr_user_list_find(&l2, "root_person");
    CHECK(u != NULL);
    CHECK_EQ(u->role, PURR_ROLE_USER_ADMIN);
}

static void test_user_parse_skips_malformed(void)
{
    purr_user_list_t l;
    const char *text =
        "good\t1\t0\t0\t0\n"
        "bad_role\t2\t9\t0\t0\n"
        "bad_uid\tnotanumber\t0\t0\t0\n"
        "too_few_fields\t3\t0\n"
        "\n"
        "another\t4\t1\t0\t0\n";
    purr_user_list_parse(&l, text, strlen(text));
    CHECK_EQ(l.count, 2);
    CHECK(purr_user_list_find(&l, "good") != NULL);
    CHECK(purr_user_list_find(&l, "another") != NULL);
    CHECK(purr_user_list_find(&l, "bad_role") == NULL);
    CHECK(purr_user_list_find(&l, "bad_uid") == NULL);
}

static void test_user_format_too_small(void)
{
    purr_user_list_t l;
    purr_user_list_init(&l);
    purr_user_list_add(&l, "someone", 1, PURR_ROLE_USER_ADMIN);
    char buf[4];
    CHECK_EQ(purr_user_list_format(&l, buf, sizeof(buf)), -1);
}

/* ---------------------------------------------------------------- passwords */

static const uint8_t k_salt_a[PURR_SALT_LEN] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const uint8_t k_salt_b[PURR_SALT_LEN] = {16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};

static void test_shadow_set_and_check(void)
{
    purr_shadow_list_t l;
    purr_shadow_list_init(&l);
    purr_shadow_set(&l, "alice", "hunter2", k_salt_a, 100);
    CHECK_EQ(l.count, 1);

    CHECK(purr_shadow_check(&l, "alice", "hunter2"));
    CHECK(!purr_shadow_check(&l, "alice", "wrong"));
    CHECK(!purr_shadow_check(&l, "bob", "hunter2"));      /* no such account */

    /* Setting again for the same name replaces, does not duplicate. */
    purr_shadow_set(&l, "alice", "newpass", k_salt_b, 200);
    CHECK_EQ(l.count, 1);
    CHECK(!purr_shadow_check(&l, "alice", "hunter2"));
    CHECK(purr_shadow_check(&l, "alice", "newpass"));
}

static void test_shadow_remove(void)
{
    purr_shadow_list_t l;
    purr_shadow_list_init(&l);
    purr_shadow_set(&l, "alice", "pw1", k_salt_a, 10);
    purr_shadow_set(&l, "bob", "pw2", k_salt_b, 10);
    CHECK_EQ(purr_shadow_list_remove(&l, "alice"), 1);
    CHECK_EQ(l.count, 1);
    CHECK(purr_shadow_find(&l, "alice") == NULL);
    CHECK(purr_shadow_check(&l, "bob", "pw2"));           /* the other entry survives */
    CHECK_EQ(purr_shadow_list_remove(&l, "alice"), 0);
}

static void test_shadow_format_parse_round_trip(void)
{
    purr_shadow_list_t l, l2;
    purr_shadow_list_init(&l);
    purr_shadow_set(&l, "alice", "hunter2", k_salt_a, 12345);
    purr_shadow_set(&l, "bob", "s3cret!", k_salt_b, 500);

    char buf[512];
    int n = purr_shadow_list_format(&l, buf, sizeof(buf));
    CHECK(n > 0);

    purr_shadow_list_parse(&l2, buf, (size_t)n);
    CHECK_EQ(l2.count, 2);
    CHECK(purr_shadow_check(&l2, "alice", "hunter2"));
    CHECK(!purr_shadow_check(&l2, "alice", "wrong"));
    CHECK(purr_shadow_check(&l2, "bob", "s3cret!"));
    const purr_shadow_entry_t *e = purr_shadow_find(&l2, "alice");
    CHECK(e != NULL);
    CHECK_EQ(e->iterations, 12345u);
    CHECK(memcmp(e->salt, k_salt_a, PURR_SALT_LEN) == 0);
}

static void test_shadow_parse_skips_malformed(void)
{
    purr_shadow_list_t l;
    /* A valid salt is 32 hex characters (16 bytes) and a valid hash is 64 (32 bytes). */
    const char *bad_len_salt = "0102030405060708090a0b0c0d0e0f1";     /* 31: one short */
    const char *non_hex_salt = "zz02030405060708090a0b0c0d0e0f10";   /* 32, but not hex */
    char text[768];
    snprintf(text, sizeof(text),
            "good\t0102030405060708090a0b0c0d0e0f10\t100\t"
            "0102030405060708090a0b0c0d0e0f100102030405060708090a0b0c0d0e0f10\n"
            "bad_salt_len\t%s\t100\t"
            "0102030405060708090a0b0c0d0e0f100102030405060708090a0b0c0d0e0f10\n"
            "bad_salt_hex\t%s\t100\t"
            "0102030405060708090a0b0c0d0e0f100102030405060708090a0b0c0d0e0f10\n"
            "bad_iterations\t0102030405060708090a0b0c0d0e0f10\tnotanumber\t"
            "0102030405060708090a0b0c0d0e0f100102030405060708090a0b0c0d0e0f10\n",
            bad_len_salt, non_hex_salt);
    purr_shadow_list_parse(&l, text, strlen(text));
    CHECK_EQ(l.count, 1);
    CHECK(purr_shadow_find(&l, "good") != NULL);
    CHECK(purr_shadow_find(&l, "bad_salt_len") == NULL);
    CHECK(purr_shadow_find(&l, "bad_salt_hex") == NULL);
    CHECK(purr_shadow_find(&l, "bad_iterations") == NULL);
}

/* ---------------------------------------------------------------- the growing delay */

static void test_login_delay(void)
{
    CHECK_EQ(purr_login_delay_seconds(0), 0u);
    CHECK_EQ(purr_login_delay_seconds(1), 1u);
    CHECK_EQ(purr_login_delay_seconds(2), 2u);
    CHECK_EQ(purr_login_delay_seconds(3), 4u);
    CHECK_EQ(purr_login_delay_seconds(4), 8u);
    CHECK_EQ(purr_login_delay_seconds(5), 16u);
    CHECK_EQ(purr_login_delay_seconds(6), 32u);
    CHECK_EQ(purr_login_delay_seconds(7), 60u);      /* 64 capped to 60 */
    CHECK_EQ(purr_login_delay_seconds(8), 60u);
    CHECK_EQ(purr_login_delay_seconds(1000), 60u);   /* stays capped, does not wrap */
}

int main(void)
{
    test_add_find_remove();
    test_add_rejects();
    test_next_uid_fills_gaps();
    test_last_admin();
    test_user_format_parse_round_trip();
    test_user_parse_skips_malformed();
    test_user_format_too_small();
    test_shadow_set_and_check();
    test_shadow_remove();
    test_shadow_format_parse_round_trip();
    test_shadow_parse_skips_malformed();
    test_login_delay();
    TK_DONE("test_users");
}
