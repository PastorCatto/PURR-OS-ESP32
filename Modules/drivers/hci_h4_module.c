/* A Bluetooth controller on a UART, speaking HCI over the H4 transport: one byte for the packet type, then the packet.
 *
 * A driver of class PURR_DRV_CLASS_HCI (purr_driver_class.h), configured by named values (purr_kv_cfg_t):
 *     port=2  tx=17  rx=18  baud=921600      the UART (many controllers start at 115200 and need a vendor command to go faster; set the speed the
 *                                           controller is already using)
 *     en=-1    the controller's enable pin, high for "on" (-1: always on)           rst=-1   its reset pin
 * It does the framing and nothing else: write() sends a packet with its type byte, read() returns one whole packet (type byte first, then the header and its
 * payload, sized from the header), and reset() sends HCI_Reset and waits for its Command Complete. A Bluetooth stack above it (HCI commands, L2CAP, GATT)
 * is separate. Hardware flow control is not available through the bus table, so use a speed the receive buffer can keep up with. No libc.
 */
#include "purr_driver.h"
#include "purr_driver_bus.h"
#include "purr_driver_class.h"

static const purr_driver_bus_table_t *s_bus;

#define MAX_PACKET 1100

typedef struct {
    int in_use;
    uint32_t uart;
    int en_pin, rst_pin;
} hci_t;

static hci_t s_dev[2];

/* Reads exactly n bytes, each within `per_byte_ms` of the last; returns n, or the count that arrived. */
static int read_n(const hci_t *d, uint8_t *dst, int n, uint32_t per_byte_ms)
{
    int got = 0;
    while (got < n) {
        int r = s_bus->uart_read(d->uart, dst + got, (size_t)(n - got), per_byte_ms);
        if (r <= 0) break;
        got += r;
    }
    return got;
}

static int h_write(void *h, uint8_t type, const uint8_t *data, size_t len)
{
    hci_t *d = h;
    if (d == NULL || !d->in_use || type < PURR_HCI_COMMAND || type > PURR_HCI_ISO || (len > 0 && data == NULL) || len + 1 > MAX_PACKET) return -1;
    uint8_t pkt[MAX_PACKET];
    pkt[0] = type;
    for (size_t i = 0; i < len; i++) pkt[1 + i] = data[i];
    return s_bus->uart_write(d->uart, pkt, len + 1) == (int)(len + 1) ? 0 : -1;
}

/* One packet: type byte, header, payload. Returns its length (type byte included), 0 if nothing arrived in time, -1 if it is bigger than `cap` (it is
 * skipped, so the stream stays in step), -2 if the stream broke off in the middle. */
static int h_read(void *h, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    hci_t *d = h;
    if (d == NULL || !d->in_use || buf == NULL || cap < 5) return -1;
    uint8_t t;
    if (read_n(d, &t, 1, timeout_ms) != 1) return 0;
    int hdr, plen_off, plen_size;
    switch (t) {
    case PURR_HCI_EVENT: hdr = 2; plen_off = 1; plen_size = 1; break;       /* event code, parameter length */
    case PURR_HCI_ACL:   hdr = 4; plen_off = 2; plen_size = 2; break;       /* handle and flags, data length (16 bits) */
    case PURR_HCI_SCO:   hdr = 3; plen_off = 2; plen_size = 1; break;       /* handle and flags, data length */
    case PURR_HCI_ISO:   hdr = 4; plen_off = 2; plen_size = 2; break;       /* handle and flags, data load length (14 bits) */
    default: return -2;                                                      /* not a packet type: the stream is out of step */
    }
    uint8_t head[4];
    if (read_n(d, head, hdr, 200) != hdr) return -2;
    int plen = head[plen_off];
    if (plen_size == 2) plen |= head[plen_off + 1] << 8;
    if (t == PURR_HCI_ISO) plen &= 0x3FFF;
    int total = 1 + hdr + plen;
    if ((size_t)total > cap) {
        uint8_t skip[32];
        int left = plen;
        while (left > 0) {                                                   /* drop the payload so the next packet starts cleanly */
            int n = read_n(d, skip, left > 32 ? 32 : left, 200);
            if (n <= 0) break;
            left -= n;
        }
        return -1;
    }
    buf[0] = t;
    for (int i = 0; i < hdr; i++) buf[1 + i] = head[i];
    if (read_n(d, buf + 1 + hdr, plen, 200) != plen) return -2;
    return total;
}

