/* T-Deck keyboard, raw key state (I2C, address 0x55). The channel a game uses to know which keys are HELD.
 *
 * In its normal mode the keyboard controller hands out one character per key press and says nothing about a release or a key being held, which is no use
 * to a game. In raw mode it reports the whole key matrix: five bytes, a bit per key. This follows LilyGO's published keyboard firmware and Retro-Go's T-Deck
 * target: the command byte 0x03 switches to raw mode and reading five bytes then returns the bitmap; 0x04 switches back to characters (that one is from
 * LilyGO's firmware too and has NOT been confirmed on a unit: if a unit stays in raw mode after a game, a restart brings the characters back).
 *
 * A driver has no write hook, so read() is the control channel (as in kbd-backlight):
 *     data[0] = 0xB1, data[1] = 1 or 0   raw mode on or off; answers with data[0] = 1 on success
 *     data[0] = 0xB2, len >= 6           reads the five raw bytes into data[1..5]; answers 5
 * While raw mode is on the ordinary keyboard driver would read these bytes as characters: only the game interface turns it on, and only while a game is in front.
 */
#include "purr_driver.h"
#include "purr_driver_bus.h"

static const purr_driver_bus_table_t *s_bus;

typedef struct {
    uint32_t h;
    int in_use;
    int raw;
} kr_t;

static kr_t s_dev[2];

#define KR_MODE 0xB1
#define KR_READ 0xB2
#define CMD_RAW 0x03
#define CMD_NORMAL 0x04

static int send_cmd(uint32_t h, uint8_t cmd)
{
    return s_bus->i2c_write(h, &cmd, 1, 50);
}

static int kr_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    (void)board;
    const purr_i2c_dev_cfg_t *c = (const purr_i2c_dev_cfg_t *)dev->cfg;
    if (c == NULL) {
        return -1;
    }
    kr_t *d = NULL;
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
    d->h = h;
    d->in_use = 1;
    d->raw = 0;                                    /* probing changes nothing: the keyboard stays in character mode */
    *out_handle = d;
    return 0;
}

static void kr_remove(void *handle)
{
    kr_t *d = handle;
    if (d != NULL && d->in_use) {
        if (d->raw) {
            send_cmd(d->h, CMD_NORMAL);
        }
        s_bus->i2c_remove_device(d->h);
        d->in_use = 0;
    }
}

static int kr_read(void *handle, uint8_t *data, size_t len)
{
    kr_t *d = handle;
    if (d == NULL || !d->in_use || len < 2) {
        return -1;
    }
    if (data[0] == KR_MODE) {
        int want = data[1] != 0;
        if (send_cmd(d->h, want ? CMD_RAW : CMD_NORMAL) != 0) {
            return -1;
        }
        d->raw = want;
        data[0] = 1;
        return 1;
    }
    if (data[0] == KR_READ && len >= 6) {
        if (!d->raw || s_bus->i2c_read(d->h, &data[1], 5, 20) != 0) {
            return -1;
        }
        return 5;
    }
    return -1;
}

static const purr_driver_t s_driver = {
    .name = "kbd-raw", .compatible = "kbd-raw",
    .probe = kr_probe, .remove = kr_remove, .read = kr_read,
    .bus_abi = PURR_DRIVER_BUS_ABI_VERSION,
};

__attribute__((used))
const purr_driver_t *kbd_raw_entry(const purr_driver_bus_table_t *bus)
{
    s_bus = bus;
    return &s_driver;
}
