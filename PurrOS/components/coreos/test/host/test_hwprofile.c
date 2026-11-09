#include <stdio.h>
#include <string.h>

#include "purr_hwprofile.h"
#include "testkit.h"

static purr_hwp_dev_t dev(const char *compat, int sda, int scl, int addr)
{
    purr_hwp_dev_t d = {0};
    strcpy(d.compatible, compat);
    strcpy(d.label, "A sensor");
    d.sda = sda;
    d.scl = scl;
    d.addr = (uint8_t)addr;
    return d;
}

static void test_add_and_names(void)
{
    purr_pinmask_t r = purr_hw_reserved_s3();
    purr_hwp_t p = {0};
    purr_hwp_dev_t a = dev("bme280", 21, 22 - 0, 0x76);
    a.scl = 17;
    CHECK_EQ(purr_hwp_add(&p, &a, r), 0);
    CHECK(strcmp(p.dev[0].name, "bme280_0") == 0);
    CHECK_EQ(purr_hwp_add(&p, &a, r), 1);                         /* the same device again */
    purr_hwp_dev_t b = dev("bme280", 21, 17, 0x77);
    CHECK_EQ(purr_hwp_add(&p, &b, r), 0);
    CHECK(strcmp(p.dev[1].name, "bme280_1") == 0);                /* a second one of the same chip gets the next name */
    CHECK_EQ(p.count, 2);
}

static void test_validation_refuses_bad_devices(void)
{
    purr_pinmask_t r = purr_hw_reserved_s3();
    purr_hwp_t p = {0};
    purr_hwp_dev_t d = dev("x", 21, 17, 0x76);
    d.addr = 0x02;
    CHECK_EQ(purr_hwp_add(&p, &d, r), -1);                        /* below the I2C range */
    d = dev("x", 21, 17, 0x76);
    d.sda = 26;                                                   /* a flash line */
    CHECK_EQ(purr_hwp_add(&p, &d, r), -1);
    d = dev("x", 19, 17, 0x76);                                   /* USB */
    CHECK_EQ(purr_hwp_add(&p, &d, r), -1);
    d = dev("x", 5, 5, 0x76);
    CHECK_EQ(purr_hwp_add(&p, &d, r), -1);                        /* same pin twice */
    d = dev("has space", 21, 17, 0x76);
    CHECK_EQ(purr_hwp_add(&p, &d, r), -1);
    d = dev("", 21, 17, 0x76);
    CHECK_EQ(purr_hwp_add(&p, &d, r), -1);
    CHECK_EQ(p.count, 0);
}

static void test_round_trip_and_hostile_file(void)
{
    purr_pinmask_t r = purr_hw_reserved_s3();
    purr_hwp_t p = {0};
    purr_hwp_dev_t a = dev("bme280", 21, 17, 0x76);
    purr_hwp_add(&p, &a, r);
    purr_hwp_dev_t b = dev("gt911", 5, 6, 0x5D);
    purr_hwp_add(&p, &b, r);
    char buf[1024];
    int n = purr_hwp_format(&p, buf, sizeof(buf));
    CHECK(n > 40);
    CHECK_EQ(purr_hwp_format(&p, buf, 30), -1);                   /* too small */
    purr_hwp_t q;
    CHECK_EQ(purr_hwp_parse(&q, buf, (size_t)n, r), 2);
    CHECK(strcmp(q.dev[1].compatible, "gt911") == 0 && q.dev[1].sda == 5 && q.dev[1].scl == 6 && q.dev[1].addr == 0x5D);

    /* a hand-edited or damaged file: reserved pins, a bad address, a missing field, junk lines. (A device on another bus is no longer "unknown": it is read as a
     * device with named values, so the "spi" entry below is kept, with sda, scl and addr as its values.) */
    const char *bad =
        "# comment\n\ndevice=ok0\ncompatible=ok\nbus=i2c\nsda=5\nscl=6\naddr=0x20\n\n"
        "device=flash\ncompatible=x\nbus=i2c\nsda=27\nscl=6\naddr=0x20\n\n"
        "device=badaddr\ncompatible=x\nbus=i2c\nsda=5\nscl=6\naddr=0x01\n\n"
        "device=nobus\ncompatible=x\nsda=5\nscl=6\naddr=0x20\n\n"
        "device=spi\ncompatible=x\nbus=spi\nsda=5\nscl=6\naddr=0x20\n\n"
        "device=nopins\ncompatible=x\nbus=i2c\naddr=0x20\n\n"
        "garbage line\nsda=7\n"
        "device=last\ncompatible=y\nbus=i2c\nsda=8\nscl=9\naddr=33\n";                /* no blank line, decimal address */
    CHECK_EQ(purr_hwp_parse(&q, bad, strlen(bad), r), 3);
    CHECK(strcmp(q.dev[0].name, "ok0") == 0 && strcmp(q.dev[1].name, "spi") == 0 && strcmp(q.dev[2].name, "last") == 0 && q.dev[2].addr == 33);
    CHECK_EQ((int)purr_hwp_kv_get(&q.dev[1], "addr", 0), 0x20);
    CHECK_EQ(q.dropped, 4);
    CHECK_EQ(purr_hwp_parse(&q, "", 0, r), 0);
}


