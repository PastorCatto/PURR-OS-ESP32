#include <string.h>

#include "purr_board.h"
#include "purr_driver.h"
#include "purr_driver_class.h"
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

/* A fake handle: the byte at *handle, incremented once per read(). Exercises purr_device_read()
 * actually reaching the driver's read() with the real bound handle, not just a NULL check. */
static int readable_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    (void)dev; (void)board;
    static uint8_t counter;
    counter = 0;
    *out_handle = &counter;
    return 0;
}
static void readable_remove(void *handle) { (void)handle; }
static int readable_read(void *handle, uint8_t *data, size_t len)
{
    uint8_t *counter = (uint8_t *)handle;
    if (len == 0) {
        return -1;
    }
    data[0] = ++(*counter);
    return 1;
}

static const purr_driver_t s_ok_driver = {
    .name = "ok", .compatible = "fake-ok", .probe = ok_probe, .remove = ok_remove,
};
static const purr_driver_t s_failing_driver = {
    .name = "failing", .compatible = "fake-failing", .probe = failing_probe, .remove = failing_remove,
};
static const purr_driver_t s_readable_driver = {
    .name = "readable", .compatible = "fake-readable",
    .probe = readable_probe, .remove = readable_remove, .read = readable_read,
};

static const purr_driver_t *const s_table[] = {&s_ok_driver, &s_failing_driver, &s_readable_driver};
#define TABLE_COUNT (int)(sizeof(s_table) / sizeof(s_table[0]))

/* Two distinct driver identities sharing the same compatible string and probe/remove bodies --
 * enough to tell "which driver is actually bound" apart by pointer (purr_device_driver()),
 * the way a real reloaded/replacement driver module would be a different purr_driver_t too. */
static int s_swap_probe_calls;
static int s_swap_remove_calls;
static int swap_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    (void)board;
    s_swap_probe_calls++;
    *out_handle = (void *)dev->name;
    return 0;
}
static void swap_remove(void *handle) { (void)handle; s_swap_remove_calls++; }
static const purr_driver_t s_swap_driver_a = {
    .name = "swap-a", .compatible = "fake-swap", .probe = swap_probe, .remove = swap_remove,
};
static const purr_driver_t s_swap_driver_b = {
    .name = "swap-b", .compatible = "fake-swap", .probe = swap_probe, .remove = swap_remove,
};

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

static void test_device_read_reaches_the_bound_drivers_read(void)
{
    purr_pins_reset();
    purr_device_t devs[] = {basic_device("sensor", "fake-readable", PURR_PRI_OPTIONAL)};
    purr_board_t b = make_board(devs, 1);
    purr_drivers_bind(&b, s_table, TABLE_COUNT);

    uint8_t byte = 0;
    CHECK_EQ(purr_device_read("sensor", &byte, 1), 1);
    CHECK_EQ(byte, 1);
    CHECK_EQ(purr_device_read("sensor", &byte, 1), 1);
    CHECK_EQ(byte, 2);                /* a second call reaches the same live handle again */
}

static void test_device_read_without_a_read_fn_fails(void)
{
    /* s_ok_driver has no .read -- purr_device_read() must not crash on a NULL function
     * pointer, just report "this device can't do that." */
    purr_pins_reset();
    purr_device_t devs[] = {basic_device("thing", "fake-ok", PURR_PRI_OPTIONAL)};
    purr_board_t b = make_board(devs, 1);
    purr_drivers_bind(&b, s_table, TABLE_COUNT);

    uint8_t byte = 0;
    CHECK_EQ(purr_device_read("thing", &byte, 1), -1);
}

static void test_device_read_on_an_unbound_device_fails(void)
{
    purr_pins_reset();
    uint8_t byte = 0;
    CHECK_EQ(purr_device_read("nothing-ever-bound-this", &byte, 1), -1);
}

/* ---------------------------------------------------------------- register()/unregister() */

static void test_register_binds_a_matching_unbound_device(void)
{
    purr_pins_reset();
    purr_driver_registry_reset();
    s_swap_probe_calls = 0;
    purr_device_t devs[] = {basic_device("gadget", "fake-swap", PURR_PRI_OPTIONAL)};
    purr_board_t b = make_board(devs, 1);
    purr_drivers_bind(&b, NULL, 0);          /* cold boot: nothing matches yet */
    CHECK_EQ(purr_device_state("gadget"), PURR_DEV_NO_DRIVER);

    CHECK_EQ(purr_driver_register(&b, &s_swap_driver_a), 0);

    CHECK_EQ(purr_device_state("gadget"), PURR_DEV_BOUND);
    CHECK_EQ(s_swap_probe_calls, 1);
    CHECK(purr_device_driver("gadget") == &s_swap_driver_a);
}

