/* A WIZnet W5500 wired Ethernet controller over SPI, used as a plain MAC: socket 0 in MACRAW mode moves whole Ethernet frames and the kernel's own TCP/IP
 * stack does the rest (the chip's hardware TCP/IP is not used). The register sequence follows ESP-IDF's esp_eth W5500 driver, which this replaces for devices
 * that load it as a module (plan4.md, "the W5500 as a module").
 *
 * A driver of class PURR_DRV_CLASS_ETHERNET (purr_driver_class.h), configured by named values (purr_kv_cfg_t):
 *     cs=<pin>          the chip select, a GPIO the hardware SPI peripheral drives (required)
 *     own_bus=0|1       1: the chip is on a bus of its own: host, sclk, mosi and miso are required and the driver starts that bus (bus ABI 8, spi_open_bus)
 *                       0 (default): the chip is on the board's shared bus (board->spi.host) next to the display
 *     host=  sclk=  mosi=  miso=       the own bus (ESP-IDF host number: SPI2 = 1, SPI3 = 2)
 *     hz=20000000       the clock (mode 0)
 *     rst=-1            an optional reset pin, low for "reset"
 * Frames are read and written in chunks, so the longest SPI transfer is CHUNK + 3 bytes. Every conversation with the chip holds the SPI lock (bus ABI 7), so the
 * kernel's receive task and the TCP/IP thread's sends never interleave their register accesses. TCP/IP-side details (the station address, DHCP) belong to the
 * kernel. No libc.
 */
#include "purr_driver.h"
#include "purr_driver_bus.h"
#include "purr_driver_class.h"

static const purr_driver_bus_table_t *s_bus;

/* ---------------------------------------------------------------- the chip (datasheet / ESP-IDF's w5500.h) */

#define BSB_COMMON          0x00
#define BSB_SOCK_REG        0x01                        /* socket 0 */
#define BSB_SOCK_TX         0x02
#define BSB_SOCK_RX         0x03

#define R_MR                0x0000
#define R_SHAR              0x0009
#define R_PHYCFGR           0x002E
#define R_VERSIONR          0x0039
#define R_SIMR              0x0018

#define S_MR                0x0000
#define S_CR                0x0001
#define S_IR                0x0002
#define S_IMR               0x002C
#define S_RXBUF_SIZE        0x001E
#define S_TXBUF_SIZE        0x001F
#define S_TX_FSR            0x0020
#define S_TX_WR             0x0024
#define S_RX_RSR            0x0026
#define S_RX_RD             0x0028

#define MR_RST              0x80
#define MR_PB               0x10                        /* ping block: the chip's own ICMP reply is off, the kernel answers */
#define SMR_MACRAW          0x04
#define SMR_MAC_FILTER      0x80
#define SCR_OPEN            0x01
#define SCR_CLOSE           0x10
#define SCR_SEND            0x20
#define SCR_RECV            0x40
#define SIR_RECV            0x04
#define SIR_SEND            0x10
#define CHIP_VERSION        0x04

#define PHY_LINK            0x01
#define PHY_SPEED_100       0x02
#define PHY_FULL            0x04

#define CHUNK               512                         /* data bytes per SPI transfer */
#define BUF_BYTES           (CHUNK + 3)

typedef struct {
    int in_use;
    uint32_t spi;                                       /* the chip's SPI device handle */
    uint8_t *tx, *rx;                                   /* BUF_BYTES each, DMA-capable (dma_alloc) and kept for good */
    uint8_t started;
    int rst_pin;
} eth_t;

static eth_t s_dev;
static uint8_t s_fallback_tx[BUF_BYTES], s_fallback_rx[BUF_BYTES];     /* when the bus table has no dma_alloc (a test); a device always has one */

/* ---------------------------------------------------------------- one register-level conversation */

/* Reads or writes `len` bytes at (bsb, addr) in variable-length mode: address (2), control (1: BSB, RWB, OM = 0), then the data until the transfer ends. */
static int xfer(eth_t *d, uint8_t bsb, uint16_t addr, int write, const uint8_t *in, uint8_t *out, uint16_t len)
{
    if (len == 0 || len > CHUNK) return -1;
    uint8_t *t = d->tx, *r = d->rx;
    t[0] = (uint8_t)(addr >> 8);
    t[1] = (uint8_t)addr;
    t[2] = (uint8_t)((bsb << 3) | (write ? 0x04 : 0x00));
    for (uint16_t i = 0; i < len; i++) t[3 + i] = write ? in[i] : 0;
    if (s_bus->spi_transfer(d->spi, t, r, (size_t)len + 3) != 0) return -1;
    if (!write) {
        for (uint16_t i = 0; i < len; i++) out[i] = r[3 + i];
    }
    return 0;
}

