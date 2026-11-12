/* The Playtime gamepad driver (Modules/drivers/gamepad_mcp23017_module.c) against a model of the MCP23017 and the module's wiring: a 2x4 matrix with a diode in
 * every switch (column -> row), pull-ups on the columns and FUNC, a function button on GPB6, and an active-low LED for every button. The driver runs here exactly
 * as it is built for the device. */
#include <stdio.h>
#include <string.h>

#include "fakebus.h"
#include "testkit.h"
#include "../../../../../Modules/drivers/gamepad_mcp23017_module.c"

/* ---------------------------------------------------------------- the chip and the module */

static struct {
    int present;                 /* the module answers at all */
    int down[9];                 /* 0..3 UP DOWN LEFT RIGHT, 4..7 A B X Y, 8 FUNC */
    uint8_t iodira, iodirb, gppua, gppub, olata, olatb;
    int writes, reads;
} pad;

static void pad_reset(void)
{
    memset(&pad, 0, sizeof(pad));
    pad.present = 1;
    pad.iodira = pad.iodirb = 0xFF;      /* power-on: every pin an input */
    pad.olata = pad.olatb = 0x00;
}

/* GPIOB as the wiring gives it: a pin that is an output reads its latch; a column with its pull-up on reads 1 unless a pressed switch connects it to a row that is
 * an output driving low (the diode only conducts column -> row). */
static uint8_t gpiob(void)
{
    uint8_t v = 0;
    for (int bit = 0; bit < 8; bit++) {
        int out = !(pad.iodirb & (1u << bit));
        int level = 1;
        if (out) {
            level = (pad.olatb >> bit) & 1;
        } else if (bit >= 2 && bit <= 5 && (pad.gppub & (1u << bit))) {
            int c = bit - 2;
            for (int r = 0; r < 2; r++) {
                int row_out_low = !(pad.iodirb & (1u << r)) && !((pad.olatb >> r) & 1);
                if (row_out_low && pad.down[r * 4 + c]) {
                    level = 0;
                }
            }
        } else if (bit == 6 && (pad.gppub & 0x40)) {
            level = pad.down[8] ? 0 : 1;
        } else if (bit >= 2 && bit <= 6) {
            level = 0;                    /* no pull-up: a floating input; the driver must not rely on this */
        }
        v |= (uint8_t)(level << bit);
    }
    return v;
}

static int chip(const uint8_t *tx, size_t n, uint8_t *rx, size_t rn)
{
    if (!pad.present) {
        return -1;
    }
    if (tx != NULL && n >= 2 && rn == 0) {
        pad.writes++;
        for (size_t i = 1; i < n; i++) {
            uint8_t reg = (uint8_t)(tx[0] + (i - 1)), val = tx[i];
            switch (reg) {
            case 0x00: pad.iodira = val; break;
            case 0x01: pad.iodirb = val; break;
            case 0x0C: pad.gppua = val; break;
            case 0x0D: pad.gppub = val; break;
            case 0x14: pad.olata = val; break;
            case 0x15: pad.olatb = val; break;
            default: break;
            }
        }
        return 0;
    }
    if (tx != NULL && n == 1 && rn == 1) {
        pad.reads++;
        rx[0] = tx[0] == 0x13 ? gpiob() : 0;
        return 0;
    }
    return -1;
}

static int led_on_of(int n)           /* is the LED of button n lit (the pin is driven low)? */
{
    if (n == 0) {
        return !(pad.olatb & 0x80);
    }
    return !((pad.olata >> (n - 1)) & 1);
}

/* ---------------------------------------------------------------- the harness */

static purr_driver_bus_table_t tbl;
static const purr_driver_t *drv;
static void *handle;

