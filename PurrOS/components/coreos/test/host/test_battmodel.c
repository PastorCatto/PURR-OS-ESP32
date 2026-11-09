#include <stdio.h>

#include "purr_battmodel.h"
#include "testkit.h"

static void test_first_reading_and_smoothing(void)
{
    purr_battmodel_t m;
    purr_battmodel_init(&m);
    int pct = -1, mv = -1;
    CHECK_EQ(purr_battmodel_get(&m, &pct, &mv), 0);                   /* nothing yet */
    purr_battmodel_feed(&m, 1, 0, 80, 4000);
    CHECK_EQ(purr_battmodel_get(&m, &pct, &mv), 1);
    CHECK_EQ(pct, 80);                                                /* the first reading is taken as it is */
    CHECK_EQ(mv, 4000);
    purr_battmodel_feed(&m, 1, 0, 40, 3750);                          /* then it follows at a quarter of the step: (80*3+40+2)/4 */
    purr_battmodel_get(&m, &pct, &mv);
    CHECK_EQ(pct, 70);
    CHECK_EQ(mv, 3938);                                               /* (4000*3+3750+2)/4 */
    CHECK_EQ(purr_battmodel_charging(&m), 0);
}

static void test_no_battery_clears_everything(void)
{
    purr_battmodel_t m;
    purr_battmodel_init(&m);
    purr_battmodel_feed(&m, 1, 1, 100, 4800);
    CHECK_EQ(purr_battmodel_charging(&m), 1);
    purr_battmodel_feed(&m, 0, 0, 0, 0);
    CHECK_EQ(purr_battmodel_get(&m, NULL, NULL), 0);
    CHECK_EQ(purr_battmodel_charging(&m), 0);
}

static void test_plugged_in_holds_full_and_charging(void)
{
    purr_battmodel_t m;
    purr_battmodel_init(&m);
    int pct;
    purr_battmodel_feed(&m, 1, 1, 100, 4800);                         /* booted on USB: the line is at 4.8 V */
    CHECK_EQ(purr_battmodel_get(&m, &pct, NULL), 1);
    CHECK_EQ(pct, 100);
    CHECK_EQ(purr_battmodel_charging(&m), 1);
    purr_battmodel_feed(&m, 1, 1, 100, 5000);                         /* it bounces up and down */
    purr_battmodel_feed(&m, 1, 1, 100, 4500);
    CHECK_EQ(purr_battmodel_charging(&m), 1);
}

static void test_a_dip_while_bouncing_does_not_flicker(void)
{
    purr_battmodel_t m;
    purr_battmodel_init(&m);
    int pct;
    purr_battmodel_feed(&m, 1, 1, 100, 4900);
    purr_battmodel_feed(&m, 1, 0, 90, 4150);                          /* a reading back at cell level: still plugged in, level held */
    purr_battmodel_get(&m, &pct, NULL);
    CHECK_EQ(pct, 100);
    CHECK_EQ(purr_battmodel_charging(&m), 1);
    purr_battmodel_feed(&m, 1, 1, 100, 4800);                         /* and it is back up: the hold starts over */
    purr_battmodel_feed(&m, 1, 0, 90, 4150);
    purr_battmodel_feed(&m, 1, 0, 90, 4150);
    CHECK_EQ(purr_battmodel_charging(&m), 0);                         /* two dips used up the hold */
}

static void test_unplugging_settles_from_full(void)
{
    purr_battmodel_t m;
    purr_battmodel_init(&m);
    int pct;
    purr_battmodel_feed(&m, 1, 1, 100, 4900);
    for (int i = 0; i < PURR_BATT_EXT_HOLD; i++) {
        purr_battmodel_feed(&m, 1, 0, 80, 4000);                      /* the hold runs out; the level is still 100 */
    }
    purr_battmodel_get(&m, &pct, NULL);
    CHECK_EQ(pct, 100);
    CHECK_EQ(purr_battmodel_charging(&m), 0);
    purr_battmodel_feed(&m, 1, 0, 80, 4000);                          /* then it follows the cell down, not in one jump */
    purr_battmodel_get(&m, &pct, NULL);
    CHECK_EQ(pct, 95);                                                /* (100*3+80+2)/4 */
}

static void test_early_charge_reads_like_a_cell(void)
{
    /* The owner's unit reads about 40 percent for the first stretch of a charge, with the line still at cell level: nothing says charging there, and the
     * model reports what the gauge says. */
    purr_battmodel_t m;
    purr_battmodel_init(&m);
    purr_battmodel_feed(&m, 1, 0, 40, 3750);
    int pct;
    purr_battmodel_get(&m, &pct, NULL);
    CHECK_EQ(pct, 40);
    CHECK_EQ(purr_battmodel_charging(&m), 0);
}

int main(void)
{
    test_first_reading_and_smoothing();
    test_no_battery_clears_everything();
    test_plugged_in_holds_full_and_charging();
    test_a_dip_while_bouncing_does_not_flicker();
    test_unplugging_settles_from_full();
    test_early_charge_reads_like_a_cell();
    TK_DONE("test_battmodel");
}
