/* The W5500 module (Modules/drivers/w5500_module.c) against a model of the chip: its register file, socket 0 in MACRAW mode with the 16 KB TX and RX rings, the
 * SEND and RECV commands, and the 2-byte length header in front of every received frame. The module runs here exactly as it is built for the device. */
#include <stdio.h>
#include <string.h>

#include "fakebus.h"
#include "testkit.h"
#include "../../../../../Modules/drivers/w5500_module.c"

/* ---------------------------------------------------------------- the chip */

#define RING 0x4000

static struct {
    uint8_t com[0x40];
    uint8_t sock[0x30];
    uint8_t tx[RING], rx[RING];
    uint16_t tx_rd, tx_wr, rx_rd, rx_wr;
    int version_delay;                 /* version reads this many times as 0 after a reset */
    int reset_stuck;                   /* the MR reset bit never clears */
    int dead;                          /* the chip does not answer at all: every read is 0xFF */
    int cmd_stuck;                     /* the socket command register never clears */
    int send_never_done;               /* SEND never raises its done bit */
    int spi_calls, max_len;
    uint8_t wire[64][1600];            /* frames the chip sent */
    int wire_len[64];
    int wire_n;
} chip;

static void chip_reset(void)
{
    int keep_delay = chip.version_delay;
    memset(chip.com, 0, sizeof(chip.com));
    memset(chip.sock, 0, sizeof(chip.sock));
    chip.tx_rd = chip.tx_wr = chip.rx_rd = chip.rx_wr = 0;
    chip.com[0x39] = chip.version_delay > 0 ? 0 : 4;
    chip.version_delay = keep_delay;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static void sync_regs(void)
{
    put16(chip.sock + 0x20, (uint16_t)(RING - (uint16_t)(chip.tx_wr - chip.tx_rd)));       /* TX_FSR */
    put16(chip.sock + 0x22, chip.tx_rd);
    put16(chip.sock + 0x24, chip.tx_wr);
    put16(chip.sock + 0x26, (uint16_t)(chip.rx_wr - chip.rx_rd));                           /* RX_RSR */
    put16(chip.sock + 0x28, chip.rx_rd);
    put16(chip.sock + 0x2A, chip.rx_wr);
}

static void chip_command(uint8_t cmd)
{
    if (chip.cmd_stuck) {
        chip.sock[0x01] = cmd;
        return;
    }
    if (cmd == 0x01) {
        chip.sock[0x03] = 0x42;                              /* Sn_SR: MACRAW */
    } else if (cmd == 0x20) {                                /* SEND: tx_rd..tx_wr goes out */
        uint16_t n = (uint16_t)(chip.tx_wr - chip.tx_rd);
        if (chip.wire_n < 64 && n <= 1600) {
            for (uint16_t i = 0; i < n; i++) chip.wire[chip.wire_n][i] = chip.tx[(uint16_t)(chip.tx_rd + i) & (RING - 1)];
            chip.wire_len[chip.wire_n++] = n;
        }
        chip.tx_rd = chip.tx_wr;
        if (!chip.send_never_done) chip.sock[0x02] |= 0x10;
    }
    chip.sock[0x01] = 0;
    sync_regs();
}

/* Frames come in with a 2-byte header holding their length plus two. */
static void chip_inject(const uint8_t *f, int len)
{
    uint16_t total = (uint16_t)(len + 2);
    uint8_t h[2];
    put16(h, total);
    for (int i = 0; i < 2; i++) chip.rx[(uint16_t)(chip.rx_wr + i) & (RING - 1)] = h[i];
    for (int i = 0; i < len; i++) chip.rx[(uint16_t)(chip.rx_wr + 2 + i) & (RING - 1)] = f[i];
    chip.rx_wr = (uint16_t)(chip.rx_wr + total);
    sync_regs();
}

static void chip_inject_raw_header(uint16_t bogus_total, int payload_bytes)
{
    uint8_t h[2];
    put16(h, bogus_total);
    for (int i = 0; i < 2; i++) chip.rx[(uint16_t)(chip.rx_wr + i) & (RING - 1)] = h[i];
    for (int i = 0; i < payload_bytes; i++) chip.rx[(uint16_t)(chip.rx_wr + 2 + i) & (RING - 1)] = 0xEE;
    chip.rx_wr = (uint16_t)(chip.rx_wr + 2 + payload_bytes);
    sync_regs();
}

static int unlocked_transfers;                                /* transfers made without the SPI lock held */

static void chip_spi(const uint8_t *tx, uint8_t *rx, size_t n)
{
    chip.spi_calls++;
    if (fb_lock_depth <= 0) unlocked_transfers++;
    if ((int)n > chip.max_len) chip.max_len = (int)n;
    if (chip.dead) {
        memset(rx, 0xFF, n);
        return;
    }
    uint16_t addr = (uint16_t)((tx[0] << 8) | tx[1]);
    int bsb = tx[2] >> 3, write = (tx[2] & 4) != 0;
    size_t len = n - 3;
    for (size_t i = 0; i < len; i++) {
        uint16_t a = (uint16_t)(addr + i);
        uint8_t *byte = NULL;
        if (bsb == 0x00 && a < sizeof(chip.com)) byte = &chip.com[a];
        else if (bsb == 0x01 && a < sizeof(chip.sock)) byte = &chip.sock[a];
        else if (bsb == 0x02) byte = &chip.tx[a & (RING - 1)];
        else if (bsb == 0x03) byte = &chip.rx[a & (RING - 1)];
        if (byte == NULL) {
            continue;
        }
        if (write) {
            *byte = tx[3 + i];
        } else {
            if (bsb == 0x00 && a == 0x39 && chip.version_delay > 0) {
                chip.version_delay--;
                rx[3 + i] = 0;
                continue;
            }
            rx[3 + i] = *byte;
        }
    }
    if (!write) {
        return;
    }
    /* registers with side effects */
    if (bsb == 0x00 && addr == 0x00 && (tx[3] & 0x80) && !chip.reset_stuck) {
        int keep = chip.version_delay;
        chip_reset();
        chip.version_delay = keep;
        chip.com[0x39] = 4;
    } else if (bsb == 0x01 && addr == 0x01 && len >= 1) {
        chip_command(tx[3]);
    } else if (bsb == 0x01 && addr == 0x02 && len >= 1) {
        chip.sock[0x02] &= (uint8_t)~tx[3];                  /* write 1 to clear */
    } else if (bsb == 0x01 && addr == 0x24 && len >= 2) {
        chip.tx_wr = get16(tx + 3);
        sync_regs();
    } else if (bsb == 0x01 && addr == 0x28 && len >= 2) {
        chip.rx_rd = get16(tx + 3);
        sync_regs();
    }
}

/* ---------------------------------------------------------------- setup */

static const purr_board_t s_board = {.spi = {.host = 1}};
static purr_driver_bus_table_t s_table;
static const purr_driver_t *s_drv;
static void *s_h;
static const uint8_t MAC[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};

static const purr_eth_ops_t *open_eth(int own_bus)
{
    memset(&chip, 0, sizeof(chip));
    chip_reset();
    sync_regs();
    s_table = fb_table();
    s_drv = w5500_entry(&s_table);
    fb_reset();
    fb_spi_hook = chip_spi;
    fb_bus_calls = 0;
    fb_bus_fail = 0;
    fb_lock_depth = 0;
    memset(&fb_cfg, 0, sizeof(fb_cfg));
    fb_cfg_set("cs", 9);
    if (own_bus) {
        fb_cfg_set("own_bus", 1);
        fb_cfg_set("host", 2);
        fb_cfg_set("sclk", 36);
        fb_cfg_set("mosi", 35);
        fb_cfg_set("miso", 37);
    }
    purr_device_t dev = fb_dev("eth0", "w5500");
    s_dev.in_use = 0;                                         /* a fresh module each time */
    if (s_drv->probe(&dev, &s_board, &s_h) != 0) return NULL;
    return s_drv->ops;
}

static uint8_t frame_buf[1600];

static void make_frame(uint8_t *f, int len, uint8_t seed)
{
    for (int i = 0; i < len; i++) f[i] = (uint8_t)(seed + i * 7);
}

/* ---------------------------------------------------------------- tests */

static void test_probe(void)
{
    const purr_eth_ops_t *ops = open_eth(1);
    CHECK(ops != NULL);
    CHECK_EQ((int)purr_driver_class_id(s_drv), PURR_DRV_CLASS_ETHERNET);
    CHECK_EQ(s_drv->bus_abi, PURR_DRIVER_BUS_ABI_VERSION);
    CHECK_EQ(fb_bus_calls, 1);                                /* the module started the device's own bus */
    CHECK_EQ(fb_bus_host, 2);
    CHECK_EQ(fb_bus_sclk, 36);
    CHECK_EQ(fb_bus_mosi, 35);
    CHECK_EQ(fb_bus_miso, 37);
    CHECK(fb_bus_max >= 515);
    CHECK_EQ(fb_spi_host, 2);                                 /* and added the chip to it */
    CHECK_EQ(fb_spi_cs, 9);

    ops = open_eth(0);                                        /* shared bus: the board's own host, no new bus */
    CHECK(ops != NULL);
    CHECK_EQ(fb_bus_calls, 0);
    CHECK_EQ(fb_spi_host, 1);

    /* things that must be refused */
    purr_device_t dev = fb_dev("eth0", "w5500");
    void *h;
    s_dev.in_use = 1;
    CHECK_EQ(s_drv->probe(&dev, &s_board, &h), -1);           /* only one controller */
    s_dev.in_use = 0;
    memset(&fb_cfg, 0, sizeof(fb_cfg));
    CHECK_EQ(s_drv->probe(&dev, &s_board, &h), -1);           /* no chip select */
    memset(&fb_cfg, 0, sizeof(fb_cfg));
    fb_cfg_set("cs", 9);
    fb_cfg_set("own_bus", 1);
    CHECK_EQ(s_drv->probe(&dev, &s_board, &h), -1);           /* an own bus with no host */
    fb_cfg_set("host", 2);
    fb_bus_fail = 1;
    CHECK_EQ(s_drv->probe(&dev, &s_board, &h), -1);           /* the kernel would not start the bus */
    fb_bus_fail = 0;
    s_table.spi_open_bus = NULL;
    CHECK_EQ(s_drv->probe(&dev, &s_board, &h), -1);           /* a kernel older than bus ABI 8 */
}

static void test_start_and_link(void)
{
    const purr_eth_ops_t *ops = open_eth(1);
    CHECK_EQ(ops->link(s_h), 0);                              /* PHYCFGR still 0 */
    CHECK_EQ(ops->send_frame(s_h, frame_buf, 60), -1);        /* not started */
    CHECK_EQ(ops->start(s_h, MAC), 0);
    CHECK(memcmp(chip.com + 0x09, MAC, 6) == 0);              /* the station address */
    CHECK_EQ(chip.sock[0x1E], 16);                            /* all 16 KB of both rings to socket 0 */
    CHECK_EQ(chip.sock[0x1F], 16);
    CHECK_EQ(chip.sock[0x00], 0x84);                          /* MACRAW with the MAC filter */
    CHECK_EQ(chip.sock[0x03], 0x42);                          /* opened */
    CHECK_EQ(chip.com[0x00], 0x10);                           /* ping block */
    CHECK_EQ(fb_lock_depth, 0);
    chip.com[0x2E] = 0x01 | 0x02 | 0x04;
    CHECK_EQ(ops->link(s_h), PURR_ETH_LINK_UP | PURR_ETH_SPEED_100 | PURR_ETH_FULL);
    chip.com[0x2E] = 0x01;
    CHECK_EQ(ops->link(s_h), PURR_ETH_LINK_UP);               /* 10 Mbit, half duplex */
    chip.com[0x2E] = 0x00;
    CHECK_EQ(ops->link(s_h), 0);
    CHECK_EQ(fb_lock_depth, 0);
    CHECK_EQ(ops->stop(s_h), 0);
    CHECK_EQ(ops->send_frame(s_h, frame_buf, 60), -1);        /* stopped */

    /* a chip that is slow to report its version still works; one that never does is refused */
    ops = open_eth(1);
    chip.version_delay = 3;
    CHECK_EQ(ops->start(s_h, MAC), 0);
    ops = open_eth(1);
    chip.dead = 1;
    CHECK_EQ(ops->start(s_h, MAC), -1);
    CHECK_EQ(fb_lock_depth, 0);                               /* even a failed start lets go of the bus */
    CHECK_EQ(ops->link(s_h), -1);                             /* nothing driving the bus */
    ops = open_eth(1);
    chip.reset_stuck = 1;
    CHECK_EQ(ops->start(s_h, MAC), -1);
    ops = open_eth(1);
    chip.cmd_stuck = 1;
    CHECK_EQ(ops->start(s_h, MAC), -1);                       /* OPEN never taken */
}

static void test_send(void)
{
    const purr_eth_ops_t *ops = open_eth(1);
    CHECK_EQ(ops->start(s_h, MAC), 0);
    chip.max_len = 0;
    make_frame(frame_buf, 60, 1);
    CHECK_EQ(ops->send_frame(s_h, frame_buf, 60), 0);
    CHECK_EQ(chip.wire_n, 1);
    CHECK_EQ(chip.wire_len[0], 60);
    CHECK(memcmp(chip.wire[0], frame_buf, 60) == 0);

    make_frame(frame_buf, PURR_ETH_MAX_FRAME, 9);             /* the longest frame: sent in chunks */
    CHECK_EQ(ops->send_frame(s_h, frame_buf, PURR_ETH_MAX_FRAME), 0);
    CHECK_EQ(chip.wire_len[1], PURR_ETH_MAX_FRAME);
    CHECK(memcmp(chip.wire[1], frame_buf, PURR_ETH_MAX_FRAME) == 0);
    CHECK(chip.max_len <= 515);                               /* no transfer longer than a chunk */

    CHECK_EQ(ops->send_frame(s_h, frame_buf, 13), -1);        /* shorter than an Ethernet header */
    CHECK_EQ(ops->send_frame(s_h, frame_buf, PURR_ETH_MAX_FRAME + 1), -1);
    CHECK_EQ(ops->send_frame(s_h, NULL, 60), -1);
    CHECK_EQ(fb_lock_depth, 0);

    /* the 16-bit write pointer wraps many times over the 16 KB ring; every frame must still arrive whole */
    int ok = 1;
    for (int i = 0; i < 60; i++) {
        chip.wire_n = 0;
        make_frame(frame_buf, 1000 + i, (uint8_t)i);
        if (ops->send_frame(s_h, frame_buf, 1000 + (size_t)i) != 0 || chip.wire_n != 1 || chip.wire_len[0] != 1000 + i ||
            memcmp(chip.wire[0], frame_buf, 1000 + (size_t)i) != 0) {
            ok = 0;
        }
    }
    CHECK(ok);

    /* a chip that never says "sent" is an error, not a hang */
    chip.send_never_done = 1;
    CHECK_EQ(ops->send_frame(s_h, frame_buf, 60), -1);
    CHECK_EQ(fb_lock_depth, 0);
}

static void test_recv(void)
{
    const purr_eth_ops_t *ops = open_eth(1);
    CHECK_EQ(ops->start(s_h, MAC), 0);
    uint8_t in[1600], out[1600];

    fb_now = 0;
    CHECK_EQ(ops->recv_frame(s_h, out, sizeof(out), 5), 0);   /* nothing there: it waits the timeout and says so */
    CHECK(fb_now >= 4000);

    make_frame(in, 98, 3);
    chip_inject(in, 98);
    CHECK_EQ(ops->recv_frame(s_h, out, sizeof(out), 0), 98);
    CHECK(memcmp(in, out, 98) == 0);
    CHECK_EQ(ops->recv_frame(s_h, out, sizeof(out), 0), 0);   /* and it was taken off the ring */
    CHECK_EQ(chip.rx_rd, chip.rx_wr);

    /* several waiting at once come out in order */
    uint8_t a[100], b[1514], c[60];
    make_frame(a, 100, 11);
    make_frame(b, 1514, 12);
    make_frame(c, 60, 13);
    chip_inject(a, 100);
    chip_inject(b, 1514);
    chip_inject(c, 60);
    CHECK_EQ(ops->recv_frame(s_h, out, sizeof(out), 0), 100);
    CHECK(memcmp(a, out, 100) == 0);
    CHECK_EQ(ops->recv_frame(s_h, out, sizeof(out), 0), 1514);
    CHECK(memcmp(b, out, 1514) == 0);
    CHECK_EQ(ops->recv_frame(s_h, out, sizeof(out), 0), 60);
    CHECK(memcmp(c, out, 60) == 0);

    /* a frame longer than the caller's buffer is cut, and the next one is still found */
    chip_inject(b, 1514);
    chip_inject(c, 60);
    CHECK_EQ(ops->recv_frame(s_h, out, 100, 0), 100);
    CHECK(memcmp(b, out, 100) == 0);
    CHECK_EQ(ops->recv_frame(s_h, out, sizeof(out), 0), 60);
    CHECK(memcmp(c, out, 60) == 0);

    /* the read pointer runs round the ring many times */
    int ok = 1;
    for (int i = 0; i < 80; i++) {
        make_frame(in, 900 + i, (uint8_t)(i * 3));
        chip_inject(in, 900 + i);
        int n = ops->recv_frame(s_h, out, sizeof(out), 0);
        if (n != 900 + i || memcmp(in, out, (size_t)n) != 0) ok = 0;
    }
    CHECK(ok);

    /* garbage in front: a length that cannot be right drops what is waiting, and the next good frame comes through */
    chip_inject_raw_header(5, 40);                            /* shorter than its own header plus an Ethernet header */
    CHECK_EQ(ops->recv_frame(s_h, out, sizeof(out), 0), 0);
    CHECK_EQ(chip.rx_rd, chip.rx_wr);
    chip_inject_raw_header(9000, 40);                         /* longer than any frame */
    CHECK_EQ(ops->recv_frame(s_h, out, sizeof(out), 0), 0);
    CHECK_EQ(chip.rx_rd, chip.rx_wr);
    chip_inject_raw_header(500, 40);                          /* longer than what the chip holds */
    CHECK_EQ(ops->recv_frame(s_h, out, sizeof(out), 0), 0);
    CHECK_EQ(chip.rx_rd, chip.rx_wr);
    make_frame(in, 70, 21);
    chip_inject(in, 70);
    CHECK_EQ(ops->recv_frame(s_h, out, sizeof(out), 0), 70);
    CHECK(memcmp(in, out, 70) == 0);

    CHECK_EQ(ops->recv_frame(s_h, NULL, 10, 0), -1);
    CHECK_EQ(fb_lock_depth, 0);
}

static void test_conversation_is_locked(void)
{
    /* every SPI transfer happens with the bus lock held, so a send and a receive on two tasks never interleave */
    const purr_eth_ops_t *ops = open_eth(1);
    unlocked_transfers = 0;
    CHECK_EQ(ops->start(s_h, MAC), 0);
    chip.com[0x2E] = 0x07;
    CHECK(ops->link(s_h) > 0);
    make_frame(frame_buf, 200, 5);
    CHECK_EQ(ops->send_frame(s_h, frame_buf, 200), 0);
    uint8_t in[100], out[1600];
    make_frame(in, 100, 2);
    chip_inject(in, 100);
    CHECK_EQ(ops->recv_frame(s_h, out, sizeof(out), 0), 100);
    CHECK_EQ(ops->recv_frame(s_h, out, sizeof(out), 0), 0);
    CHECK(chip.spi_calls > 10);
    CHECK_EQ(unlocked_transfers, 0);
    CHECK_EQ(fb_lock_depth, 0);
}

int main(void)
{
    test_probe();
    test_start_and_link();
    test_send();
    test_recv();
    test_conversation_is_locked();
    TK_DONE("test_drv_eth");
}
