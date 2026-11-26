/* The Click Wheel shell's adaptive layout (purr_wh_layout.h): the two profiles, the rules for any other size, and what a bad size gets. */
#include <stdio.h>
#include <string.h>

#include "purr_wh_layout.h"
#include "testkit.h"

static void test_profiles(void)
{
    purr_wh_metrics_t m;
    CHECK(purr_wh_has_profile(320, 240, 0));
    CHECK(purr_wh_has_profile(200, 200, 1));
    CHECK(!purr_wh_has_profile(320, 240, 1));                    /* a profile is for a kind of panel as well as a size */
    CHECK(!purr_wh_has_profile(200, 200, 0));
    CHECK(!purr_wh_has_profile(240, 320, 0));                    /* turned: no profile, the rules answer */

    CHECK_EQ(purr_wh_layout(320, 240, 0, &m), 0);
    CHECK_EQ(m.source, PURR_WH_FROM_PROFILE);
    CHECK_EQ(m.shape, PURR_WH_WIDE);
    CHECK_EQ(m.bar_h, 18);
    CHECK_EQ(m.row_h, 26);
    CHECK_EQ(m.rows, 8);
    CHECK_EQ(m.split, 1);
    CHECK_EQ(m.reflection, 1);
    CHECK_EQ(m.mono, 0);
    CHECK(m.panel_w > 0 && m.panel_w < 320 / 2);
    CHECK(m.bar_h + m.rows * m.row_h <= m.h);

    CHECK_EQ(purr_wh_layout(200, 200, 1, &m), 0);
    CHECK_EQ(m.source, PURR_WH_FROM_PROFILE);
    CHECK_EQ(m.shape, PURR_WH_SQUARE);
    CHECK_EQ(m.mono, 1);
    CHECK_EQ(m.split, 0);
    CHECK_EQ(m.panel_w, 0);
    CHECK_EQ(m.reflection, 0);                                   /* e-ink: no reflection */
    CHECK_EQ(m.rows, 8);
    CHECK(m.bar_h + m.rows * m.row_h <= m.h);
}

/* The rules, worked on the two known sizes, land near the hand-made profiles: a size nobody measured looks like its neighbours. */
static void test_rules_match_profiles(void)
{
    purr_wh_metrics_t m;
    CHECK_EQ(purr_wh_layout(321, 240, 0, &m), 0);                /* one pixel off: no profile, the rules */
    CHECK_EQ(m.source, PURR_WH_FROM_RULES);
    CHECK_EQ(m.shape, PURR_WH_WIDE);
    CHECK_EQ(m.bar_h, 18);                                       /* 240 / 13 */
    CHECK_EQ(m.row_h, 26);
    CHECK_EQ(m.rows, 8);
    CHECK_EQ(m.split, 1);
    CHECK_EQ(m.font_row, PURR_WH_FONT_MEDIUM);
    CHECK_EQ(m.clock_tier, 2);

    CHECK_EQ(purr_wh_layout(200, 201, 1, &m), 0);
    CHECK_EQ(m.source, PURR_WH_FROM_RULES);
    CHECK_EQ(m.shape, PURR_WH_SQUARE);
    CHECK_EQ(m.bar_h, 16);                                       /* 200 / 13 = 15, held up to the smallest bar (the profile says 18) */
    CHECK_EQ(m.row_h, 22);
    CHECK_EQ(m.split, 0);
    CHECK_EQ(m.reflection, 0);
    CHECK_EQ(m.font_row, PURR_WH_FONT_SMALL);
    CHECK_EQ(m.clock_tier, 1);
}

