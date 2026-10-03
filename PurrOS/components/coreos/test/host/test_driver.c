#include <string.h>

#include "purr_board.h"
#include "purr_driver.h"
#include "testkit.h"

/* ---------------------------------------------------------------- fake drivers */

static int s_probe_calls;

static int ok_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    (void)board;
    s_probe_calls++;
    *out_handle = (void *)dev->name;      /* any non-NULL, distinguishable value */
    return 0;
}
static void ok_remove(void *handle) { (void)handle; }

static int failing_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    (void)dev; (void)board; (void)out_handle;
    return -1;
}
static void failing_remove(void *handle) { (void)handle; }

static const purr_driver_t s_ok_driver = {
    .name = "ok", .compatible = "fake-ok", .probe = ok_probe, .remove = ok_remove,
};
static const purr_driver_t s_failing_driver = {
    .name = "failing", .compatible = "fake-failing", .probe = failing_probe, .remove = failing_remove,
};

static const purr_driver_t *const s_table[] = {&s_ok_driver, &s_failing_driver};
#define TABLE_COUNT (int)(sizeof(s_table) / sizeof(s_table[0]))

/* ---------------------------------------------------------------- fake board helper */

static purr_board_t make_board(purr_device_t *devices, int count)
{
    purr_board_t b;
    memset(&b, 0, sizeof(b));
    b.devices = devices;
    b.device_count = count;
    return b;
}

static purr_device_t basic_device(const char *name, const char *compatible, purr_priority_t pri)
{
    purr_device_t d;
    memset(&d, 0, sizeof(d));
    d.name = name;
    d.compatible = compatible;
    d.priority = pri;
    for (int i = 0; i < PURR_MAX_EXTRA_PINS; i++) {
        d.extra_pins[i] = PURR_PIN_NONE;
    }
    return d;
}

/* ---------------------------------------------------------------- tests */

static void test_matching_binds_a_device(void)
{
    purr_pins_reset();
    s_probe_calls = 0;
    purr_device_t devs[] = {basic_device("thing", "fake-ok", PURR_PRI_REQUIRED)};
    purr_board_t b = make_board(devs, 1);

    purr_drivers_bind(&b, s_table, TABLE_COUNT);

    CHECK_EQ(purr_device_state("thing"), PURR_DEV_BOUND);
    CHECK_EQ(s_probe_calls, 1);
    CHECK(purr_device_handle("thing") != NULL);
}

static void test_no_driver_found(void)
{
    purr_pins_reset();
    purr_device_t devs[] = {basic_device("mystery", "no-such-driver", PURR_PRI_OPTIONAL)};
    purr_board_t b = make_board(devs, 1);

    purr_drivers_bind(&b, s_table, TABLE_COUNT);

    CHECK_EQ(purr_device_state("mystery"), PURR_DEV_NO_DRIVER);
    CHECK(purr_device_handle("mystery") == NULL);
}

static void test_probe_failure_is_recorded_and_isolated(void)
{
    /* kernel/SPEC.md section 5: the kernel never panics on a failed driver, and one
     * device's failure never stops the others from binding. */
    purr_pins_reset();
    purr_device_t devs[] = {
        basic_device("broken", "fake-failing", PURR_PRI_REQUIRED),
        basic_device("fine", "fake-ok", PURR_PRI_OPTIONAL),
    };
    purr_board_t b = make_board(devs, 2);

    purr_drivers_bind(&b, s_table, TABLE_COUNT);

    CHECK_EQ(purr_device_state("broken"), PURR_DEV_PROBE_FAILED);
    CHECK_EQ(purr_device_state("fine"), PURR_DEV_BOUND);
}