static void test_named_value_devices(void)
{
    const char *text =
        "device=gps0\ncompatible=gnss-nmea\nlabel=GPS receiver\nbus=uart\nport=1\ntx=43\nrx=44\nbaud=38400\n\n"
        "device=lora0\ncompatible=lora-sx126x\nbus=spi\ncs=9\nbusy=13\ndio1=45\nrst=-1\nfreq=868000000\n\n"
        "device=bad1\ncompatible=gnss-nmea\nbus=uart\ntx=26\nrx=44\n\n"             /* 26 is flash */
        "device=bad2\ncompatible=gnss-nmea\nbus=uart\ntx=9999\n\n"                  /* not a pin */
        "device=bad3\ncompatible=gnss-nmea\nbus=uart\n\n"                            /* nothing configured */
        "device=bme\ncompatible=bme280\nbus=i2c\nsda=21\nscl=17\naddr=0x76\n";    /* an old-style device still reads */
    purr_hwp_t p;
    int n = purr_hwp_parse(&p, text, strlen(text), purr_hw_reserved_s3());
    CHECK_EQ(n, 3);
    CHECK_EQ(p.dropped, 3);
    CHECK(strcmp(p.dev[0].bus, "uart") == 0);
    CHECK_EQ((int)purr_hwp_kv_get(&p.dev[0], "tx", -9), 43);
    CHECK_EQ((int)purr_hwp_kv_get(&p.dev[0], "baud", 0), 38400);
    CHECK_EQ((int)purr_hwp_kv_get(&p.dev[1], "dio1", -9), 45);          /* a strapping pin is fine here: it is named, not scanned */
    CHECK_EQ((int)purr_hwp_kv_get(&p.dev[1], "rst", 0), -1);            /* -1: not connected */
    CHECK_EQ((int)purr_hwp_kv_get(&p.dev[1], "freq", 0), 868000000);
    CHECK_EQ((int)purr_hwp_kv_get(&p.dev[0], "nope", 77), 77);
    CHECK_EQ(p.dev[2].addr, 0x76);

    char out[2048];
    int len = purr_hwp_format(&p, out, sizeof(out));
    CHECK(len > 0);
    purr_hwp_t q;
    CHECK_EQ(purr_hwp_parse(&q, out, (size_t)len, purr_hw_reserved_s3()), 3);       /* what is written reads back the same */
    CHECK_EQ(q.dropped, 0);
    CHECK_EQ((int)purr_hwp_kv_get(&q.dev[1], "freq", 0), 868000000);
    CHECK_EQ((int)purr_hwp_kv_get(&q.dev[0], "rx", 0), 44);

    CHECK(purr_hwp_key_is_pin("tx") && purr_hwp_key_is_pin("dio1") && !purr_hwp_key_is_pin("baud") && !purr_hwp_key_is_pin("freq"));

    purr_hwp_dev_t d = {0};
    strcpy(d.compatible, "x");
    strcpy(d.name, "x0");
    strcpy(d.bus, "uart");
    for (int i = 0; i < PURR_HWP_KV; i++) {
        char k[8];
        snprintf(k, sizeof(k), "k%d", i);
        CHECK_EQ(purr_hwp_kv_set(&d, k, i), 0);
    }
    CHECK_EQ(purr_hwp_kv_set(&d, "extra", 1), -1);                         /* the list is full */
    CHECK_EQ(purr_hwp_kv_set(&d, "Bad Key", 1), -1);
    CHECK_EQ(purr_hwp_kv_set(&d, "k3", 33), 0);                            /* an existing key is replaced, not added */
    CHECK_EQ((int)purr_hwp_kv_get(&d, "k3", 0), 33);
}

int main(void)
{
    test_add_and_names();
    test_validation_refuses_bad_devices();
    test_round_trip_and_hostile_file();
    test_named_value_devices();
    TK_DONE("test_hwprofile");
}