static void test_other_sizes(void)
{
    purr_wh_metrics_t m;
    CHECK_EQ(purr_wh_layout(240, 320, 0, &m), 0);                /* a rotated T-Deck */
    CHECK_EQ(m.shape, PURR_WH_TALL);
    CHECK_EQ(m.split, 0);
    CHECK(m.rows >= 9);
    CHECK_EQ(m.reflection, 0);                                   /* only wide screens have the reflection */

    CHECK_EQ(purr_wh_layout(480, 320, 0, &m), 0);                /* a bigger wide colour panel */
    CHECK_EQ(m.shape, PURR_WH_WIDE);
    CHECK_EQ(m.split, 1);
    CHECK(m.row_h >= 30);
    CHECK_EQ(m.font_row, PURR_WH_FONT_LARGE);
    CHECK_EQ(m.reflection, 1);

    CHECK_EQ(purr_wh_layout(296, 128, 1, &m), 0);                /* a flat e-ink strip (2.9 inch): very wide, very short */
    CHECK_EQ(m.shape, PURR_WH_WIDE);
    CHECK(m.rows >= 3);
    CHECK_EQ(m.split, 1);
    CHECK_EQ(m.reflection, 0);                                   /* mono never has it */

    CHECK_EQ(purr_wh_layout(100, 100, 1, &m), 0);                /* the smallest it accepts */
    CHECK(m.rows >= 3);
    CHECK(m.bar_h + m.rows * m.row_h <= 100);
}

/* Whatever the size, the numbers are usable: the rows fit under the bar, nothing is zero or negative. */
static void test_always_usable(void)
{
    purr_wh_metrics_t m;
    for (int w = 80; w <= 800; w += 37) {
        for (int h = 80; h <= 800; h += 41) {
            for (int mono = 0; mono < 2; mono++) {
                CHECK_EQ(purr_wh_layout(w, h, mono, &m), 0);
                CHECK(m.bar_h > 0 && m.row_h > 0 && m.pad > 0 && m.icon > 0 && m.cover > 0);
                CHECK(m.rows >= 1);
                CHECK(m.bar_h + m.rows * m.row_h <= h);
                CHECK(m.cover <= (w < h ? w : h));
                CHECK(m.split ? (m.panel_w > 0 && m.panel_w < w) : (m.panel_w == 0));
                CHECK(!(m.mono && m.reflection));
                CHECK(m.font_row <= PURR_WH_FONT_LARGE && m.font_title <= PURR_WH_FONT_LARGE && m.font_small <= PURR_WH_FONT_LARGE);
            }
        }
    }
}

static void test_bad_sizes(void)
{
    purr_wh_metrics_t m;
    CHECK_EQ(purr_wh_layout(0, 0, 0, &m), -1);
    CHECK_EQ(purr_wh_layout(79, 240, 0, &m), -1);
    CHECK_EQ(purr_wh_layout(240, 79, 0, &m), -1);
    CHECK_EQ(purr_wh_layout(5000, 240, 0, &m), -1);
    CHECK_EQ(purr_wh_layout(-1, 240, 0, &m), -1);
    CHECK_EQ(purr_wh_layout(320, 240, 0, NULL), -1);
}

static void test_describe(void)
{
    purr_wh_metrics_t m;
    char buf[96];
    purr_wh_layout(320, 240, 0, &m);
    purr_wh_describe(&m, buf, sizeof(buf));
    CHECK(strstr(buf, "320x240") != NULL && strstr(buf, "profile") != NULL && strstr(buf, "side panel") != NULL);
    purr_wh_layout(200, 200, 1, &m);
    purr_wh_describe(&m, buf, sizeof(buf));
    CHECK(strstr(buf, "mono") != NULL && strstr(buf, "square") != NULL && strstr(buf, "side panel") == NULL);
    char tiny[8];
    purr_wh_describe(&m, tiny, sizeof(tiny));                    /* truncated, never overrun */
    CHECK(strlen(tiny) < sizeof(tiny));
    purr_wh_describe(NULL, buf, sizeof(buf));
    purr_wh_describe(&m, NULL, 0);
}

int main(void)
{
    test_profiles();
    test_rules_match_profiles();
    test_other_sizes();
    test_always_usable();
    test_bad_sizes();
    test_describe();
    TK_DONE("test_wh_layout");
}