static void test_register_does_not_steal_an_already_bound_device(void)
{
    purr_pins_reset();
    purr_driver_registry_reset();
    purr_device_t devs[] = {basic_device("thing", "fake-ok", PURR_PRI_OPTIONAL)};
    purr_board_t b = make_board(devs, 1);
    purr_drivers_bind(&b, s_table, TABLE_COUNT);
    CHECK_EQ(purr_device_state("thing"), PURR_DEV_BOUND);
    CHECK(purr_device_driver("thing") == &s_ok_driver);

    /* A second driver claiming the same compatible string must not steal a device that
     * already has a working driver -- that is unregister()+register()'s job, not register()
     * alone. */
    s_swap_probe_calls = 0;
    static const purr_driver_t s_impostor = {
        .name = "impostor", .compatible = "fake-ok", .probe = swap_probe, .remove = swap_remove,
    };
    CHECK_EQ(purr_driver_register(&b, &s_impostor), 0);
    CHECK(purr_device_driver("thing") == &s_ok_driver);
    CHECK_EQ(s_swap_probe_calls, 0);
}

/* A device flagged PURR_DEFER_NET (the T-Deck Plus LoRa radio) is neither probed nor given its pins until Wi-Fi has settled. */
static void test_defer_net_waits_for_wifi_then_binds(void)
{
    purr_pins_reset();
    purr_driver_registry_reset();
    s_swap_probe_calls = 0;
    purr_device_t devs[] = {basic_device("radio", "fake-swap", PURR_PRI_OPTIONAL), basic_device("other", "fake-ok", PURR_PRI_OPTIONAL)};
    devs[0].defer = PURR_DEFER_NET;
    devs[0].extra_pins[0] = 9;
    purr_board_t b = make_board(devs, 2);
    purr_drivers_bind(&b, s_table, TABLE_COUNT);
    CHECK_EQ(purr_device_state("radio"), PURR_DEV_DEFERRED);
    CHECK_EQ(purr_device_state("other"), PURR_DEV_BOUND);                     /* only the flagged device waits */
    CHECK(purr_pins_owner(9) == NULL);                                         /* its pins are still free */

    CHECK_EQ(purr_driver_register(&b, &s_swap_driver_a), 0);                   /* the driver arrives while Wi-Fi starts: still waiting, not probed */
    CHECK_EQ(purr_device_state("radio"), PURR_DEV_DEFERRED);
    CHECK_EQ(s_swap_probe_calls, 0);
    CHECK_EQ(purr_drivers_net_is_settled(), 0);

    purr_drivers_net_settled(&b);                                              /* Wi-Fi settled: now it binds */
    CHECK_EQ(purr_drivers_net_is_settled(), 1);
    CHECK_EQ(purr_device_state("radio"), PURR_DEV_BOUND);
    CHECK_EQ(s_swap_probe_calls, 1);
    CHECK(purr_device_driver("radio") == &s_swap_driver_a);
    CHECK(purr_pins_owner(9) != NULL);

    purr_drivers_net_settled(&b);                                              /* calling it again changes nothing */
    CHECK_EQ(s_swap_probe_calls, 1);
}

static void test_defer_net_with_no_driver_reports_no_driver_after_settling(void)
{
    purr_pins_reset();
    purr_driver_registry_reset();
    purr_device_t devs[] = {basic_device("radio", "fake-missing", PURR_PRI_OPTIONAL)};
    devs[0].defer = PURR_DEFER_NET;
    purr_board_t b = make_board(devs, 1);
    purr_drivers_bind(&b, s_table, TABLE_COUNT);
    CHECK_EQ(purr_device_state("radio"), PURR_DEV_DEFERRED);
    purr_drivers_net_settled(&b);
    CHECK_EQ(purr_device_state("radio"), PURR_DEV_NO_DRIVER);
}