static int rd(eth_t *d, uint8_t bsb, uint16_t addr, uint8_t *out, uint16_t len) { return xfer(d, bsb, addr, 0, 0, out, len); }
static int wr(eth_t *d, uint8_t bsb, uint16_t addr, const uint8_t *in, uint16_t len) { return xfer(d, bsb, addr, 1, in, 0, len); }

static int rd8(eth_t *d, uint8_t bsb, uint16_t addr, uint8_t *v) { return rd(d, bsb, addr, v, 1); }
static int wr8(eth_t *d, uint8_t bsb, uint16_t addr, uint8_t v) { return wr(d, bsb, addr, &v, 1); }

static int rd16(eth_t *d, uint8_t bsb, uint16_t addr, uint16_t *v)
{
    uint8_t b[2];
    if (rd(d, bsb, addr, b, 2) != 0) return -1;
    *v = (uint16_t)((b[0] << 8) | b[1]);
    return 0;
}

static int wr16(eth_t *d, uint8_t bsb, uint16_t addr, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    return wr(d, bsb, addr, b, 2);
}

/* A 16-bit register the chip may be updating while we read it: read twice until both reads agree (the datasheet's advice, and what ESP-IDF does). */
static int rd16_stable(eth_t *d, uint8_t bsb, uint16_t addr, uint16_t *v)
{
    uint16_t a, b;
    for (int i = 0; i < 8; i++) {
        if (rd16(d, bsb, addr, &a) != 0 || rd16(d, bsb, addr, &b) != 0) return -1;
        if (a == b) {
            *v = a;
            return 0;
        }
    }
    return -1;
}

/* The socket command register clears itself once the chip has taken the command. */
static int command(eth_t *d, uint8_t cmd)
{
    if (wr8(d, BSB_SOCK_REG, S_CR, cmd) != 0) return -1;
    uint64_t t0 = s_bus->now_us();
    for (;;) {
        uint8_t c = 1;
        if (rd8(d, BSB_SOCK_REG, S_CR, &c) != 0) return -1;
        if (c == 0) return 0;
        if ((s_bus->now_us() - t0) / 1000 >= 100) return -1;
        s_bus->delay_ms(1);
    }
}

/* Chunked copies to and from the chip's buffers. The buffer addresses are the chip's 16-bit pointers, which wrap by themselves. */
static int buf_write(eth_t *d, uint8_t bsb, uint16_t addr, const uint8_t *src, uint16_t len)
{
    while (len > 0) {
        uint16_t n = len > CHUNK ? CHUNK : len;
        if (wr(d, bsb, addr, src, n) != 0) return -1;
        addr = (uint16_t)(addr + n);
        src += n;
        len = (uint16_t)(len - n);
    }
    return 0;
}

static int buf_read(eth_t *d, uint8_t bsb, uint16_t addr, uint8_t *dst, uint16_t len)
{
    while (len > 0) {
        uint16_t n = len > CHUNK ? CHUNK : len;
        if (rd(d, bsb, addr, dst, n) != 0) return -1;
        addr = (uint16_t)(addr + n);
        dst += n;
        len = (uint16_t)(len - n);
    }
    return 0;
}

/* ---------------------------------------------------------------- ops */