static void start(int present)
{
    pad_reset();
    pad.present = present;
    fb_i2c_hook = chip;
    fb_now = 1000000;
    memset(&fb_cfg, 0, sizeof(fb_cfg));
    fb_cfg_set("port", 0);
    fb_cfg_set("sda", 17);
    fb_cfg_set("scl", 18);
    fb_cfg_set("addr", 0x20);
    fb_cfg_set("hz", 400000);
    tbl = fb_table();
    drv = gamepad_mcp23017_entry(&tbl);
    purr_device_t dev = fb_dev("keyboard", "gamepad-mcp23017");
    handle = NULL;
    CHECK_EQ(drv->probe(&dev, NULL, &handle), 0);
}

static int poll_key(void)
{
    uint8_t k = 0;
    int n = drv->read(handle, &k, 1);
    return n == 1 ? k : 0;
}

/* One tick later (past the scan interval), then read. */
static int tick_key(void)
{
    fb_now += 10000;
    return poll_key();
}

static void press(int n) { pad.down[n] = 1; }
static void release(int n) { pad.down[n] = 0; }

static void test_probe(void)
{
    start(1);
    CHECK_EQ(fb_i2c_addr, 0x20);
    CHECK_EQ(fb_i2c_port, 0);
    CHECK_EQ(fb_i2c_sda, 17);
    CHECK_EQ(fb_i2c_scl, 18);
    CHECK_EQ(fb_i2c_hz, 400000);
    CHECK(strcmp(drv->compatible, "gamepad-mcp23017") == 0);
    CHECK_EQ(drv->bus_abi, PURR_DRIVER_BUS_ABI_VERSION);

    /* The address and pins are data: a board can put the module elsewhere. */
    memset(&fb_cfg, 0, sizeof(fb_cfg));                          /* fb_cfg_set appends, so start a fresh list */
    fb_cfg_set("addr", 0x22);
    fb_cfg_set("sda", 5);
    purr_device_t dev = fb_dev("keyboard", "gamepad-mcp23017");
    void *h2 = NULL;
    CHECK_EQ(drv->probe(&dev, NULL, &h2), 0);
    CHECK_EQ(fb_i2c_addr, 0x22);
    CHECK_EQ(fb_i2c_sda, 5);
}

static void test_setup_and_idle(void)
{
    start(1);
    CHECK_EQ(tick_key(), 0);                                     /* nothing pressed */
    CHECK_EQ(pad.iodira, 0x00);                                  /* the LED pins are outputs */
    CHECK_EQ(pad.gppub, 0x7C);                                   /* pull-ups on GPB2..GPB6 (columns and FUNC) */
    CHECK_EQ(pad.gppua, 0x00);
    for (int n = 0; n < 9; n++) {
        CHECK(!led_on_of(n));                                    /* every LED off */
    }
    CHECK_EQ(pad.olata, 0xFF);                                   /* active low: all off is all ones */
    CHECK_EQ(pad.olatb & 0x03, 0x00);                            /* the row latches stay low so a row drives low when it is an output */
    CHECK_EQ(pad.iodirb & 0x80, 0x00);                           /* GPB7 (the UP LED) is an output the whole time */
}

static void test_every_button(void)
{
    static const uint8_t want[9] = {17, 18, 20, 19, '\n', 27, 1, 2, '\t'};
    for (int n = 0; n < 9; n++) {
        start(1);
        CHECK_EQ(tick_key(), 0);
        press(n);
        CHECK_EQ(tick_key(), want[n]);                           /* the matrix position gives the right key */
        CHECK_EQ(tick_key(), 0);                                 /* a held button is one key, not a stream */
        release(n);
        CHECK_EQ(tick_key(), 0);
        press(n);
        CHECK_EQ(tick_key(), want[n]);                           /* a second press is a second key */
    }
}

static void test_leds(void)
{
    start(1);
    tick_key();
    press(0);                                                    /* UP */
    tick_key();
    CHECK(led_on_of(0));
    CHECK(!led_on_of(1));
    press(4);                                                    /* A */
    tick_key();
    CHECK(led_on_of(0));
    CHECK(led_on_of(4));
    CHECK_EQ(pad.olata, (uint8_t)~(1u << 3));                    /* A is GPA3 */
    release(0);
    release(4);
    tick_key();
    for (int n = 0; n < 9; n++) {
        CHECK(!led_on_of(n));
    }
    press(8);                                                    /* FUNC is GPA7 */
    tick_key();
    CHECK(led_on_of(8));
    CHECK_EQ(pad.olata, (uint8_t)~(1u << 7));
}