static int h_reset(void *h)
{
    hci_t *d = h;
    if (d == NULL || !d->in_use) return -1;
    if (d->rst_pin >= 0) {
        s_bus->gpio_set_output(d->rst_pin, 0);
        s_bus->delay_ms(20);
        s_bus->gpio_set_output(d->rst_pin, 1);
        s_bus->delay_ms(100);
    }
    if (d->en_pin >= 0) {
        s_bus->gpio_set_output(d->en_pin, 1);
        s_bus->delay_ms(100);
    }
    uint8_t junk[64];
    while (s_bus->uart_read(d->uart, junk, sizeof(junk), 0) > 0) {            /* anything left over from before */
    }
    static const uint8_t reset[3] = {0x03, 0x0C, 0x00};                       /* opcode 0x0C03 (little endian), no parameters */
    if (h_write(h, PURR_HCI_COMMAND, reset, sizeof(reset)) != 0) return -1;
    uint64_t t0 = s_bus->now_us();
    while ((s_bus->now_us() - t0) / 1000 < 2000) {
        uint8_t pkt[64];
        int n = h_read(h, pkt, sizeof(pkt), 100);
        if (n >= 7 && pkt[0] == PURR_HCI_EVENT && pkt[1] == 0x0E && pkt[4] == 0x03 && pkt[5] == 0x0C) return pkt[6] == 0 ? 0 : -1;   /* Command Complete for HCI_Reset */
    }
    return -1;
}

static const purr_hci_ops_t s_ops = {.version = 1, .reset = h_reset, .write = h_write, .read = h_read};

static int h_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    (void)board;
    const purr_kv_cfg_t *c = (const purr_kv_cfg_t *)dev->cfg;
    if (c == NULL || s_bus->uart_open == NULL) return -1;
    hci_t *d = NULL;
    for (int i = 0; i < 2; i++) {
        if (!s_dev[i].in_use) {
            d = &s_dev[i];
            break;
        }
    }
    if (d == NULL) return -1;
    int rx = purr_kv(c, "rx", PURR_PIN_NONE), tx = purr_kv(c, "tx", PURR_PIN_NONE);
    if (rx < 0 || tx < 0) return -1;
    uint32_t u = s_bus->uart_open(purr_kv(c, "port", 2), tx, rx, purr_kv(c, "baud", 921600), 4096);
    if (u == 0) return -1;
    for (unsigned i = 0; i < sizeof(*d); i++) ((char *)d)[i] = 0;
    d->in_use = 1;
    d->uart = u;
    d->en_pin = purr_kv(c, "en", PURR_PIN_NONE);
    d->rst_pin = purr_kv(c, "rst", PURR_PIN_NONE);
    *out_handle = d;
    return 0;
}

static void h_remove(void *handle)
{
    hci_t *d = handle;
    if (d != NULL && d->in_use) {
        s_bus->uart_close(d->uart);
        d->in_use = 0;
    }
}

static const purr_driver_t s_driver = {
    .name = "hci-h4", .compatible = "hci-h4",
    .probe = h_probe, .remove = h_remove,
    .bus_abi = PURR_DRIVER_BUS_ABI_VERSION,
    .class_id = PURR_DRV_CLASS_HCI, .ops = &s_ops,
};

__attribute__((used))
const purr_driver_t *hci_h4_entry(const purr_driver_bus_table_t *bus)
{
    s_bus = bus;
    return &s_driver;
}
