#include "purr_menu.h"
#include "testkit.h"

static const purr_menu_part_t two[] = {{"ota_0", 1}, {"ota_1", 1}};
static const purr_menu_part_t mixed[] = {{"nvs", 0}, {"ota_0", 1}, {"junk", 0}, {"ota_1", 1}};
static const purr_menu_part_t none[] = {{"ota_0", 0}, {"ota_1", 0}};

static void test_entries(void)
{
    purr_menu_t m;
    purr_menu_init(&m, mixed, 4, -1);
    CHECK_EQ(m.count, 2);                 /* only the bootable ones, in order */
    CHECK_EQ(m.entries[0].part, 1);
    CHECK_EQ(m.entries[1].part, 3);
    CHECK_EQ(m.selected, 0);
    CHECK_EQ(m.no_boot_options, 0);

    purr_menu_init(&m, mixed, 4, 3);      /* preferred slot is selected first */
    CHECK_EQ(m.selected, 1);
    purr_menu_init(&m, mixed, 4, 2);      /* preferred but not bootable: ignored */
    CHECK_EQ(m.selected, 0);

    purr_menu_part_t many[12];
    for (int i = 0; i < 12; i++) {
        many[i] = (purr_menu_part_t){"x", 1};
    }
    purr_menu_init(&m, many, 12, -1);
    CHECK_EQ(m.count, PURR_MENU_MAX_ENTRIES);
}

static void test_countdown(void)
{
    purr_menu_t m;
    purr_menu_init(&m, two, 2, 1);
    CHECK_EQ(purr_menu_seconds_left(&m), 3);
    CHECK_EQ(purr_menu_step(&m, 1000, PURR_KEY_NONE).action, PURR_MENU_ACT_NONE);
    CHECK_EQ(purr_menu_seconds_left(&m), 2);
    CHECK_EQ(purr_menu_step(&m, 1999, PURR_KEY_NONE).action, PURR_MENU_ACT_NONE);
    purr_menu_result_t r = purr_menu_step(&m, 1, PURR_KEY_NONE);
    CHECK_EQ(r.action, PURR_MENU_ACT_BOOT);
    CHECK_EQ(r.index, 1);                 /* the preferred one */
}

static void test_keys(void)
{
    purr_menu_t m;
    purr_menu_init(&m, two, 2, 0);
    purr_menu_step(&m, 100, PURR_KEY_DOWN);
    CHECK_EQ(m.selected, 1);
    CHECK_EQ(purr_menu_seconds_left(&m), -1);   /* a key stops the countdown */
    CHECK_EQ(purr_menu_step(&m, 60000, PURR_KEY_NONE).action, PURR_MENU_ACT_NONE);
    purr_menu_step(&m, 10, PURR_KEY_DOWN);      /* clamps at the end */
    CHECK_EQ(m.selected, 1);
    purr_menu_step(&m, 10, PURR_KEY_UP);
    purr_menu_step(&m, 10, PURR_KEY_UP);        /* and at the start */
    CHECK_EQ(m.selected, 0);
    purr_menu_result_t r = purr_menu_step(&m, 10, PURR_KEY_ENTER);
    CHECK_EQ(r.action, PURR_MENU_ACT_BOOT);
    CHECK_EQ(r.index, 0);
}

static void test_wasd(void)
{
    CHECK_EQ(purr_key_from_char('w'), PURR_KEY_UP);
    CHECK_EQ(purr_key_from_char('W'), PURR_KEY_UP);
    CHECK_EQ(purr_key_from_char('s'), PURR_KEY_DOWN);
    CHECK_EQ(purr_key_from_char('S'), PURR_KEY_DOWN);
    CHECK_EQ(purr_key_from_char('d'), PURR_KEY_ENTER);
    CHECK_EQ(purr_key_from_char('\r'), PURR_KEY_ENTER);
    CHECK_EQ(purr_key_from_char('\n'), PURR_KEY_ENTER);
    CHECK_EQ(purr_key_from_char('x'), PURR_KEY_NONE);
    CHECK_EQ(purr_key_from_char('\0'), PURR_KEY_NONE);
}

static void test_nothing_to_boot(void)
{
    purr_menu_t m;
    purr_menu_init(&m, none, 2, -1);
    CHECK_EQ(m.no_boot_options, 1);
    CHECK_EQ(m.count, 1);
    CHECK_EQ(purr_menu_visible(&m), 0);
    CHECK_EQ(purr_menu_seconds_left(&m), -1);   /* never auto-boots */

    /* Hidden for two seconds, and keys are ignored meanwhile. */
    CHECK_EQ(purr_menu_step(&m, 1000, PURR_KEY_ENTER).action, PURR_MENU_ACT_NONE);
    CHECK_EQ(purr_menu_visible(&m), 0);
    CHECK_EQ(purr_menu_step(&m, 999, PURR_KEY_NONE).action, PURR_MENU_ACT_NONE);
    CHECK_EQ(purr_menu_visible(&m), 0);
    CHECK_EQ(purr_menu_step(&m, 1, PURR_KEY_NONE).action, PURR_MENU_ACT_NONE);
    CHECK_EQ(purr_menu_visible(&m), 1);

    /* Shown, and waits for a choice however long it takes. */
    CHECK_EQ(purr_menu_step(&m, 600000, PURR_KEY_NONE).action, PURR_MENU_ACT_NONE);
    CHECK_EQ(purr_menu_step(&m, 10, PURR_KEY_ENTER).action, PURR_MENU_ACT_INTERNET_RECOVERY);

    /* No partitions at all is the same case. */
    purr_menu_init(&m, none, 0, -1);
    CHECK_EQ(m.no_boot_options, 1);
}

int main(void)
{
    test_entries();
    test_countdown();
    test_keys();
    test_wasd();
    test_nothing_to_boot();
    TK_DONE("test_menu");
}