static void test_a_flagged_device_registered_after_settling_binds_at_once(void)
{
    purr_pins_reset();
    purr_driver_registry_reset();
    s_swap_probe_calls = 0;
    purr_device_t devs[] = {basic_device("radio", "fake-swap", PURR_PRI_OPTIONAL)};
    devs[0].defer = PURR_DEFER_NET;
    purr_board_t b = make_board(devs, 1);
    purr_drivers_bind(&b, NULL, 0);
    purr_drivers_net_settled(&b);
    CHECK_EQ(purr_device_state("radio"), PURR_DEV_NO_DRIVER);
    CHECK_EQ(purr_driver_register(&b, &s_swap_driver_a), 0);
    CHECK_EQ(purr_device_state("radio"), PURR_DEV_BOUND);
    CHECK_EQ(s_swap_probe_calls, 1);
}

static void test_unregister_calls_remove_and_frees_the_device(void)
{
    purr_pins_reset();
    purr_driver_registry_reset();
    s_swap_probe_calls = 0;
    s_swap_remove_calls = 0;
    purr_device_t devs[] = {basic_device("gadget", "fake-swap", PURR_PRI_OPTIONAL)};
    purr_board_t b = make_board(devs, 1);
    purr_drivers_bind(&b, NULL, 0);
    purr_driver_register(&b, &s_swap_driver_a);
    CHECK_EQ(purr_device_state("gadget"), PURR_DEV_BOUND);

    purr_driver_unregister(&s_swap_driver_a);

    CHECK_EQ(s_swap_remove_calls, 1);
    CHECK_EQ(purr_device_state("gadget"), PURR_DEV_UNBOUND);
    CHECK(purr_device_handle("gadget") == NULL);
    CHECK(purr_device_driver("gadget") == NULL);
}

static void test_unregister_releases_pins_for_a_fresh_claim(void)
{
    purr_pins_reset();
    purr_driver_registry_reset();
    purr_device_t devs[1];
    devs[0] = basic_device("gadget", "fake-swap", PURR_PRI_OPTIONAL);
    devs[0].extra_pins[0] = 9;
    purr_board_t b = make_board(devs, 1);
    purr_drivers_bind(&b, NULL, 0);
    purr_driver_register(&b, &s_swap_driver_a);
    CHECK(strcmp(purr_pins_owner(9), "gadget") == 0);

    purr_driver_unregister(&s_swap_driver_a);

    CHECK(purr_pins_owner(9) == NULL);
    CHECK_EQ(purr_pins_claim(9, "someone-else"), 0);
}

static void test_register_after_unregister_rebinds_to_the_new_driver(void)
{
    /* The actual hot-swap scenario: unregister() the running driver, register() its
     * replacement -- the device ends up bound to the new one, not stuck unbound and not
     * still pointing at the old (now-unregistered) driver. */
    purr_pins_reset();
    purr_driver_registry_reset();
    purr_device_t devs[] = {basic_device("gadget", "fake-swap", PURR_PRI_OPTIONAL)};
    purr_board_t b = make_board(devs, 1);
    purr_drivers_bind(&b, NULL, 0);
    purr_driver_register(&b, &s_swap_driver_a);
    CHECK(purr_device_driver("gadget") == &s_swap_driver_a);

    purr_driver_unregister(&s_swap_driver_a);
    CHECK_EQ(purr_driver_register(&b, &s_swap_driver_b), 0);

    CHECK_EQ(purr_device_state("gadget"), PURR_DEV_BOUND);
    CHECK(purr_device_driver("gadget") == &s_swap_driver_b);
}

/* Devices that arrive after boot (the discovered profile): a second board object whose device list is separate, bound against the drivers
 * registered so far, and again when a later driver arrives. */
static void test_bind_extra_for_discovered_devices(void)
{
    purr_pins_reset();
    purr_driver_registry_reset();
    s_probe_calls = 0;
    static const purr_device_t devs[] = {
        {.name = "disc0", .compatible = "fake-ok", .priority = PURR_PRI_OPTIONAL, .extra_pins = {PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE}},
        {.name = "disc1", .compatible = "fake-readable", .priority = PURR_PRI_OPTIONAL, .extra_pins = {PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE}},
    };
    purr_board_t b = {.name = "discovered", .devices = devs, .device_count = 2};
    CHECK_EQ(purr_driver_register(NULL, &s_ok_driver), 0);       /* the driver arrives before the device does */
    purr_drivers_bind_extra(&b);
    CHECK_EQ(purr_device_state("disc0"), PURR_DEV_BOUND);
    CHECK_EQ(purr_device_state("disc1"), PURR_DEV_NO_DRIVER);
    CHECK_EQ(s_probe_calls, 1);
    CHECK_EQ(purr_driver_register(NULL, &s_readable_driver), 0);   /* a driver that arrives later */
    purr_drivers_bind_extra(&b);
    CHECK_EQ(purr_device_state("disc1"), PURR_DEV_BOUND);
    CHECK_EQ(s_probe_calls, 1);                                  /* the bound one is never probed again */
    purr_drivers_bind_extra(NULL);                               /* harmless */
    purr_driver_registry_reset();
    purr_pins_reset();
}


