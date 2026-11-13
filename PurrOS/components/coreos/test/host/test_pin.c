/* The lock screen's optional PIN (purr_pin.h): the record, the check, and the pause after wrong tries. */
#include <stdio.h>
#include <string.h>

#include "purr_pin.h"
#include "testkit.h"

static const uint8_t SALT_A[PURR_PIN_SALT_LEN] = {1, 2, 3, 4, 5, 6, 7, 8};
static const uint8_t SALT_B[PURR_PIN_SALT_LEN] = {8, 7, 6, 5, 4, 3, 2, 1};

static void test_valid(void)
{
    CHECK(purr_pin_valid("1234"));
    CHECK(purr_pin_valid("0000"));
    CHECK(!purr_pin_valid("123"));
    CHECK(!purr_pin_valid("12345"));
    CHECK(!purr_pin_valid("12a4"));
    CHECK(!purr_pin_valid(""));
    CHECK(!purr_pin_valid(" 123"));
    CHECK(!purr_pin_valid(NULL));
}

static void test_record(void)
{
    char rec[PURR_PIN_RECORD_MAX], rec2[PURR_PIN_RECORD_MAX], rec3[PURR_PIN_RECORD_MAX];
    CHECK_EQ(purr_pin_make("4821", SALT_A, rec, sizeof(rec)), 0);
    CHECK(strncmp(rec, "v1:0102030405060708:", 20) == 0);
    CHECK_EQ((int)strlen(rec), 3 + 16 + 1 + 64);
    CHECK(strstr(rec, "4821") == NULL);                          /* the PIN is not in the record */
    CHECK(purr_pin_check(rec, "4821"));
    CHECK(!purr_pin_check(rec, "4822"));
    CHECK(!purr_pin_check(rec, "0000"));
    CHECK(!purr_pin_check(rec, "482"));                          /* not a PIN at all */
    CHECK(!purr_pin_check(rec, NULL));

    CHECK_EQ(purr_pin_make("4821", SALT_A, rec2, sizeof(rec2)), 0);
    CHECK(strcmp(rec, rec2) == 0);                               /* the same PIN and salt give the same record */
    CHECK_EQ(purr_pin_make("4821", SALT_B, rec3, sizeof(rec3)), 0);
    CHECK(strcmp(rec, rec3) != 0);                               /* another salt, another record */
    CHECK(purr_pin_check(rec3, "4821"));

    char line[PURR_PIN_RECORD_MAX + 2];                          /* as it comes back from a file: with a newline */
    snprintf(line, sizeof(line), "%s\n", rec);
    CHECK(purr_pin_check(line, "4821"));
    snprintf(line, sizeof(line), "%s\r\n", rec);
    CHECK(purr_pin_check(line, "4821"));

    CHECK_EQ(purr_pin_make("48x1", SALT_A, rec2, sizeof(rec2)), -1);
    char small[40];
    CHECK_EQ(purr_pin_make("4821", SALT_A, small, sizeof(small)), -1);
    CHECK_EQ(purr_pin_make("4821", NULL, rec2, sizeof(rec2)), -1);
}

static void test_damaged_records(void)
{
    char rec[PURR_PIN_RECORD_MAX];
    purr_pin_make("1111", SALT_A, rec, sizeof(rec));
    CHECK(!purr_pin_check("", "1111"));
    CHECK(!purr_pin_check("garbage", "1111"));
    CHECK(!purr_pin_check(NULL, "1111"));
    char bad[PURR_PIN_RECORD_MAX];
    memcpy(bad, rec, sizeof(bad));
    bad[0] = 'x';
    CHECK(!purr_pin_check(bad, "1111"));                         /* wrong version */
    memcpy(bad, rec, sizeof(bad));
    bad[10] = 'g';
    CHECK(!purr_pin_check(bad, "1111"));                         /* not hex */
    memcpy(bad, rec, sizeof(bad));
    bad[40] = (bad[40] == 'a') ? 'b' : 'a';
    CHECK(!purr_pin_check(bad, "1111"));                         /* a changed hash */
    memcpy(bad, rec, sizeof(bad));
    bad[strlen(bad) - 1] = '\0';
    CHECK(!purr_pin_check(bad, "1111"));                         /* cut short */
    memcpy(bad, rec, sizeof(bad));
    bad[19] = '/';
    CHECK(!purr_pin_check(bad, "1111"));                         /* the colon is missing */
}

static void test_guard(void)
{
    purr_pin_guard_t g = {0, 0};
    uint32_t now = 1000;
    CHECK_EQ(purr_pin_guard_wait(&g, now), 0u);
    purr_pin_guard_fail(&g, now);
    purr_pin_guard_fail(&g, now);
    CHECK_EQ(purr_pin_guard_wait(&g, now), 0u);                  /* two wrong tries: no pause */
    purr_pin_guard_fail(&g, now);
    CHECK_EQ(purr_pin_guard_wait(&g, now), 30u);                 /* the third: 30 s */
    CHECK_EQ(purr_pin_guard_wait(&g, now + 10000), 20u);
    CHECK_EQ(purr_pin_guard_wait(&g, now + 29001), 1u);          /* rounded up, so a try is never early */
    CHECK_EQ(purr_pin_guard_wait(&g, now + 30000), 0u);
    now += 31000;
    purr_pin_guard_fail(&g, now);
    purr_pin_guard_fail(&g, now);
    CHECK_EQ(purr_pin_guard_wait(&g, now), 0u);
    purr_pin_guard_fail(&g, now);                                /* the sixth: 60 s */
    CHECK_EQ(purr_pin_guard_wait(&g, now), 60u);
    CHECK(!purr_pin_guard_locked_out(&g));
    now += 61000;
    for (int i = 0; i < 3; i++) {
        purr_pin_guard_fail(&g, now);
    }
    CHECK_EQ(purr_pin_guard_wait(&g, now), 120u);                /* the ninth: 120 s, and only the password from here */
    CHECK(purr_pin_guard_locked_out(&g));
    purr_pin_guard_ok(&g);                                       /* the password was used */
    CHECK(!purr_pin_guard_locked_out(&g));
    CHECK_EQ(purr_pin_guard_wait(&g, now), 0u);

    /* the pause is capped at five minutes */
    memset(&g, 0, sizeof(g));
    for (int i = 0; i < 60; i++) {
        purr_pin_guard_fail(&g, 5000);
    }
    CHECK(purr_pin_guard_wait(&g, 5000) <= 300u);
    CHECK(g.fails <= 255);

    /* a clock that wraps at 2^32 does not make the pause vanish or last for ever */
    memset(&g, 0, sizeof(g));
    uint32_t near_end = 0xFFFFFFFFu - 5000u;
    for (int i = 0; i < 3; i++) {
        purr_pin_guard_fail(&g, near_end);
    }
    CHECK_EQ(purr_pin_guard_wait(&g, near_end), 30u);
    CHECK_EQ(purr_pin_guard_wait(&g, near_end + 20000u), 10u);   /* now is past the wrap */
    CHECK_EQ(purr_pin_guard_wait(&g, near_end + 31000u), 0u);
}

int main(void)
{
    test_valid();
    test_record();
    test_damaged_records();
    test_guard();
    TK_DONE("test_pin");
}
