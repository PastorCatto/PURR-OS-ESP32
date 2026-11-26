/* The now-playing session (purr_media.h): who owns it, the position of a playing track, the commands and what they need. */
#include <string.h>

#include "purr_media.h"
#include "testkit.h"

static purr_media_info_t info(const char *title, int state, uint32_t pos, uint32_t dur, uint32_t caps)
{
    purr_media_info_t i;
    memset(&i, 0, sizeof(i));
    strcpy(i.title, title);
    strcpy(i.artist, "Lowtide");
    i.state = (uint8_t)state;
    i.position_ms = pos;
    i.duration_ms = dur;
    i.caps = caps;
    return i;
}

static void test_report_get(void)
{
    purr_media_t m;
    purr_media_init(&m);
    char app[32];
    purr_media_info_t g;
    CHECK_EQ(purr_media_get(&m, 0, app, sizeof(app), &g), 0);           /* nothing playing */
    purr_media_info_t i = info("Paper Lanterns", PURR_MEDIA_PLAYING, 84000, 215000, 0);
    CHECK_EQ(purr_media_report(&m, "music", &i, 1000), 0);
    CHECK_EQ(purr_media_get(&m, 1000, app, sizeof(app), &g), 1);
    CHECK(strcmp(app, "music") == 0 && strcmp(g.title, "Paper Lanterns") == 0 && strcmp(g.artist, "Lowtide") == 0);
    CHECK_EQ(g.position_ms, 84000);
    CHECK_EQ(purr_media_get(&m, 11000, app, sizeof(app), &g), 1);       /* ten seconds on: a playing track moves */
    CHECK_EQ(g.position_ms, 94000);
    CHECK_EQ(purr_media_get(&m, 1000 + 500000, app, sizeof(app), &g), 1);
    CHECK_EQ(g.position_ms, 215000);                                    /* held at the end */
    CHECK_EQ(purr_media_get(&m, 0xFFFFFF00u, app, sizeof(app), &g), 1); /* a clock that wrapped since the report: no jump backwards past the end */
    CHECK(g.position_ms <= 215000);

    i = info("Paper Lanterns", PURR_MEDIA_PAUSED, 90000, 215000, 0);
    CHECK_EQ(purr_media_report(&m, "music", &i, 20000), 0);
    CHECK_EQ(purr_media_get(&m, 99999, app, sizeof(app), &g), 1);
    CHECK_EQ(g.position_ms, 90000);                                     /* paused: it stays */
}

static void test_bad_reports(void)
{
    purr_media_t m;
    purr_media_init(&m);
    purr_media_info_t i = info("x", PURR_MEDIA_PLAYING, 0, 0, 0);
    CHECK_EQ(purr_media_report(&m, NULL, &i, 0), -1);
    CHECK_EQ(purr_media_report(&m, "", &i, 0), -1);
    CHECK_EQ(purr_media_report(&m, "a", NULL, 0), -1);
    i.state = 3;
    CHECK_EQ(purr_media_report(&m, "a", &i, 0), -1);
    i = info("x", PURR_MEDIA_PLAYING, 5000, 4000, 0);
    CHECK_EQ(purr_media_report(&m, "a", &i, 0), -1);                    /* past the end */
    i = info("x", PURR_MEDIA_PLAYING, 5000, 0, 0);
    CHECK_EQ(purr_media_report(&m, "a", &i, 0), 0);                     /* unknown duration is fine */
    memset(i.title, 'T', sizeof(i.title));                              /* no terminator: it is cut, not read past */
    CHECK_EQ(purr_media_report(&m, "a", &i, 0), 0);
    purr_media_info_t g;
    purr_media_get(&m, 0, NULL, 0, &g);
    CHECK_EQ((int)strlen(g.title), PURR_MEDIA_TEXT - 1);
}

static void test_ownership(void)
{
    purr_media_t m;
    purr_media_init(&m);
    purr_media_info_t play = info("A", PURR_MEDIA_PLAYING, 0, 100000, 0), pause = info("B", PURR_MEDIA_PAUSED, 0, 100000, 0);
    CHECK_EQ(purr_media_report(&m, "music", &play, 0), 0);
    CHECK_EQ(purr_media_report(&m, "radio", &pause, 1), -2);            /* a paused app cannot take it from one that is playing */
    char app[32];
    purr_media_get(&m, 1, app, sizeof(app), NULL);
    CHECK(strcmp(app, "music") == 0);
    CHECK_EQ(purr_media_report(&m, "radio", &play, 2), 0);              /* one that plays does */
    purr_media_get(&m, 2, app, sizeof(app), NULL);
    CHECK(strcmp(app, "radio") == 0);
    CHECK_EQ(purr_media_clear(&m, "music"), -1);                        /* not the owner any more */
    CHECK_EQ(purr_media_clear(&m, "radio"), 0);
    CHECK_EQ(purr_media_get(&m, 3, app, sizeof(app), NULL), 0);
    CHECK_EQ(purr_media_report(&m, "music", &pause, 4), 0);             /* nobody owns it: a paused app may */
    pause = info("B", PURR_MEDIA_STOPPED, 0, 0, 0);
    CHECK_EQ(purr_media_report(&m, "radio", &pause, 5), 0);             /* the owner is not playing: anyone may take it */
}