static void test_pin_conflict_stops_that_device_only(void)
{
    purr_pins_reset();
    purr_device_t devs[2];
    devs[0] = basic_device("first", "fake-ok", PURR_PRI_REQUIRED);
    devs[0].extra_pins[0] = 7;
    devs[1] = basic_device("second", "fake-ok", PURR_PRI_OPTIONAL);
    devs[1].extra_pins[0] = 7;              /* same pin as "first" */
    purr_board_t b = make_board(devs, 2);

    purr_drivers_bind(&b, s_table, TABLE_COUNT);

    CHECK_EQ(purr_device_state("first"), PURR_DEV_BOUND);
    CHECK_EQ(purr_device_state("second"), PURR_DEV_PIN_CONFLICT);
    CHECK(strcmp(purr_pins_owner(7), "first") == 0);   /* the first claim wins */
}

static void test_pin_none_never_conflicts(void)
{
    purr_pins_reset();
    CHECK_EQ(purr_pins_claim(PURR_PIN_NONE, "a"), 0);
    CHECK_EQ(purr_pins_claim(PURR_PIN_NONE, "b"), 0);
    CHECK(purr_pins_owner(PURR_PIN_NONE) == NULL);
}

static void test_same_owner_can_reclaim_its_own_pin(void)
{
    /* A device with the same pin listed twice (or re-bound) is not a self-conflict. */
    purr_pins_reset();
    CHECK_EQ(purr_pins_claim(5, "owner"), 0);
    CHECK_EQ(purr_pins_claim(5, "owner"), 0);
    CHECK_EQ(purr_pins_claim(5, "someone-else"), 1);
}

static void test_priority_is_just_recorded(void)
{
    /* kernel/SPEC.md section 3: the kernel only records the level; it is not itself the
     * thing that decides a required device's failure is fatal. */
    purr_pins_reset();
    purr_device_t devs[] = {basic_device("critical", "fake-failing", PURR_PRI_REQUIRED)};
    purr_board_t b = make_board(devs, 1);

    purr_drivers_bind(&b, s_table, TABLE_COUNT);

    CHECK_EQ(purr_device_state("critical"), PURR_DEV_PROBE_FAILED);
    CHECK_EQ(devs[0].priority, PURR_PRI_REQUIRED);     /* unexamined by the registry itself */
}

static void test_rebind_resets_previous_state(void)
{
    purr_pins_reset();
    purr_device_t devs[] = {basic_device("flaky", "fake-ok", PURR_PRI_OPTIONAL)};
    purr_board_t b = make_board(devs, 1);
    purr_drivers_bind(&b, s_table, TABLE_COUNT);
    CHECK_EQ(purr_device_state("flaky"), PURR_DEV_BOUND);

    /* A second bind against a board that no longer lists the device: its old state must
     * not linger as a stale PURR_DEV_BOUND from the previous call. */
    purr_board_t empty = make_board(NULL, 0);
    purr_drivers_bind(&empty, s_table, TABLE_COUNT);
    CHECK_EQ(purr_device_state("flaky"), PURR_DEV_UNBOUND);
}

/* kernel/SPEC.md section 2's own acceptance test: "a fake driver and a fake board are added
 * in the host tests with no change to kernel code." s_ok_driver/s_failing_driver (this file's
 * own fake drivers, declared above with zero changes to purr_driver.c) and a fake board built
 * entirely in this test are exactly that -- every test above already demonstrates it; this
 * case names the claim explicitly rather than leaving it implicit. */
static void test_plug_and_play_acceptance(void)
{
    purr_pins_reset();
    purr_device_t devs[] = {basic_device("acceptance", "fake-ok", PURR_PRI_IMPORTANT)};
    purr_board_t b = make_board(devs, 1);
    purr_drivers_bind(&b, s_table, TABLE_COUNT);
    CHECK_EQ(purr_device_state("acceptance"), PURR_DEV_BOUND);
}

int main(void)
{
    test_matching_binds_a_device();
    test_no_driver_found();
    test_probe_failure_is_recorded_and_isolated();
    test_pin_conflict_stops_that_device_only();
    test_pin_none_never_conflicts();
    test_same_owner_can_reclaim_its_own_pin();
    test_priority_is_just_recorded();
    test_rebind_resets_previous_state();
    test_plug_and_play_acceptance();
    TK_DONE("test_driver");
}