/* ---------------------------------------------------------------- driver classes (bus ABI 6) */

static int fake_get_fix(void *h, purr_gnss_fix_t *out)
{
    (void)h;
    memset(out, 0, sizeof(*out));
    out->valid = 1;
    out->lat_e7 = 515000000;
    return 1;
}
static const purr_gnss_ops_t s_gnss_ops = {.version = 1, .get_fix = fake_get_fix};

static const purr_driver_t s_gnss_driver = {
    .name = "gnss", .compatible = "fake-gnss", .probe = ok_probe, .remove = ok_remove,
    .bus_abi = 6, .class_id = PURR_DRV_CLASS_GNSS, .ops = &s_gnss_ops,
};
/* A driver built for ABI 5: the kernel must never read its (absent) class fields. We fake the "garbage past the end" by setting them non-zero. */
static const purr_driver_t s_old_driver = {
    .name = "old", .compatible = "fake-old", .probe = ok_probe, .remove = ok_remove,
    .bus_abi = 5, .class_id = PURR_DRV_CLASS_LORA, .ops = &s_gnss_ops,
};
/* A compiled-in driver (bus_abi 0) may carry a class: it is built with the same header as the kernel. */
static const purr_driver_t s_builtin_lora = {
    .name = "lora", .compatible = "fake-lora", .probe = ok_probe, .remove = ok_remove,
    .bus_abi = 0, .class_id = PURR_DRV_CLASS_LORA, .ops = &s_gnss_ops,
};

static void test_driver_classes(void)
{
    purr_pins_reset();
    purr_driver_registry_reset();
    static const purr_device_t devs[] = {
        {.name = "gps0", .compatible = "fake-gnss", .extra_pins = {43, 44, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE}},
        {.name = "old0", .compatible = "fake-old", .extra_pins = {PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE}},
        {.name = "lora0", .compatible = "fake-lora", .extra_pins = {PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE}},
    };
    purr_board_t b = {.name = "fake", .devices = devs, .device_count = 3};
    const purr_driver_t *table[] = {&s_gnss_driver, &s_old_driver, &s_builtin_lora};
    purr_drivers_bind(&b, table, 3);
    CHECK_EQ(purr_device_state("gps0"), PURR_DEV_BOUND);
    CHECK_EQ(purr_device_state("old0"), PURR_DEV_BOUND);

    CHECK_EQ((int)purr_driver_class_id(&s_gnss_driver), PURR_DRV_CLASS_GNSS);
    CHECK_EQ((int)purr_driver_class_id(&s_old_driver), 0);          /* too old to have a class: never read */
    CHECK_EQ((int)purr_driver_class_id(&s_builtin_lora), PURR_DRV_CLASS_LORA);
    CHECK_EQ((int)purr_driver_class_id(NULL), 0);

    purr_class_entry_t e[4];
    CHECK_EQ(purr_class_find(PURR_DRV_CLASS_GNSS, e, 4), 1);
    CHECK(strcmp(e[0].device, "gps0") == 0);
    CHECK(e[0].ops == &s_gnss_ops);
    CHECK_EQ(purr_class_find(PURR_DRV_CLASS_LORA, e, 4), 1);        /* the built-in one only; the ABI 5 driver is not a LoRa radio */
    CHECK(strcmp(e[0].device, "lora0") == 0);
    CHECK_EQ(purr_class_find(PURR_DRV_CLASS_MODEM, e, 4), 0);
    CHECK_EQ(purr_class_find(0, e, 4), 0);                          /* "no class" is not something to ask for */

    void *h = NULL;
    const purr_gnss_ops_t *ops = purr_class_ops(PURR_DRV_CLASS_GNSS, &h);
    CHECK(ops != NULL && h != NULL);
    purr_gnss_fix_t fix;
    CHECK_EQ(ops->get_fix(h, &fix), 1);
    CHECK_EQ((int)fix.lat_e7, 515000000);
    CHECK(purr_class_ops(PURR_DRV_CLASS_HCI, NULL) == NULL);

    purr_driver_unregister(&s_gnss_driver);                          /* unbinding takes it out of the class registry too */
    CHECK_EQ(purr_class_find(PURR_DRV_CLASS_GNSS, e, 4), 0);
    CHECK(strcmp(purr_drv_class_name(PURR_DRV_CLASS_HCI), "hci") == 0);
    purr_driver_registry_reset();
    purr_pins_reset();
}

