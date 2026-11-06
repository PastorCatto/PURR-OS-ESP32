/* test_resetnote.c - the wording of the data-reset notice. */
#include <string.h>

#include "purr_resetnote.h"
#include "testkit.h"

static void test_every_reason_has_words(void)
{
    char b[96], f[400];
    for (int why = PURR_RESET_AUTO_FORMAT; why <= PURR_RESET_DEFAULT_OVER_HOMES; why++) {
        CHECK_EQ(purr_resetnote_text(why, -84, 1, b, sizeof(b), f, sizeof(f)), 1);
        CHECK(strlen(b) > 10 && strlen(b) < 80);                 /* one line that fits the sign-in screen */
        CHECK(strlen(f) > strlen(b));
    }
    purr_resetnote_text(PURR_RESET_AUTO_FORMAT, -84, 1, b, sizeof(b), f, sizeof(f));
    CHECK(strstr(f, "-84") != NULL);                              /* the error code is there for the person who reports it */
    CHECK(strstr(f, "held data") != NULL);
    purr_resetnote_text(PURR_RESET_AUTO_FORMAT, -84, 0, b, sizeof(b), f, sizeof(f));
    CHECK(strstr(f, "held data") == NULL);
    purr_resetnote_text(PURR_RESET_DEFAULT_OVER_HOMES, 0, 1, b, sizeof(b), f, sizeof(f));
    CHECK(strstr(f, "MEOW") != NULL && strstr(f, "capitals") != NULL && strstr(f, "/home") != NULL);
}

static void test_unknown_and_small_buffers(void)
{
    char b[96], f[400];
    CHECK_EQ(purr_resetnote_text(99, 0, 0, b, sizeof(b), f, sizeof(f)), 0);
    CHECK(strlen(b) > 0 && strlen(f) > 0);
    char tiny[8];
    CHECK_EQ(purr_resetnote_text(PURR_RESET_USER_ERASE, 0, 0, tiny, sizeof(tiny), tiny, sizeof(tiny)), 1);
    CHECK_EQ(strlen(tiny), 7);                                    /* cut cleanly and terminated */
    CHECK_EQ(purr_resetnote_text(PURR_RESET_USER_ERASE, 0, 0, NULL, 0, NULL, 0), 1);
}

int main(void)
{
    test_every_reason_has_words();
    test_unknown_and_small_buffers();
    TK_DONE("test_resetnote");
}
