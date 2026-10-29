/* A trackball made of four direction pulses and a click, each on its own GPIO (the T-Deck Plus's).
 *
 * A plain driver (no class): its read() returns four bytes, [dx, dy, click, 0], where dx and dy are the signed number of steps the ball turned since the last
 * read (right and down are positive) and click is 1 while the button is held. Configured by named values (purr_kv_cfg_t):
 *     up=3  down=15  left=1  right=2  click=0        the five pins (the direction pins may be -1 to leave an axis out)
 * The ball's sensors toggle a line for every step, so a step is a change of level between two reads: read() has to be called often enough that the ball cannot turn
 * a whole step twice in between (every 10 ms or sooner; the shell's input loop reads at 100 Hz or better). Which pin is which direction differs between boards
 * and between trackball models, so the mapping is data. No libc.
 */
#include "purr_driver.h"
#include "purr_driver_bus.h"

static const purr_driver_bus_table_t *s_bus;

typedef struct {
    int in_use;
    int pin[5];                       /* up, down, left, right, click */
    uint8_t last[4];
    int primed;
} tb_t;

static tb_t s_dev[2];

static int tb_read(void *h, uint8_t *data, size_t len)
{
    tb_t *d = h;
    if (d == NULL || !d->in_use || data == NULL || len < 4) return -1;
    int steps[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) {
        if (d->pin[i] < 0) continue;
        uint8_t level = s_bus->gpio_get(d->pin[i]) ? 1 : 0;
        if (d->primed && level != d->last[i]) steps[i] = 1;
        d->last[i] = level;
    }
    d->primed = 1;
    int dx = steps[3] - steps[2], dy = steps[1] - steps[0];
    data[0] = (uint8_t)(int8_t)dx;
    data[1] = (uint8_t)(int8_t)dy;
    data[2] = d->pin[4] >= 0 && s_bus->gpio_get(d->pin[4]) == 0;      /* the click is active low */
    data[3] = 0;
    return 4;
}

static int tb_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    (void)board;
    const purr_kv_cfg_t *c = (const purr_kv_cfg_t *)dev->cfg;
    if (c == NULL) return -1;
    tb_t *d = NULL;
    for (int i = 0; i < 2; i++) {
        if (!s_dev[i].in_use) {
            d = &s_dev[i];
            break;
        }
    }
    if (d == NULL) return -1;
    static const char *names[5] = {"up", "down", "left", "right", "click"};
    for (unsigned i = 0; i < sizeof(*d); i++) ((char *)d)[i] = 0;
    int any = 0;
    for (int i = 0; i < 5; i++) {
        d->pin[i] = purr_kv(c, names[i], PURR_PIN_NONE);
        if (d->pin[i] >= 0) {
            s_bus->gpio_set_input_pullup(d->pin[i]);
            any = 1;
        }
    }
    if (!any) return -1;
    d->in_use = 1;
    *out_handle = d;
    return 0;
}

static void tb_remove(void *handle)
{
    tb_t *d = handle;
    if (d != NULL) d->in_use = 0;
}

static const purr_driver_t s_driver = {
    .name = "trackball-gpio", .compatible = "trackball-gpio",
    .probe = tb_probe, .remove = tb_remove, .read = tb_read,
    .bus_abi = PURR_DRIVER_BUS_ABI_VERSION,
};

__attribute__((used))
const purr_driver_t *trackball_gpio_entry(const purr_driver_bus_table_t *bus)
{
    s_bus = bus;
    return &s_driver;
}