static void test_kv_config(void)
{
    purr_kv_cfg_t c = {.count = 3, .kv = {{"tx", 43}, {"rx", 44}, {"baud", 38400}}};
    CHECK_EQ((int)purr_kv(&c, "tx", -1), 43);
    CHECK_EQ((int)purr_kv(&c, "baud", 9600), 38400);
    CHECK_EQ((int)purr_kv(&c, "cs", -7), -7);                        /* absent: the fallback */
    CHECK_EQ((int)purr_kv(&c, "t", -7), -7);                         /* a prefix is not a match */
    CHECK_EQ((int)purr_kv(&c, "txx", -7), -7);
    CHECK_EQ((int)purr_kv(NULL, "tx", 5), 5);
}

/* ---------------------------------------------------------------- devices the kernel declares at run time (purr_device_add) */

static void test_a_device_added_at_run_time_binds_and_is_found_by_a_later_driver(void)
{
    purr_pins_reset();
    purr_driver_registry_reset();
    s_swap_probe_calls = 0;
    purr_device_t devs[] = {basic_device("gadget", "fake-ok", PURR_PRI_OPTIONAL)};
    purr_board_t b = make_board(devs, 1);
    purr_drivers_bind(&b, s_table, 3);
    static purr_device_t nic;
    nic = basic_device("eth0", "fake-swap", PURR_PRI_OPTIONAL);

    CHECK_EQ(purr_device_add(&b, &nic), 0);                              /* nobody offers fake-swap yet */
    CHECK_EQ(purr_device_state("eth0"), PURR_DEV_NO_DRIVER);
    CHECK_EQ(purr_device_add(&b, &nic), -1);                             /* the name is taken */
    static purr_device_t dup;
    dup = basic_device("gadget", "fake-swap", PURR_PRI_OPTIONAL);
    CHECK_EQ(purr_device_add(&b, &dup), -1);                             /* so is a name the board lists */

    CHECK_EQ(purr_driver_register(&b, &s_swap_driver_a), 0);             /* the module arrives later and finds it */
    CHECK_EQ(purr_device_state("eth0"), PURR_DEV_BOUND);
    CHECK_EQ(s_swap_probe_calls, 1);
    CHECK(purr_device_driver("eth0") == &s_swap_driver_a);
    CHECK_EQ(purr_device_state("gadget"), PURR_DEV_BOUND);               /* the board's own device is untouched */

    /* and when the driver is there first, the device binds at once */
    purr_driver_registry_reset();
    purr_drivers_bind(&b, s_table, 3);
    CHECK_EQ(purr_driver_register(&b, &s_swap_driver_a), 0);
    static purr_device_t nic2;
    nic2 = basic_device("eth1", "fake-swap", PURR_PRI_OPTIONAL);
    CHECK_EQ(purr_device_add(&b, &nic2), 0);
    CHECK_EQ(purr_device_state("eth1"), PURR_DEV_BOUND);

    /* a full rebind starts the declared list over */
    purr_drivers_bind(&b, s_table, 3);
    CHECK_EQ(purr_device_add(&b, &nic2), 0);
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
    test_bind_extra_for_discovered_devices();
    test_device_read_reaches_the_bound_drivers_read();
    test_device_read_without_a_read_fn_fails();
    test_device_read_on_an_unbound_device_fails();
    test_register_binds_a_matching_unbound_device();
    test_register_does_not_steal_an_already_bound_device();
    test_defer_net_waits_for_wifi_then_binds();
    test_defer_net_with_no_driver_reports_no_driver_after_settling();
    test_a_flagged_device_registered_after_settling_binds_at_once();
    test_unregister_calls_remove_and_frees_the_device();
    test_unregister_releases_pins_for_a_fresh_claim();
    test_register_after_unregister_rebinds_to_the_new_driver();
    test_driver_classes();
    test_kv_config();
    test_a_device_added_at_run_time_binds_and_is_found_by_a_later_driver();
    TK_DONE("test_driver");
}
