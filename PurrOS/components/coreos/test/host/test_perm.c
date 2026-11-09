#include <string.h>

#include "purr_perm.h"
#include "testkit.h"

static void test_decide(void)
{
    purr_perm_t t;
    memset(&t, 0, sizeof(t));
    CHECK_EQ(purr_perm_decide(&t, "beacon", PURR_PERM_WAKE_ALARM, 0), PURR_PERM_PROMPT);
    CHECK_EQ(purr_perm_decide(&t, "beacon", PURR_PERM_WAKE_ALARM, 1), PURR_PERM_YES);     /* privileged: never asked */
    CHECK_EQ(purr_perm_decide(&t, "beacon", 99, 1), PURR_PERM_NO);                         /* unknown fails closed */
    CHECK_EQ(purr_perm_decide(&t, "beacon", -1, 0), PURR_PERM_NO);

    purr_perm_set(&t, "beacon", PURR_PERM_WAKE_ALARM, PURR_PERM_ALWAYS);
    CHECK_EQ(purr_perm_decide(&t, "beacon", PURR_PERM_WAKE_ALARM, 0), PURR_PERM_YES);
    CHECK_EQ(purr_perm_decide(&t, "beacon", PURR_PERM_KEEP_AWAKE, 0), PURR_PERM_PROMPT);  /* per permission */
    CHECK_EQ(purr_perm_decide(&t, "other", PURR_PERM_WAKE_ALARM, 0), PURR_PERM_PROMPT);   /* per app */

    purr_perm_set(&t, "beacon", PURR_PERM_WAKE_ALARM, PURR_PERM_NEVER);
    CHECK_EQ(purr_perm_decide(&t, "beacon", PURR_PERM_WAKE_ALARM, 0), PURR_PERM_NO);
    CHECK_EQ(purr_perm_decide(&t, "beacon", PURR_PERM_WAKE_ALARM, 1), PURR_PERM_YES);     /* privileged ignores it */
}

static void test_once_and_denials(void)
{
    purr_perm_t t;
    memset(&t, 0, sizeof(t));
    purr_perm_set(&t, "a", PURR_PERM_KEEP_AWAKE, PURR_PERM_ONCE);
    CHECK_EQ(purr_perm_decide(&t, "a", PURR_PERM_KEEP_AWAKE, 0), PURR_PERM_YES);
    purr_perm_app_stopped(&t, "a");
    CHECK_EQ(purr_perm_decide(&t, "a", PURR_PERM_KEEP_AWAKE, 0), PURR_PERM_PROMPT);      /* asks again next run */

    CHECK_EQ(purr_perm_deny(&t, "a", PURR_PERM_KEEP_AWAKE), 1);
    CHECK_EQ(purr_perm_deny(&t, "a", PURR_PERM_KEEP_AWAKE), 2);
    CHECK_EQ(purr_perm_denials(&t, "a", PURR_PERM_KEEP_AWAKE), PURR_PERM_DENIALS_BEFORE_NEVER);
    purr_perm_set(&t, "a", PURR_PERM_KEEP_AWAKE, PURR_PERM_ALWAYS);
    CHECK_EQ(purr_perm_denials(&t, "a", PURR_PERM_KEEP_AWAKE), 0);                       /* a yes clears the count */
    purr_perm_deny(&t, "a", PURR_PERM_KEEP_AWAKE);
    purr_perm_app_stopped(&t, "a");
    CHECK_EQ(purr_perm_denials(&t, "a", PURR_PERM_KEEP_AWAKE), 0);
    CHECK_EQ(purr_perm_state(&t, "a", PURR_PERM_KEEP_AWAKE), PURR_PERM_ALWAYS);          /* saved grants stay */
}

static void test_file_round_trip(void)
{
    purr_perm_t t, u;
    char buf[512];
    memset(&t, 0, sizeof(t));
    memset(&u, 0, sizeof(u));
    purr_perm_set(&t, "beacon", PURR_PERM_WAKE_ALARM, PURR_PERM_ALWAYS);
    purr_perm_set(&t, "beacon", PURR_PERM_KEEP_AWAKE, PURR_PERM_NEVER);
    purr_perm_set(&t, "chat", PURR_PERM_KEEP_AWAKE, PURR_PERM_ONCE);          /* not saved */
    purr_perm_set(&t, "mesh", PURR_PERM_WAKE_ALARM, PURR_PERM_ALWAYS);
    size_t n = purr_perm_format(&t, buf, sizeof(buf));
    CHECK(n > 0 && n == strlen(buf));
    CHECK(strstr(buf, "chat") == NULL);
    purr_perm_parse(&u, buf);
    CHECK_EQ(purr_perm_state(&u, "beacon", PURR_PERM_WAKE_ALARM), PURR_PERM_ALWAYS);
    CHECK_EQ(purr_perm_state(&u, "beacon", PURR_PERM_KEEP_AWAKE), PURR_PERM_NEVER);
    CHECK_EQ(purr_perm_state(&u, "mesh", PURR_PERM_WAKE_ALARM), PURR_PERM_ALWAYS);
    CHECK_EQ(purr_perm_state(&u, "chat", PURR_PERM_KEEP_AWAKE), PURR_PERM_ASK);

    /* A cramped buffer yields whole lines only. */
    char small[40];
    n = purr_perm_format(&t, small, sizeof(small));
    CHECK(n == strlen(small));
    CHECK(n == 0 || small[n - 1] == '\n');
}

