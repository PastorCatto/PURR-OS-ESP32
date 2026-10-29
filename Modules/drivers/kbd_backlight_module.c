/* T-Deck keyboard backlight (I2C). A board device on the T-Deck Plus, and still usable by hand (hwdev add kbd-backlight 18 8 0x55).
 *
 * The T-Deck keyboard is its own little controller at 0x55. Besides handing out key presses it takes a two-byte command to light the keys:
 * [0x01, level] sets the backlight brightness (0 = off, 255 = full). This follows LilyGO's published keyboard firmware; if a unit ignores it, the
 * keys simply stay dark and nothing else is affected.
 *
 * The device sits on the board's own bus (the same wires as the keyboard and touch), so it is not something "Find hardware" scans: it is added by hand.
 * read() is a test hook: each read steps the brightness through off, low, medium, full and returns the new level as one byte, so
 * `hwdev read kbd-backlight_0` shows the effect on the keys without a settings screen.
 */
#include "purr_driver.h"
#include "purr_driver_bus.h"

static const purr_driver_bus_table_t *s_bus;

typedef struct {
    uint32_t h;
    uint8_t step;
    int in_use;
} kb_t;

static kb_t s_dev[2];

static const uint8_t k_levels[] = {0, 64, 128, 255};

static int set_level(uint32_t h, uint8_t level)
{
    uint8_t cmd[2] = {0x01, level};
    return s_bus->i2c_write(h, cmd, 2, 50);
}

static int kb_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    (void)board;
    const purr_i2c_dev_cfg_t *c = (const purr_i2c_dev_cfg_t *)dev->cfg;
    if (c == NULL) {
        return -1;
    }
    kb_t *d = NULL;
    for (int i = 0; i < 2; i++) {
        if (!s_dev[i].in_use) {
            d = &s_dev[i];
            break;
        }
    }
    if (d == NULL) {
        return -1;
    }
    uint32_t h = s_bus->i2c_add_device(c->port, c->sda, c->scl, c->addr, (int)c->hz);
    if (h == 0) {
        return -1;
    }
    if (set_level(h, k_levels[2]) != 0) {            /* nothing acknowledged: not a keyboard controller */
        s_bus->i2c_remove_device(h);
        return -1;
    }
    d->h = h;
    d->step = 2;
    d->in_use = 1;
    *out_handle = d;
    return 0;
}

static void kb_remove(void *handle)
{
    kb_t *d = handle;
    if (d != NULL && d->in_use) {
        set_level(d->h, 0);
        s_bus->i2c_remove_device(d->h);
        d->in_use = 0;
    }
}

/* read() is also the control channel (a driver has no write hook): with two or more bytes in the buffer, data[0] = 0xA5 sets the level to data[1]
 * and 0xA6 only reports it. Either answers with the level in data[0]. Anything else steps the level, as the test hook always did. */
#define KB_SET 0xA5
#define KB_GET 0xA6

static int kb_read(void *handle, uint8_t *data, size_t len)
{
    kb_t *d = handle;
    if (d == NULL || !d->in_use || len < 1) {
        return -1;
    }
    if (len >= 2 && data[0] == KB_GET) {
        data[0] = k_levels[d->step];
        return 1;
    }
    if (len >= 2 && data[0] == KB_SET) {
        uint8_t want = data[1];
        uint8_t best = 0;
        for (unsigned i = 0; i < sizeof(k_levels) / sizeof(k_levels[0]); i++) {     /* the nearest of the four steps */
            int a = (int)k_levels[i] - (int)want, b = (int)k_levels[best] - (int)want;
            if ((a < 0 ? -a : a) < (b < 0 ? -b : b)) {
                best = (uint8_t)i;
            }
        }
        if (set_level(d->h, k_levels[best]) != 0) {
            return -1;
        }
        d->step = best;
        data[0] = k_levels[best];
        return 1;
    }
    d->step = (uint8_t)((d->step + 1) % (sizeof(k_levels) / sizeof(k_levels[0])));
    if (set_level(d->h, k_levels[d->step]) != 0) {
        return -1;
    }
    data[0] = k_levels[d->step];
    return 1;
}

static const purr_driver_t s_driver = {
    .name = "kbd-backlight", .compatible = "kbd-backlight",
    .probe = kb_probe, .remove = kb_remove, .read = kb_read,
    .bus_abi = PURR_DRIVER_BUS_ABI_VERSION,
};

__attribute__((used))
const purr_driver_t *kbd_backlight_entry(const purr_driver_bus_table_t *bus)
{
    s_bus = bus;
    return &s_driver;
}