static void test_two_buttons_at_once(void)
{
    start(1);
    tick_key();
    press(4);                                                    /* A (row 2, column 1) */
    press(0);                                                    /* UP (row 1, column 1) */
    int k1 = tick_key();
    int k2 = poll_key();                                         /* the second key is queued, not lost */
    CHECK((k1 == 17 && k2 == '\n') || (k1 == '\n' && k2 == 17));
    CHECK_EQ(poll_key(), 0);
}

static void test_repeat(void)
{
    start(1);
    tick_key();
    press(2);                                                    /* LEFT */
    CHECK_EQ(tick_key(), 20);
    int keys = 0;
    uint64_t t0 = fb_now;
    while (fb_now - t0 < 380000) {                               /* before the first repeat delay: nothing */
        keys += tick_key() != 0;
    }
    CHECK_EQ(keys, 0);
    uint64_t first = 0;
    while (first == 0 && fb_now - t0 < 600000) {
        if (tick_key() == 20) {
            first = fb_now;
        }
    }
    CHECK(first != 0);
    int more = 0;
    uint64_t t1 = fb_now;
    while (fb_now - t1 < 500000) {
        more += tick_key() == 20;
    }
    CHECK(more >= 3 && more <= 5);                               /* about every 120 ms */
    release(2);
    CHECK_EQ(tick_key(), 0);
    CHECK_EQ(tick_key(), 0);

    start(1);
    tick_key();
    press(4);                                                    /* A does not repeat */
    CHECK_EQ(tick_key(), '\n');
    int a = 0;
    for (int i = 0; i < 100; i++) {
        a += tick_key() != 0;
    }
    CHECK_EQ(a, 0);
}

static void test_absent_then_attached(void)
{
    start(0);                                                    /* no module in the slot */
    for (int i = 0; i < 20; i++) {
        CHECK_EQ(tick_key(), 0);                                 /* not an error, not a key */
    }
    int w = pad.writes;
    CHECK(w == 0);                                               /* a missing chip takes no writes */
    pad.present = 1;
    press(1);                                                    /* DOWN, held as the module is plugged in */
    int got = 0;
    for (int i = 0; i < 100 && !got; i++) {
        fb_now += 100000;                                        /* the driver looks again every half second */
        got = poll_key();
    }
    CHECK_EQ(got, 18);
    CHECK(led_on_of(1));                                         /* it was set up properly */
}

static void test_module_unplugged(void)
{
    start(1);
    tick_key();
    press(3);                                                    /* RIGHT */
    CHECK_EQ(tick_key(), 19);
    pad.present = 0;                                             /* pulled out */
    for (int i = 0; i < 10; i++) {
        CHECK_EQ(tick_key(), 0);
    }
    pad_reset();                                                 /* a module put back is a fresh power-on one */
    int got = 0;
    for (int i = 0; i < 100 && !got; i++) {
        fb_now += 100000;
        got = poll_key();
    }
    CHECK_EQ(got, 0);                                            /* nothing is held, nothing is pressed: no phantom key */
    press(5);                                                    /* B */
    for (int i = 0; i < 100 && !got; i++) {
        fb_now += 100000;
        got = poll_key();
    }
    CHECK_EQ(got, 27);
}

static void test_scan_rate(void)
{
    start(1);
    tick_key();
    int before = pad.reads;
    for (int i = 0; i < 50; i++) {                               /* many reads in the same instant: one scan */
        poll_key();
    }
    CHECK(pad.reads - before <= 2);
}

int main(void)
{
    test_probe();
    test_setup_and_idle();
    test_every_button();
    test_leds();
    test_two_buttons_at_once();
    test_repeat();
    test_absent_then_attached();
    test_module_unplugged();
    test_scan_rate();
    TK_DONE("test_drv_gamepad");
}
