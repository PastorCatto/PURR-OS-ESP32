/* First proof of the loadable-driver mechanism (Drivers/SPEC.md section 4, kernel/SPEC.md
 * section 5's "separate driver modules, with the same container, come later" -- that
 * condition is met now, Modules/SPEC.md's mechanism having already passed). Same incremental
 * discipline hello_module.c/about_module.c already proved for command modules before any real
 * subsystem converted: a trivial, harmless driver first -- touches no real hardware, proves
 * only that a driver module can be built, signed, loaded, relocated, mapped executable, and
 * called with a real purr_driver_bus_table_t, getting back a usable purr_driver_t -- a real
 * one (st7789 or gt911) is the next, separate step, once this is confirmed end to end on real
 * hardware. */
#include "purr_driver.h"
#include "purr_driver_bus.h"

static const purr_driver_bus_table_t *s_bus;

static int hello_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    (void)dev;
    (void)board;
    *out_handle = NULL;
    return 0;
}

static void hello_remove(void *handle)
{
    (void)handle;
}

static const purr_driver_t s_hello_driver = {
    .name = "hello_driver",
    .compatible = "hello_driver",
    .probe = hello_probe,
    .remove = hello_remove,
    .bus_abi = PURR_DRIVER_BUS_ABI_VERSION,
};

__attribute__((used))
const purr_driver_t *hello_driver_entry(const purr_driver_bus_table_t *bus)
{
    s_bus = bus;
    return &s_hello_driver;
}
