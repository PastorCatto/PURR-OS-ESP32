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

    /* a hand-edited or damaged file: reserved pins, a bad address, a missing field, an unknown bus, junk lines */
    const char *bad =
        "# comment\n\ndevice=ok0\ncompatible=ok\nbus=i2c\nsda=5\nscl=6\naddr=0x20\n\n"
        "device=flash\ncompatible=x\nbus=i2c\nsda=27\nscl=6\naddr=0x20\n\n"
        "device=badaddr\ncompatible=x\nbus=i2c\nsda=5\nscl=6\naddr=0x01\n\n"
        "device=nobus\ncompatible=x\nsda=5\nscl=6\naddr=0x20\n\n"
        "device=spi\ncompatible=x\nbus=spi\nsda=5\nscl=6\naddr=0x20\n\n"
        "device=nopins\ncompatible=x\nbus=i2c\naddr=0x20\n\n"
        "garbage line\nsda=7\n"
        "device=last\ncompatible=y\nbus=i2c\nsda=8\nscl=9\naddr=33\n";                /* no blank line, decimal address */
    CHECK_EQ(purr_hwp_parse(&q, bad, strlen(bad), r), 2);
    CHECK(strcmp(q.dev[0].name, "ok0") == 0 && strcmp(q.dev[1].name, "last") == 0 && q.dev[1].addr == 33);
    CHECK_EQ(q.dropped, 5);
    CHECK_EQ(purr_hwp_parse(&q, "", 0, r), 0);
}

int main(void)
{
    test_add_and_names();
    test_validation_refuses_bad_devices();
    test_round_trip_and_hostile_file();
    TK_DONE("test_hwprofile");
}