static int e_start(void *h, const uint8_t mac[6])
{
    eth_t *d = h;
    if (d == NULL || !d->in_use) return -1;
    s_bus->spi_lock();
    int rc = -1;
    uint8_t v = MR_RST;
    if (d->rst_pin >= 0) {                                                  /* the optional hardware reset first */
        s_bus->gpio_write(d->rst_pin, 0);
        s_bus->delay_ms(2);
        s_bus->gpio_write(d->rst_pin, 1);
        s_bus->delay_ms(10);
    }
    if (wr8(d, BSB_COMMON, R_MR, MR_RST) != 0) goto out;                    /* software reset; the bit clears itself */
    {
        uint64_t t0 = s_bus->now_us();
        for (;;) {
            if (rd8(d, BSB_COMMON, R_MR, &v) != 0) goto out;
            if (!(v & MR_RST)) break;
            if ((s_bus->now_us() - t0) / 1000 >= 500) goto out;
            s_bus->delay_ms(10);
        }
    }
    {                                                                       /* the chip has no ID register: its version number says it is a W5500 */
        uint64_t t0 = s_bus->now_us();
        for (;;) {
            if (rd8(d, BSB_COMMON, R_VERSIONR, &v) != 0) goto out;
            if (v == CHIP_VERSION) break;
            if ((s_bus->now_us() - t0) / 1000 >= 500) goto out;             /* some units read 0 for a moment after the reset */
            s_bus->delay_ms(10);
        }
    }
    /* All 16 KB of each buffer to socket 0 (the only socket that can be MACRAW), none to the others. */
    if (wr8(d, BSB_SOCK_REG, S_RXBUF_SIZE, 16) != 0 || wr8(d, BSB_SOCK_REG, S_TXBUF_SIZE, 16) != 0) goto out;
    if (wr8(d, BSB_COMMON, R_MR, MR_PB) != 0) goto out;
    if (wr8(d, BSB_COMMON, R_SIMR, 0) != 0) goto out;                       /* polling: no interrupt line */
    if (wr8(d, BSB_SOCK_REG, S_MR, SMR_MACRAW | SMR_MAC_FILTER) != 0) goto out;
    if (wr8(d, BSB_SOCK_REG, S_IMR, SIR_RECV) != 0) goto out;
    if (wr(d, BSB_COMMON, R_SHAR, mac, 6) != 0) goto out;
    if (command(d, SCR_OPEN) != 0) goto out;
    d->started = 1;
    rc = 0;
out:
    s_bus->spi_unlock();
    return rc;
}

static int e_stop(void *h)
{
    eth_t *d = h;
    if (d == NULL || !d->in_use || !d->started) return -1;
    s_bus->spi_lock();
    d->started = 0;
    int rc = command(d, SCR_CLOSE);
    s_bus->spi_unlock();
    return rc;
}

static int e_link(void *h)
{
    eth_t *d = h;
    if (d == NULL || !d->in_use) return -1;
    uint8_t p;
    s_bus->spi_lock();
    int rc = rd8(d, BSB_COMMON, R_PHYCFGR, &p);
    s_bus->spi_unlock();
    if (rc != 0 || p == 0xFF) return -1;                                    /* all ones: nothing is driving the bus, the chip is not there */
    return ((p & PHY_LINK) ? PURR_ETH_LINK_UP : 0) | ((p & PHY_SPEED_100) ? PURR_ETH_SPEED_100 : 0) | ((p & PHY_FULL) ? PURR_ETH_FULL : 0);
}

static int e_send(void *h, const uint8_t *frame, size_t len)
{
    eth_t *d = h;
    if (d == NULL || !d->in_use || !d->started || frame == NULL || len < 14 || len > PURR_ETH_MAX_FRAME) return -1;
    int rc = -1;
    s_bus->spi_lock();
    uint16_t room, wp;
    if (rd16_stable(d, BSB_SOCK_REG, S_TX_FSR, &room) != 0 || room < len) goto out;
    if (rd16(d, BSB_SOCK_REG, S_TX_WR, &wp) != 0) goto out;
    if (buf_write(d, BSB_SOCK_TX, wp, frame, (uint16_t)len) != 0) goto out;
    if (wr16(d, BSB_SOCK_REG, S_TX_WR, (uint16_t)(wp + len)) != 0) goto out;
    if (command(d, SCR_SEND) != 0) goto out;
    {
        uint64_t t0 = s_bus->now_us();                                      /* the chip says when the frame has left */
        for (;;) {
            uint8_t ir;
            if (rd8(d, BSB_SOCK_REG, S_IR, &ir) != 0) goto out;
            if (ir & SIR_SEND) {
                if (wr8(d, BSB_SOCK_REG, S_IR, SIR_SEND) != 0) goto out;    /* write 1 to clear */
                break;
            }
            if ((s_bus->now_us() - t0) / 1000 >= 20) goto out;
            s_bus->delay_ms(1);
        }
    }
    rc = 0;
out:
    s_bus->spi_unlock();
    return rc;
}

/* Hands back the frame at the read pointer, or 0 when none is waiting. A header that cannot be right (shorter than its own two bytes, longer than any frame, or
 * more than the chip says it holds) means the stream is out of step: everything waiting is dropped so the next frame starts clean. */
