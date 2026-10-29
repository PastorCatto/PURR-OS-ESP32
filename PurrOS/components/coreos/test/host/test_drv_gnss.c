#include <stdio.h>
#include <string.h>

#include "fakebus.h"
#include "testkit.h"
#include "../../../../../Modules/drivers/gnss_nmea_module.c"

/* A sentence with its correct checksum. */
static void feed_nmea(const char *body)
{
    uint8_t x = 0;
    for (const char *p = body; *p; p++) x ^= (uint8_t)*p;
    char line[128];
    snprintf(line, sizeof(line), "$%s*%02X\r\n", body, x);
    fb_feed(line);
}

static const purr_driver_t *s_drv;

static const purr_gnss_ops_t *open_gnss(void **h)
{
    static purr_driver_bus_table_t t;
    t = fb_table();
    const purr_driver_t *drv = s_drv = gnss_nmea_entry(&t);
    CHECK_EQ((int)purr_driver_class_id(drv), PURR_DRV_CLASS_GNSS);
    fb_reset();
    memset(&fb_cfg, 0, sizeof(fb_cfg));
    fb_cfg_set("port", 1);
    fb_cfg_set("tx", 43);
    fb_cfg_set("rx", 44);
    fb_cfg_set("baud", 38400);
    purr_device_t dev = fb_dev("gps0", "gnss-nmea");
    CHECK_EQ(drv->probe(&dev, NULL, h), 0);
    return drv->ops;
}

static void test_probe_and_config(void)
{
    void *h;
    const purr_gnss_ops_t *ops = open_gnss(&h);
    CHECK(ops != NULL && ops->version == 1);
    CHECK_EQ(fb_uart_port, 1);
    CHECK_EQ(fb_uart_tx_pin, 43);
    CHECK_EQ(fb_uart_rx_pin, 44);
    CHECK_EQ(fb_uart_baud, 38400);
    s_drv->remove(h);
}

static void test_a_good_fix(void)
{
    void *h;
    const purr_gnss_ops_t *ops = open_gnss(&h);
    fb_now = 1000000;
    feed_nmea("GNRMC,123519.00,A,5130.12345,N,00007.65432,W,10.00,84.40,281026,,,A");
    feed_nmea("GNGGA,123519.00,5130.12345,N,00007.65432,W,1,09,0.9,35.4,M,47.0,M,,");
    feed_nmea("GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00*");   /* bad: the helper added a second *; ignored harmlessly */
    feed_nmea("GPGSV,1,1,11,03,03,111,00");
    fb_now = 1250000;
    purr_gnss_fix_t f;
    CHECK_EQ(ops->get_fix(h, &f), 1);
    CHECK_EQ(f.valid, 1);
    CHECK_EQ(f.quality, 1);
    CHECK_EQ(f.sats_used, 9);
    CHECK_EQ(f.sats_view, 11);
    CHECK_EQ(f.hdop_x10, 9);
    CHECK_EQ((int)f.lat_e7, 515020575);                     /* 51 deg + 30.12345 min / 60 = 51.502057... */
    CHECK_EQ((int)f.lon_e7, -1275720);                      /* 0 deg 7.65432 min west = -0.127572 */
    CHECK_EQ((int)f.alt_mm, 35400);
    CHECK_EQ((int)f.speed_mmps, 5144);                      /* 10 knots */
    CHECK_EQ((int)f.course_cdeg, 8440);
    CHECK(f.time_valid);
    CHECK_EQ(f.hour, 12);
    CHECK_EQ(f.minute, 35);
    CHECK_EQ(f.second, 19);
    CHECK_EQ(f.year, 2026);
    CHECK_EQ(f.month, 10);
    CHECK_EQ(f.day, 28);
    CHECK_EQ((int)f.age_ms, 0);                             /* parsed just now, by this very call */
    fb_now += 250000;                                       /* 250 ms later, nothing new arrived */
    CHECK_EQ(ops->get_fix(h, &f), 1);
    CHECK_EQ((int)f.age_ms, 250);
    s_drv->remove(h);
}

