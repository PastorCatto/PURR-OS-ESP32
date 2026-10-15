/* Real loadable keyboard driver -- converted off purr_kernel.c's compiled-in kbd_probe/
 * kbd_read/kbd_remove (Drivers/SPEC.md section 4's condition -- loadable native modules --
 * is satisfied now; purr_driver_bus.h's generic read() is what made this possible). Same
 * i2c_master_bus/device bring-up and the same 20ms poll timeout the compiled-in version used,
 * just reached through the bus table's opaque handles instead of linking against ESP-IDF
 * directly -- this module is freestanding (Modules/SPEC.md section 2/4), it cannot link
 * against i2c_master.h at all.
 *
 * Not wired into real boot yet: proven standalone first, same incremental discipline
 * hello_driver_module.c used before any real subsystem converted. The static s_drivers[]
 * table in purr_kernel.c still has the compiled-in version; this module exists to be tested
 * against real hardware before that gets touched.
 */
#include "purr_driver.h"
#include "purr_driver_bus.h"

static const purr_driver_bus_table_t *s_bus;

static int kbd_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    (void)board;
    const purr_keyboard_cfg_t *k = (const purr_keyboard_cfg_t *)dev->cfg;
    if (k->addr == 0) {
        return -1;                /* this board has no keyboard */
    }
    uint32_t h = s_bus->i2c_add_device(k->port, k->sda, k->scl, k->addr, (int)k->hz);
    if (h == 0) {
        return -1;
    }
    *out_handle = (void *)(uintptr_t)h;
    return 0;
}

static void kbd_remove(void *handle)
{
    (void)handle;
}

static int kbd_read(void *handle, uint8_t *data, size_t len)
{
    uint32_t h = (uint32_t)(uintptr_t)handle;
    if (h == 0 || len == 0) {
        return -1;
    }
    return s_bus->i2c_read(h, data, len, 20) == 0 ? (int)len : -1;
}

static const purr_driver_t s_kbd_driver = {
    .name = "kbd-i2c", .compatible = "kbd-i2c",
    .probe = kbd_probe, .remove = kbd_remove, .read = kbd_read,
};

__attribute__((used))
const purr_driver_t *kbd_i2c_entry(const purr_driver_bus_table_t *bus)
{
    s_bus = bus;
    return &s_kbd_driver;
}