static int e_recv_once(eth_t *d, uint8_t *buf, size_t cap)
{
    uint16_t have, rp;
    if (rd16_stable(d, BSB_SOCK_REG, S_RX_RSR, &have) != 0) return -1;
    if (have == 0) return 0;
    if (rd16(d, BSB_SOCK_REG, S_RX_RD, &rp) != 0) return -1;
    uint8_t hdr[2];
    if (buf_read(d, BSB_SOCK_RX, rp, hdr, 2) != 0) return -1;
    uint16_t total = (uint16_t)((hdr[0] << 8) | hdr[1]);                    /* the frame plus these two bytes */
    if (total < 2 + 14 || total > 2 + PURR_ETH_MAX_FRAME + 4 || total > have) {
        if (wr16(d, BSB_SOCK_REG, S_RX_RD, (uint16_t)(rp + have)) != 0) return -1;
        return command(d, SCR_RECV) == 0 ? 0 : -1;
    }
    uint16_t flen = (uint16_t)(total - 2);
    uint16_t take = flen > cap ? (uint16_t)cap : flen;
    if (buf_read(d, BSB_SOCK_RX, (uint16_t)(rp + 2), buf, take) != 0) return -1;
    if (wr16(d, BSB_SOCK_REG, S_RX_RD, (uint16_t)(rp + total)) != 0) return -1;
    if (command(d, SCR_RECV) != 0) return -1;
    return take;
}

static int e_recv(void *h, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    eth_t *d = h;
    if (d == NULL || !d->in_use || !d->started || buf == NULL || cap == 0) return -1;
    uint64_t t0 = s_bus->now_us();
    for (;;) {
        s_bus->spi_lock();
        int n = e_recv_once(d, buf, cap);
        s_bus->spi_unlock();
        if (n != 0) return n;                                               /* a frame, or an error */
        if ((s_bus->now_us() - t0) / 1000 >= timeout_ms) return 0;
        s_bus->delay_ms(1);
    }
}

static const purr_eth_ops_t s_ops = {.version = 1, .start = e_start, .stop = e_stop, .link = e_link, .send_frame = e_send, .recv_frame = e_recv};

/* ---------------------------------------------------------------- probe */

static int e_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    const purr_kv_cfg_t *c = (const purr_kv_cfg_t *)dev->cfg;
    if (c == NULL || board == NULL || out_handle == NULL) return -1;
    if (s_bus->spi_add_device == NULL || s_bus->spi_transfer == NULL || s_bus->spi_lock == NULL || s_bus->spi_unlock == NULL ||
        s_bus->now_us == NULL || s_bus->delay_ms == NULL) {
        return -1;
    }
    eth_t *d = &s_dev;
    if (d->in_use) return -1;                                               /* one controller */
    int cs = purr_kv(c, "cs", PURR_PIN_NONE);
    if (cs < 0) return -1;
    int host = board->spi.host;
    if (purr_kv(c, "own_bus", 0)) {
        if (s_bus->spi_open_bus == NULL) return -1;                         /* a kernel older than bus ABI 8 */
        host = purr_kv(c, "host", -1);
        if (host < 0) return -1;
        if (s_bus->spi_open_bus(host, purr_kv(c, "sclk", PURR_PIN_NONE), purr_kv(c, "mosi", PURR_PIN_NONE), purr_kv(c, "miso", PURR_PIN_NONE),
                                BUF_BYTES) != 0) {
            return -1;
        }
    }
    d->rst_pin = purr_kv(c, "rst", -1);
    if (d->rst_pin >= 0 && s_bus->gpio_set_output != NULL && s_bus->gpio_write != NULL) {
        s_bus->gpio_set_output(d->rst_pin, 1);
    } else {
        d->rst_pin = -1;
    }
    if (d->tx == NULL || d->rx == NULL) {
        d->tx = s_bus->dma_alloc ? s_bus->dma_alloc(BUF_BYTES) : s_fallback_tx;
        d->rx = s_bus->dma_alloc ? s_bus->dma_alloc(BUF_BYTES) : s_fallback_rx;
        if (d->tx == NULL || d->rx == NULL) {
            d->tx = d->rx = NULL;
            return -1;
        }
    }
    d->spi = s_bus->spi_add_device(host, cs, 0, purr_kv(c, "hz", 20000000));
    if (d->spi == 0) return -1;
    d->started = 0;
    d->in_use = 1;
    *out_handle = d;                                                        /* the chip is reset and started by start(), when the network is brought up */
    return 0;
}

static void e_remove(void *handle)
{
    eth_t *d = handle;
    if (d != NULL && d->in_use) {
        d->in_use = 0;
        d->started = 0;
    }
}

static const purr_driver_t s_driver = {
    .name = "w5500", .compatible = "w5500",
    .probe = e_probe, .remove = e_remove,
    .bus_abi = PURR_DRIVER_BUS_ABI_VERSION,
    .class_id = PURR_DRV_CLASS_ETHERNET, .ops = &s_ops,
};

__attribute__((used))
const purr_driver_t *w5500_entry(const purr_driver_bus_table_t *bus)
{
    s_bus = bus;
    return &s_driver;
}