static void test_no_fix_and_noise(void)
{
    void *h;
    const purr_gnss_ops_t *ops = open_gnss(&h);
    purr_gnss_fix_t f;
    CHECK_EQ(ops->get_fix(h, &f), 0);                       /* nothing heard yet: "none yet", not an error */
    CHECK_EQ(f.valid, 0);
    feed_nmea("GNGGA,000000.00,,,,,0,00,99.99,,,,,,");      /* a receiver still searching */
    feed_nmea("GNRMC,000000.00,V,,,,,,,,,,N");
    CHECK_EQ(ops->get_fix(h, &f), 0);
    CHECK_EQ(f.sats_used, 0);

    fb_feed("$GNGGA,123519.00,5130.00000,N,00000.00000,E,1,09,0.9,10.0,M,47.0,M,,*00\r\n");   /* a wrong checksum: dropped */
    CHECK_EQ(ops->get_fix(h, &f), 0);
    fb_feed("garbage without a dollar\r\n\x01\x02\xff\xfe\r\n");
    char flood[300];
    memset(flood, 'x', sizeof(flood) - 1);
    flood[sizeof(flood) - 1] = '\0';
    fb_feed(flood);                                         /* a line longer than any sentence */
    fb_feed("\r\n");
    feed_nmea("GNGGA,123519.00,5130.00000,N,00000.00000,E,1,09,0.9,10.0,M,47.0,M,,");        /* and the next good one still works */
    CHECK_EQ(ops->get_fix(h, &f), 1);
    CHECK_EQ((int)f.lat_e7, 515000000);
    s_drv->remove(h);
}

static void test_southern_and_fix_lost(void)
{
    void *h;
    const purr_gnss_ops_t *ops = open_gnss(&h);
    purr_gnss_fix_t f;
    feed_nmea("GNGGA,010203.00,3351.00000,S,15112.00000,E,2,07,1.2,5.0,M,0.0,M,,");
    CHECK_EQ(ops->get_fix(h, &f), 1);
    CHECK((int)f.lat_e7 < 0 && (int)f.lon_e7 > 0);
    CHECK_EQ(f.quality, 2);
    feed_nmea("GNGGA,010204.00,,,,,0,00,99.99,,,,,,");      /* quality 0: the fix is gone */
    CHECK_EQ(ops->get_fix(h, &f), 0);
    s_drv->remove(h);
}

static void test_rate_and_power(void)
{
    void *h;
    const purr_gnss_ops_t *ops = open_gnss(&h);
    CHECK_EQ(ops->set_rate_hz(h, 5), 0);
    CHECK_EQ((int)fb_tx_len, 14);
    CHECK(fb_tx[0] == 0xB5 && fb_tx[1] == 0x62 && fb_tx[2] == 0x06 && fb_tx[3] == 0x08);
    CHECK(fb_tx[6] == 200 && fb_tx[7] == 0);                /* 1000 / 5 = 200 ms */
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 12; i++) { a = (uint8_t)(a + fb_tx[i]); b = (uint8_t)(b + a); }
    CHECK(fb_tx[12] == a && fb_tx[13] == b);                /* the UBX checksum */
    CHECK_EQ(ops->set_rate_hz(h, 0), -1);
    CHECK_EQ(ops->set_rate_hz(h, 50), -1);
    CHECK_EQ(ops->power(h, 1), 0);                          /* no power pin: nothing to switch, still fine */

    static purr_driver_bus_table_t t;
    t = fb_table();
    const purr_driver_t *drv = gnss_nmea_entry(&t);
    drv->remove(h);
    fb_reset();
    memset(&fb_cfg, 0, sizeof(fb_cfg));
    fb_cfg_set("rx", 44);
    fb_cfg_set("pwr", 21);
    purr_device_t dev = fb_dev("gps1", "gnss-nmea");
    void *h2;
    CHECK_EQ(drv->probe(&dev, NULL, &h2), 0);
    CHECK_EQ(fb_gpio[21], 1);                               /* switched on at probe */
    CHECK_EQ(ops->power(h2, 0), 0);
    CHECK_EQ(fb_gpio[21], 0);
    CHECK_EQ(fb_uart_baud, 38400);                          /* the default */
    drv->remove(h2);

    fb_reset();
    memset(&fb_cfg, 0, sizeof(fb_cfg));
    fb_cfg_set("tx", 43);                                   /* no rx: it could never hear anything */
    purr_device_t bad = fb_dev("gps2", "gnss-nmea");
    CHECK_EQ(drv->probe(&bad, NULL, &h2), -1);
    fb_reset();
    memset(&fb_cfg, 0, sizeof(fb_cfg));
    fb_cfg_set("rx", 44);
    fb_uart_fail = 1;
    CHECK_EQ(drv->probe(&bad, NULL, &h2), -1);              /* the UART would not open */
}

int main(void)
{
    test_probe_and_config();
    test_a_good_fix();
    test_no_fix_and_noise();
    test_southern_and_fix_lost();
    test_rate_and_power();
    TK_DONE("test_drv_gnss");
}