static void test_parse_hostile(void)
{
    purr_perm_t t;
    memset(&t, 0, sizeof(t));
    purr_perm_parse(&t, "garbage\n:=\nx:y=z\nok:keep-awake=maybe\n# c:keep-awake=always\n"
                        "sp ace:keep-awake=always\n\nreal:wake-alarm=always\r\nno newline:keep-awake=never");
    CHECK_EQ(t.count, 1);                                                      /* only "real" parsed */
    CHECK_EQ(purr_perm_state(&t, "real", PURR_PERM_WAKE_ALARM), PURR_PERM_ALWAYS);
    purr_perm_parse(&t, NULL);

    char big[200];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    purr_perm_parse(&t, big);                                                  /* over-long line ignored */
    CHECK_EQ(t.count, 1);
    CHECK_EQ(purr_perm_set(&t, big, PURR_PERM_KEEP_AWAKE, PURR_PERM_ALWAYS), -1);
    CHECK_EQ(purr_perm_set(&t, "", PURR_PERM_KEEP_AWAKE, PURR_PERM_ALWAYS), -1);
    CHECK_EQ(purr_perm_set(&t, "x", PURR_PERM_COUNT, PURR_PERM_ALWAYS), -1);          /* one past the last is refused */
    CHECK_EQ(purr_perm_set(&t, "x", PURR_PERM_KEEP_AWAKE, 9), -1);
}

static void test_full_forget_reset(void)
{
    purr_perm_t t;
    char name[16];
    memset(&t, 0, sizeof(t));
    for (int i = 0; i < PURR_PERM_MAX_APPS; i++) {
        snprintf(name, sizeof(name), "app%d", i);
        CHECK_EQ(purr_perm_set(&t, name, PURR_PERM_KEEP_AWAKE, PURR_PERM_ALWAYS), 0);
    }
    CHECK_EQ(purr_perm_set(&t, "one-too-many", PURR_PERM_KEEP_AWAKE, PURR_PERM_ALWAYS), -1);
    CHECK_EQ(purr_perm_decide(&t, "one-too-many", PURR_PERM_KEEP_AWAKE, 0), PURR_PERM_PROMPT);   /* full: still asks */

    purr_perm_forget(&t, "app3");
    CHECK_EQ(t.count, PURR_PERM_MAX_APPS - 1);
    CHECK_EQ(purr_perm_state(&t, "app3", PURR_PERM_KEEP_AWAKE), PURR_PERM_ASK);
    CHECK_EQ(purr_perm_state(&t, "app15", PURR_PERM_KEEP_AWAKE), PURR_PERM_ALWAYS);              /* moved, not lost */
    CHECK_EQ(purr_perm_set(&t, "one-too-many", PURR_PERM_KEEP_AWAKE, PURR_PERM_ALWAYS), 0);

    purr_perm_reset(&t, "app15");
    CHECK_EQ(purr_perm_state(&t, "app15", PURR_PERM_KEEP_AWAKE), PURR_PERM_ASK);
    CHECK(purr_perm_by_name("keep-awake") == PURR_PERM_KEEP_AWAKE && purr_perm_by_name("wifi") == -1);
}

static void test_messaging_permissions(void)
{
    CHECK(purr_perm_by_name("messaging") == PURR_PERM_MESSAGING && purr_perm_by_name("contacts") == PURR_PERM_CONTACTS && purr_perm_by_name("radio") == PURR_PERM_RADIO);
    CHECK(purr_perm_describe(PURR_PERM_MESSAGING) != NULL && purr_perm_describe(PURR_PERM_RADIO) != NULL);
    purr_perm_t t;
    memset(&t, 0, sizeof(t));
    CHECK_EQ(purr_perm_decide(&t, "messages", PURR_PERM_MESSAGING, 0), PURR_PERM_PROMPT);       /* asked at first use */
    purr_perm_set(&t, "messages", PURR_PERM_MESSAGING, PURR_PERM_ALWAYS);
    purr_perm_set(&t, "messages", PURR_PERM_CONTACTS, PURR_PERM_NEVER);
    CHECK_EQ(purr_perm_decide(&t, "messages", PURR_PERM_MESSAGING, 0), PURR_PERM_YES);
    CHECK_EQ(purr_perm_decide(&t, "messages", PURR_PERM_CONTACTS, 0), PURR_PERM_NO);
    char text[256];
    purr_perm_format(&t, text, sizeof(text));
    purr_perm_t t2;
    memset(&t2, 0, sizeof(t2));
    purr_perm_parse(&t2, text);
    CHECK_EQ(purr_perm_state(&t2, "messages", PURR_PERM_MESSAGING), PURR_PERM_ALWAYS);        /* saved and read back by name */
    CHECK_EQ(purr_perm_state(&t2, "messages", PURR_PERM_CONTACTS), PURR_PERM_NEVER);
}

int main(void)
{
    test_decide();
    test_once_and_denials();
    test_file_round_trip();
    test_parse_hostile();
    test_full_forget_reset();
    test_messaging_permissions();
    TK_DONE("test_perm");
}
