/* The SX126x LoRa driver (Modules/drivers/lora_sx126x_module.c) against a model of the chip that behaves like the real one where it matters for receiving: a packet is heard only if the
 * chip is in receive mode from the start of the packet to its end (a trip through standby in the middle loses it), and its "received" flag stays set until it is read or cleared. A
 * long packet (LongFast takes 0.3 s to over a second on the air) is what the app's receive loop (a call of 60 ms, again and again) has to catch. */
#include <stdio.h>
#include <string.h>

#include "fakebus.h"
#include "testkit.h"
#include "../../../../../Modules/drivers/lora_sx126x_module.c"

static struct {
    int mode;                       /* 2 standby, 5 receive, 6 transmit */
    uint16_t irq;
    uint64_t pkt_start_us, pkt_end_us;
    int pkt_pending, pkt_alive;
    uint8_t pkt[64];
    int pkt_len;
    int rx_entered_before_start;
    uint8_t last_sync[2];
    int standby_calls, set_rx_calls;
} chip;

static void chip_time(void)
{
    if (chip.pkt_pending && chip.mode == 5 && chip.pkt_alive && fb_now >= chip.pkt_end_us) {
        chip.irq |= 0x0002;                                             /* RxDone */
        chip.pkt_pending = 0;
    }
}

static void chip_spi(const uint8_t *tx, uint8_t *rx, size_t n)
{
    chip_time();
    switch (tx[0]) {
    case 0x80:                                                          /* SetStandby */
        chip.standby_calls++;
        if (chip.mode == 5 && chip.pkt_pending && fb_now > chip.pkt_start_us) {
            chip.pkt_alive = 0;                                         /* a reception in progress is abandoned */
        }
        chip.mode = 2;
        break;
    case 0x82:                                                          /* SetRx */
        chip.set_rx_calls++;
        chip.mode = 5;
        if (chip.pkt_pending && fb_now <= chip.pkt_start_us) {
            chip.pkt_alive = 1;                                         /* listening before the packet began: it will be heard to the end if nothing interrupts */
        }
        break;
    case 0x83: chip.mode = 6; chip.irq |= 0x0001; chip.mode = 2; break; /* SetTx: done at once */
    case 0x02: {                                                        /* ClearIrqStatus */
        uint16_t m = (uint16_t)((tx[1] << 8) | tx[2]);
        chip.irq &= (uint16_t)~m;
        break;
    }
    case 0x12:                                                          /* GetIrqStatus */
        if (rx && n >= 4) { rx[2] = (uint8_t)(chip.irq >> 8); rx[3] = (uint8_t)chip.irq; }
        break;
    case 0xC0:                                                          /* GetStatus: the mode in bits 6:4 */
        if (rx && n >= 2) rx[1] = (uint8_t)(chip.mode == 5 ? 0x50 : 0x20);
        break;
    case 0x13:                                                          /* GetRxBufferStatus */
        if (rx && n >= 4) { rx[2] = (uint8_t)chip.pkt_len; rx[3] = 0; }
        break;
    case 0x14:                                                          /* GetPacketStatus */
        if (rx && n >= 5) { rx[2] = 100; rx[3] = 24; rx[4] = 90; }
        break;
    case 0x1E:                                                          /* ReadBuffer */
        if (rx && n >= 3) memcpy(rx + 3, chip.pkt, n - 3 < sizeof(chip.pkt) ? n - 3 : sizeof(chip.pkt));
        break;
    case 0x0D:                                                          /* WriteRegister */
        if (n >= 5 && tx[1] == 0x07 && tx[2] == 0x40) { chip.last_sync[0] = tx[3]; chip.last_sync[1] = tx[4]; }
        break;
    default:
        break;
    }
}

/* a packet that will start at t_us and last dur_us */
static void air(uint64_t t_us, uint64_t dur_us, const char *text)
{
    chip.pkt_pending = 1;
    chip.pkt_alive = chip.mode == 5 && fb_now <= t_us;
    chip.pkt_start_us = t_us;
    chip.pkt_end_us = t_us + dur_us;
    chip.pkt_len = (int)strlen(text);
    memcpy(chip.pkt, text, (size_t)chip.pkt_len);
}

