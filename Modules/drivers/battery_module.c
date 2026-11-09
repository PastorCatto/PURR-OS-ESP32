/* Battery gauge for boards that have no fuel-gauge chip: the cell's voltage through a resistor divider into an ADC pin (T-Deck: GPIO 4, 1:2).
 *
 * The device cfg is a purr_adc_dev_cfg_t {pin, mul, div}: the real voltage is pin millivolts * mul / div. read() hands back four bytes:
 *   [0] charge in percent (0..100), from a typical single Li-ion discharge curve
 *   [1] [2] the cell voltage in millivolts, little-endian
 *   [3] flags: bit 0 set when the reading looks like a connected cell (2.8 V and up; clear means no battery, or one far too flat)
 *               bit 1 set when the line is held above what a cell rests at (over 4.3 V): the charger or USB power is lifting it
 * The charger lifts this line above 4.4 V, which the first version of this driver read as "no battery" (the icon vanished while plugged in). The
 * upper limit is now the ADC's own (twice the pin's 3.3 V). Percent from voltage is an estimate: it moves with load and temperature, and while
 * charging it reads 100. The OS smooths it.
 */
#include "purr_driver.h"
#include "purr_driver_bus.h"

static const purr_driver_bus_table_t *s_bus;

typedef struct {
    int pin, mul, div;
    int in_use;
} bat_t;

static bat_t s_dev;

static const struct {
    uint16_t mv;
    uint8_t pct;
} k_curve[] = {
    {4200, 100}, {4100, 90}, {4000, 80}, {3920, 70}, {3850, 60}, {3800, 50},
    {3750, 40},  {3700, 30}, {3650, 20}, {3550, 10}, {3400, 3},  {3300, 0},
};

static int percent_of(int mv)
{
    const int n = (int)(sizeof(k_curve) / sizeof(k_curve[0]));
    if (mv >= k_curve[0].mv) {
        return 100;
    }
    if (mv <= k_curve[n - 1].mv) {
        return 0;
    }
    for (int i = 1; i < n; i++) {
        if (mv >= k_curve[i].mv) {
            int span = k_curve[i - 1].mv - k_curve[i].mv;
            int rise = k_curve[i - 1].pct - k_curve[i].pct;
            return k_curve[i].pct + (mv - k_curve[i].mv) * rise / span;
        }
    }
    return 0;
}

static int bat_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    (void)board;
    const purr_adc_dev_cfg_t *c = (const purr_adc_dev_cfg_t *)dev->cfg;
    if (c == NULL || s_dev.in_use || c->div <= 0 || s_bus->adc_read_mv == NULL) {
        return -1;
    }
    if (s_bus->adc_read_mv(c->pin) < 0) {                  /* not an ADC pin, or the converter would not start */
        return -1;
    }
    s_dev.pin = c->pin;
    s_dev.mul = c->mul;
    s_dev.div = c->div;
    s_dev.in_use = 1;
    *out_handle = &s_dev;
    return 0;
}

static void bat_remove(void *handle)
{
    bat_t *d = handle;
    if (d != NULL) {
        d->in_use = 0;
    }
}

static int bat_read(void *handle, uint8_t *data, size_t len)
{
    bat_t *d = handle;
    if (d == NULL || !d->in_use || len < 4) {
        return -1;
    }
    int pin_mv = s_bus->adc_read_mv(d->pin);
    if (pin_mv < 0) {
        return -1;
    }
    int mv = pin_mv * d->mul / d->div;
    if (mv > 65535) {
        mv = 65535;
    }
    data[0] = (uint8_t)percent_of(mv);
    data[1] = (uint8_t)(mv & 0xFF);
    data[2] = (uint8_t)(mv >> 8);
    data[3] = (uint8_t)((mv >= 2800 ? 1 : 0) | (mv > 4300 ? 2 : 0));
    return 4;
}

static const purr_driver_t s_driver = {
    .name = "battery-adc", .compatible = "battery-adc",
    .probe = bat_probe, .remove = bat_remove, .read = bat_read,
    .bus_abi = PURR_DRIVER_BUS_ABI_VERSION,
};

__attribute__((used))
const purr_driver_t *battery_entry(const purr_driver_bus_table_t *bus)
{
    s_bus = bus;
    return &s_driver;
}