static void test_commands(void)
{
    purr_media_t m;
    purr_media_init(&m);
    CHECK_EQ(purr_media_command(&m, PURR_MEDIA_CMD_PLAY, 0), -1);       /* no session */
    purr_media_info_t i = info("A", PURR_MEDIA_PLAYING, 0, 100000, PURR_MEDIA_CAP_PAUSE);
    purr_media_report(&m, "music", &i, 0);
    CHECK_EQ(purr_media_command(&m, 99, 0), -1);
    CHECK_EQ(purr_media_command(&m, 0, 0), -1);
    CHECK_EQ(purr_media_command(&m, PURR_MEDIA_CMD_NEXT, 0), -2);       /* it did not say it can skip */
    CHECK_EQ(purr_media_command(&m, PURR_MEDIA_CMD_SEEK, 5000), -2);
    CHECK_EQ(purr_media_command(&m, PURR_MEDIA_CMD_TOGGLE, 0), 0);
    CHECK_EQ(purr_media_command(&m, PURR_MEDIA_CMD_STOP, 0), 0);        /* stop needs nothing */
    CHECK_EQ(purr_media_command(&m, PURR_MEDIA_CMD_PAUSE, 0), 0);
    CHECK_EQ(purr_media_command(&m, PURR_MEDIA_CMD_PLAY, 0), 0);
    CHECK_EQ(purr_media_command(&m, PURR_MEDIA_CMD_PLAY, 0), -3);       /* the queue holds four */

    int cmd;
    uint32_t arg;
    CHECK_EQ(purr_media_next_command(&m, "radio", &cmd, &arg), 0);      /* not the owner */
    CHECK_EQ(purr_media_next_command(&m, "music", &cmd, &arg), 1);
    CHECK_EQ(cmd, PURR_MEDIA_CMD_TOGGLE);                               /* in order */
    CHECK_EQ(purr_media_next_command(&m, "music", &cmd, &arg), 1);
    CHECK_EQ(cmd, PURR_MEDIA_CMD_STOP);
    CHECK_EQ(purr_media_next_command(&m, "music", &cmd, &arg), 1);
    CHECK_EQ(purr_media_next_command(&m, "music", &cmd, &arg), 1);
    CHECK_EQ(cmd, PURR_MEDIA_CMD_PLAY);
    CHECK_EQ(purr_media_next_command(&m, "music", &cmd, &arg), 0);

    i = info("A", PURR_MEDIA_PLAYING, 0, 100000, PURR_MEDIA_CAP_PAUSE | PURR_MEDIA_CAP_SEEK | PURR_MEDIA_CAP_SKIP);
    purr_media_report(&m, "music", &i, 0);
    CHECK_EQ(purr_media_command(&m, PURR_MEDIA_CMD_SEEK, 42000), 0);
    CHECK_EQ(purr_media_command(&m, PURR_MEDIA_CMD_NEXT, 77), 0);
    CHECK_EQ(purr_media_next_command(&m, "music", &cmd, &arg), 1);
    CHECK(cmd == PURR_MEDIA_CMD_SEEK && arg == 42000);
    CHECK_EQ(purr_media_next_command(&m, "music", &cmd, &arg), 1);
    CHECK(cmd == PURR_MEDIA_CMD_NEXT && arg == 0);                      /* the argument only travels with SEEK */

    purr_media_command(&m, PURR_MEDIA_CMD_STOP, 0);
    purr_media_info_t other = info("B", PURR_MEDIA_PLAYING, 0, 0, 0);
    purr_media_report(&m, "radio", &other, 1);                          /* a new owner: the old owner's waiting commands are gone */
    CHECK_EQ(purr_media_next_command(&m, "radio", &cmd, &arg), 0);
    CHECK_EQ(purr_media_next_command(&m, "music", &cmd, &arg), 0);
}

static void test_changes(void)
{
    purr_media_t m;
    purr_media_init(&m);
    purr_media_info_t i = info("A", PURR_MEDIA_PLAYING, 0, 0, 0);
    uint32_t c0 = m.changes;
    purr_media_report(&m, "music", &i, 0);
    CHECK(m.changes != c0);
    uint32_t c1 = m.changes;
    purr_media_clear(&m, "music");
    CHECK(m.changes != c1);
}

int main(void)
{
    test_report_get();
    test_bad_reports();
    test_ownership();
    test_commands();
    test_changes();
    TK_DONE("test_media");
}