int main(void)
{
    purr_driver_bus_table_t tbl = fb_table();
    lora_sx126x_entry(&tbl);
    fb_spi_hook = chip_spi;
    lora_t *d = &s_dev[0];
    memset(d, 0, sizeof(*d));
    d->in_use = 1;
    d->spi = 7;
    d->busy_pin = 5;
    d->dio1_pin = d->rst_pin = d->pwr_pin = -1;
    purr_lora_cfg_t cfg = {.freq_hz = 906875000u, .bw_hz = 250000, .sf = 11, .cr = 5, .tx_dbm = 10, .crc_on = 1, .preamble = 16, .sync_word = 0x24B4};
    chip.mode = 2;
    CHECK(s_ops.configure(d, &cfg) == 0);
    CHECK(chip.last_sync[0] == 0x24 && chip.last_sync[1] == 0xB4);       /* the mesh's sync word reaches the chip as the register value */

    uint8_t buf[64];
    purr_lora_rxinfo_t info;

    /* nothing on the air: the call waits its time and says so */
    fb_now = 1000000;
    CHECK(s_ops.receive(d, buf, sizeof(buf), 60, &info) == 0);

    /* a packet that takes 0.4 s, heard by an app that calls receive for 60 ms at a time with a few milliseconds between calls */
    air(fb_now + 20000, 400000, "a long packet");
    int got = 0;
    for (int call = 0; call < 20 && !got; call++) {
        int n = s_ops.receive(d, buf, sizeof(buf), 60, &info);
        if (n > 0) {
            CHECK(n == 13 && memcmp(buf, "a long packet", 13) == 0);
            CHECK(info.rssi_dbm == -50 && info.snr_db == 6);
            got = 1;
        }
        fb_now += 4000;                                                  /* the app's own work between two calls */
    }
    CHECK(got);

    /* a packet that ends while the app is busy elsewhere (between two calls): it is still there at the next call */
    air(fb_now + 10000, 100000, "between calls");
    s_ops.receive(d, buf, sizeof(buf), 5, &info);                        /* starts listening */
    fb_now += 500000;                                                    /* the app is away for half a second */
    int n = s_ops.receive(d, buf, sizeof(buf), 60, &info);
    CHECK(n == 13 && memcmp(buf, "between calls", 13) == 0);

    /* sending takes the chip out of receive; the next receive puts it back */
    CHECK(s_ops.send(d, (const uint8_t *)"out", 3, 1000) == 0);
    air(fb_now + 10000, 50000, "after sending");
    n = 0;
    for (int call = 0; call < 10 && n == 0; call++) {
        n = s_ops.receive(d, buf, sizeof(buf), 60, &info);
        fb_now += 3000;
    }
    CHECK(n == 13 && memcmp(buf, "after sending", 13) == 0);

    /* the chip is not put through standby and receive again on every call while it is already listening */
    int before = chip.set_rx_calls;
    for (int call = 0; call < 10; call++) {
        s_ops.receive(d, buf, sizeof(buf), 60, &info);
        fb_now += 3000;
    }
    CHECK(chip.set_rx_calls - before <= 1);

    /* the chip falls out of receive on its own (an error): within half a second the driver notices and listens again, and the next packet is heard */
    s_ops.receive(d, buf, sizeof(buf), 5, &info);
    chip.mode = 2;
    fb_now += 600000;
    air(fb_now + 10000, 50000, "after a lapse");
    n = 0;
    for (int call = 0; call < 10 && n == 0; call++) {
        n = s_ops.receive(d, buf, sizeof(buf), 60, &info);
        fb_now += 3000;
    }
    CHECK(n == 13 && memcmp(buf, "after a lapse", 13) == 0);

    TK_DONE("test_drv_lora");
}
