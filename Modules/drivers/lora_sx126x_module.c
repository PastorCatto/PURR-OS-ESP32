/* LoRa radio: Semtech SX1261 / SX1262 / SX1268 over SPI (the T-Deck Plus's radio is an SX1262).
 *
 * A driver of class PURR_DRV_CLASS_LORA (purr_driver_class.h), configured by named values (purr_kv_cfg_t):
 *     cs=9  busy=13  dio1=45  rst=-1   the pins; busy is the chip's "busy" line (-1: wait a fixed time instead), rst is optional
 *     tcxo_mv=0     the voltage that DIO3 gives a TCXO (0: the board has a plain crystal; 1600 ... 3300 otherwise)
 *     dio2sw=1      DIO2 drives the antenna switch (most boards)        pwr=-1   a power-enable pin, high for "on"
 *     hz=8000000    SPI clock
 * The SPI bus is the board's own (board->spi.host); on the T-Deck it is shared with the display and the SD card, so the other chip selects must be held high
 * (the board file lists them as idle pins). Each operation is a short sequence of the chip's SPI commands (Semtech's SX126x datasheet, section 13); the driver
 * waits for BUSY to fall before every command, and gives up after 200 ms rather than hang.
 */
#include "purr_driver.h"
#include "purr_driver_bus.h"
#include "purr_driver_class.h"

static const purr_driver_bus_table_t *s_bus;

typedef struct {
    int in_use;
    uint32_t spi;
    int busy_pin, dio1_pin, rst_pin, pwr_pin;
    purr_lora_cfg_t cfg;
    int configured;
} lora_t;

static lora_t s_dev[2];

/* A module has no libc, but the compiler may still call memcpy and memset for struct copies and array initialisers, so the module brings its own. */
void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)c;
    return dst;
}

/* SX126x opcodes */
#define OP_GET_STATUS       0xC0
#define OP_SET_SLEEP        0x84
#define OP_SET_STANDBY      0x80
#define OP_SET_TX           0x83
#define OP_SET_RX           0x82
#define OP_SET_RF_FREQ      0x86
#define OP_SET_PKT_TYPE     0x8A
#define OP_SET_PA_CFG       0x95
#define OP_SET_TX_PARAMS    0x8E
#define OP_SET_BUF_BASE     0x8F
#define OP_SET_MOD_PARAMS   0x8B
#define OP_SET_PKT_PARAMS   0x8C
#define OP_SET_DIO_IRQ      0x08
#define OP_GET_IRQ          0x12
#define OP_CLR_IRQ          0x02
#define OP_WRITE_BUF        0x0E
#define OP_READ_BUF         0x1E
#define OP_GET_RX_STATUS    0x13
#define OP_GET_PKT_STATUS   0x14
#define OP_WRITE_REG        0x0D
#define OP_CALIBRATE        0x89
#define OP_CALIBRATE_IMAGE  0x98
#define OP_SET_REG_MODE     0x96
#define OP_SET_DIO3_TCXO    0x97
#define OP_SET_DIO2_RFSW    0x9D

#define IRQ_TX_DONE   0x0001
#define IRQ_RX_DONE   0x0002
#define IRQ_CRC_ERR   0x0040
#define IRQ_TIMEOUT   0x0200

#define REG_SYNC_WORD 0x0740

static int wait_busy(const lora_t *d)
{
    if (d->busy_pin < 0) {
        s_bus->delay_ms(2);
        return 0;
    }
    uint64_t t0 = s_bus->now_us();
    while (s_bus->gpio_get(d->busy_pin)) {
        if (s_bus->now_us() - t0 > 200000) {
            return -1;                                   /* the chip never came back: do not hang the system for it */
        }
        s_bus->delay_ms(1);
    }
    return 0;
}

/* One SPI transaction of n bytes (tx and rx the same length). */
static int xfer(const lora_t *d, const uint8_t *tx, uint8_t *rx, size_t n)
{
    if (wait_busy(d) != 0) return -1;
    return s_bus->spi_transfer(d->spi, tx, rx, n);
}

static int cmd(const lora_t *d, uint8_t op, const uint8_t *args, size_t n)
{
    uint8_t buf[16];
    if (n > sizeof(buf) - 1) return -1;
    buf[0] = op;
    for (size_t i = 0; i < n; i++) buf[1 + i] = args[i];
    return xfer(d, buf, 0, n + 1);
}

static int cmd1(const lora_t *d, uint8_t op, uint8_t a)
{
    return cmd(d, op, &a, 1);
}

static int get_irq(const lora_t *d, uint16_t *irq)
{
    uint8_t tx[4] = {OP_GET_IRQ, 0, 0, 0}, rx[4];
    if (xfer(d, tx, rx, 4) != 0) return -1;
    *irq = (uint16_t)((rx[2] << 8) | rx[3]);
    return 0;
}

static int clear_irq(const lora_t *d, uint16_t mask)
{
    uint8_t a[2] = {(uint8_t)(mask >> 8), (uint8_t)mask};
    return cmd(d, OP_CLR_IRQ, a, 2);
}

/* The chip answers GetStatus with its mode in bits 6:4: 2 or 3 is standby. An absent chip leaves MISO floating (all ones: mode 7), so this tells a radio from nothing. */
static int chip_alive(const lora_t *d)
{
    uint8_t tx[2] = {OP_GET_STATUS, 0}, rx[2] = {0, 0};
    if (xfer(d, tx, rx, 2) != 0) return 0;
    int mode = (rx[1] >> 4) & 7;
    return mode == 2 || mode == 3;
}

static uint8_t bw_code(uint32_t bw)
{
    static const struct { uint32_t hz; uint8_t code; } t[] = {
        {7800, 0x00}, {10400, 0x08}, {15600, 0x01}, {20800, 0x09}, {31250, 0x02},
        {41700, 0x0A}, {62500, 0x03}, {125000, 0x04}, {250000, 0x05}, {500000, 0x06},
    };
    uint8_t best = 0x04;
    uint32_t diff = 0xFFFFFFFFu;
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {        /* the nearest supported bandwidth */
        uint32_t dd = t[i].hz > bw ? t[i].hz - bw : bw - t[i].hz;
        if (dd < diff) {
            diff = dd;
            best = t[i].code;
        }
    }
    return best;
}

static uint32_t bw_hz_of(uint8_t code)
{
    switch (code) {
    case 0x00: return 7800;
    case 0x08: return 10400;
    case 0x01: return 15600;
    case 0x09: return 20800;
    case 0x02: return 31250;
    case 0x0A: return 41700;
    case 0x03: return 62500;
    case 0x04: return 125000;
    case 0x05: return 250000;
    default: return 500000;
    }
}

static int l_configure(void *h, const purr_lora_cfg_t *cfg)
{
    lora_t *d = h;
    if (d == NULL || !d->in_use || cfg == NULL) return -1;
    if (cfg->sf < 5 || cfg->sf > 12 || cfg->cr < 5 || cfg->cr > 8 || cfg->freq_hz < 150000000u || cfg->freq_hz > 960000000u) return -1;
    int rc = cmd1(d, OP_SET_STANDBY, 0x00);                          /* standby, RC oscillator */
    /* Image calibration for the band (datasheet 9.2.1). */
    uint8_t img[2];
    uint32_t mhz = cfg->freq_hz / 1000000u;
    if (mhz >= 902) { img[0] = 0xE1; img[1] = 0xE9; }
    else if (mhz >= 863) { img[0] = 0xD7; img[1] = 0xDB; }
    else if (mhz >= 779) { img[0] = 0xC1; img[1] = 0xC5; }
    else if (mhz >= 470) { img[0] = 0x75; img[1] = 0x81; }
    else { img[0] = 0x6B; img[1] = 0x6F; }
    rc |= cmd(d, OP_CALIBRATE_IMAGE, img, 2);
    /* RF frequency: freq x 2^25 / 32 MHz. */
    uint32_t raw = (uint32_t)(((uint64_t)cfg->freq_hz << 25) / 32000000u);
    uint8_t f[4] = {(uint8_t)(raw >> 24), (uint8_t)(raw >> 16), (uint8_t)(raw >> 8), (uint8_t)raw};
    rc |= cmd(d, OP_SET_RF_FREQ, f, 4);
    /* PA for the SX1262 at up to +22 dBm, then the power and a 200 us ramp. */
    static const uint8_t pa[4] = {0x04, 0x07, 0x00, 0x01};
    rc |= cmd(d, OP_SET_PA_CFG, pa, 4);
    int8_t p = cfg->tx_dbm > 22 ? 22 : (cfg->tx_dbm < -9 ? -9 : cfg->tx_dbm);
    uint8_t tp[2] = {(uint8_t)p, 0x04};
    rc |= cmd(d, OP_SET_TX_PARAMS, tp, 2);
    /* Modulation: SF, BW, CR (4/5 is code 1), low data rate optimisation when a symbol lasts 16 ms or more. */
    uint32_t bwh = bw_hz_of(bw_code(cfg->bw_hz));
    int ldro = ((uint64_t)(1u << cfg->sf) * 1000u / bwh) >= 16 ? 1 : 0;
    uint8_t mp[4] = {cfg->sf, bw_code(cfg->bw_hz), (uint8_t)(cfg->cr - 4), (uint8_t)ldro};
    rc |= cmd(d, OP_SET_MOD_PARAMS, mp, 4);
    uint16_t pre = cfg->preamble ? cfg->preamble : 8;
    uint8_t pk[6] = {(uint8_t)(pre >> 8), (uint8_t)pre, cfg->implicit_header ? 1 : 0, 0xFF, cfg->crc_on ? 1 : 0, 0};
    rc |= cmd(d, OP_SET_PKT_PARAMS, pk, 6);
    uint16_t sw = cfg->sync_word ? cfg->sync_word : 0x1424;
    uint8_t reg[4] = {(uint8_t)(REG_SYNC_WORD >> 8), (uint8_t)REG_SYNC_WORD, (uint8_t)(sw >> 8), (uint8_t)sw};
    rc |= cmd(d, OP_WRITE_REG, reg, 4);
    /* Interrupts: TxDone, RxDone, CRC error and timeout, all on DIO1. */
    uint16_t m = IRQ_TX_DONE | IRQ_RX_DONE | IRQ_CRC_ERR | IRQ_TIMEOUT;
    uint8_t irq[8] = {(uint8_t)(m >> 8), (uint8_t)m, (uint8_t)(m >> 8), (uint8_t)m, 0, 0, 0, 0};
    rc |= cmd(d, OP_SET_DIO_IRQ, irq, 8);
    if (rc != 0) return -1;
    d->cfg = *cfg;
    d->configured = 1;
    return 0;
}

static int l_send(void *h, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    lora_t *d = h;
    if (d == NULL || !d->in_use || !d->configured || data == NULL || len == 0 || len > 255) return -1;
    int rc = cmd1(d, OP_SET_STANDBY, 0x00);
    rc |= clear_irq(d, 0xFFFF);
    uint8_t pk[6] = {(uint8_t)(d->cfg.preamble >> 8), (uint8_t)d->cfg.preamble, d->cfg.implicit_header ? 1 : 0, (uint8_t)len, d->cfg.crc_on ? 1 : 0, 0};
    if (d->cfg.preamble == 0) { pk[0] = 0; pk[1] = 8; }
    rc |= cmd(d, OP_SET_PKT_PARAMS, pk, 6);
    uint8_t w[257];
    w[0] = OP_WRITE_BUF;
    w[1] = 0x00;
    for (size_t i = 0; i < len; i++) w[2 + i] = data[i];
    rc |= xfer(d, w, 0, len + 2);
    static const uint8_t tx0[3] = {0, 0, 0};                          /* no timeout: it ends with TxDone */
    rc |= cmd(d, OP_SET_TX, tx0, 3);
    if (rc != 0) return -1;
    uint64_t t0 = s_bus->now_us();
    for (;;) {
        uint16_t irq = 0;
        if (get_irq(d, &irq) != 0) return -1;
        if (irq & IRQ_TX_DONE) {
            clear_irq(d, 0xFFFF);
            cmd1(d, OP_SET_STANDBY, 0x00);
            return 0;
        }
        if (irq & IRQ_TIMEOUT) break;
        if ((s_bus->now_us() - t0) / 1000 > timeout_ms) break;
        s_bus->delay_ms(2);
    }
    cmd1(d, OP_SET_STANDBY, 0x00);                                    /* gave up: leave the chip idle, not transmitting */
    return -2;
}

static int l_receive(void *h, uint8_t *buf, size_t cap, uint32_t timeout_ms, purr_lora_rxinfo_t *info)
{
    lora_t *d = h;
    if (d == NULL || !d->in_use || !d->configured || buf == NULL || cap == 0) return -1;
    int rc = cmd1(d, OP_SET_STANDBY, 0x00);
    rc |= clear_irq(d, 0xFFFF);
    static const uint8_t rx_cont[3] = {0xFF, 0xFF, 0xFF};             /* receive until told to stop */
    rc |= cmd(d, OP_SET_RX, rx_cont, 3);
    if (rc != 0) return -1;
    uint64_t t0 = s_bus->now_us();
    for (;;) {
        uint16_t irq = 0;
        if (get_irq(d, &irq) != 0) return -1;
        if (irq & IRQ_RX_DONE) {
            clear_irq(d, 0xFFFF);
            if ((irq & IRQ_CRC_ERR) && d->cfg.crc_on) {               /* a damaged packet: keep listening */
                cmd(d, OP_SET_RX, rx_cont, 3);
                continue;
            }
            uint8_t st_tx[4] = {OP_GET_RX_STATUS, 0, 0, 0}, st[4];
            if (xfer(d, st_tx, st, 4) != 0) return -1;
            size_t plen = st[2], start = st[3];
            uint8_t ps_tx[5] = {OP_GET_PKT_STATUS, 0, 0, 0, 0}, ps[5];
            if (xfer(d, ps_tx, ps, 5) != 0) return -1;
            if (info != NULL) {
                info->rssi_dbm = (int16_t)(-(int)ps[2] / 2);
                info->snr_db = (int8_t)((int8_t)ps[3] / 4);
                info->reserved = 0;
            }
            if (plen > cap) plen = cap;
            uint8_t rb_tx[260], rb[260];
            for (size_t i = 0; i < plen + 3; i++) rb_tx[i] = 0;
            rb_tx[0] = OP_READ_BUF;
            rb_tx[1] = (uint8_t)start;
            if (xfer(d, rb_tx, rb, plen + 3) != 0) return -1;
            for (size_t i = 0; i < plen; i++) buf[i] = rb[3 + i];
            cmd1(d, OP_SET_STANDBY, 0x00);
            return (int)plen;
        }
        if ((s_bus->now_us() - t0) / 1000 > timeout_ms) break;
        s_bus->delay_ms(2);
    }
    cmd1(d, OP_SET_STANDBY, 0x00);
    return 0;                                                         /* nothing arrived in time */
}

static int l_sleep(void *h)
{
    lora_t *d = h;
    if (d == NULL || !d->in_use) return -1;
    return cmd1(d, OP_SET_SLEEP, 0x04);                               /* warm start: keep the configuration */
}

static const purr_lora_ops_t s_ops = {.version = 1, .configure = l_configure, .send = l_send, .receive = l_receive, .sleep = l_sleep};

static int l_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    const purr_kv_cfg_t *c = (const purr_kv_cfg_t *)dev->cfg;
    if (c == NULL || board == NULL) return -1;
    lora_t *d = NULL;
    for (int i = 0; i < 2; i++) {
        if (!s_dev[i].in_use) {
            d = &s_dev[i];
            break;
        }
    }
    if (d == NULL) return -1;
    int cs = purr_kv(c, "cs", PURR_PIN_NONE);
    if (cs < 0) return -1;
    for (unsigned i = 0; i < sizeof(*d); i++) ((char *)d)[i] = 0;
    d->busy_pin = purr_kv(c, "busy", PURR_PIN_NONE);
    d->dio1_pin = purr_kv(c, "dio1", PURR_PIN_NONE);
    d->rst_pin = purr_kv(c, "rst", PURR_PIN_NONE);
    d->pwr_pin = purr_kv(c, "pwr", PURR_PIN_NONE);
    if (d->pwr_pin >= 0) {
        s_bus->gpio_set_output(d->pwr_pin, 1);
        s_bus->delay_ms(20);
    }
    if (d->busy_pin >= 0) s_bus->gpio_set_input_pullup(d->busy_pin);
    if (d->dio1_pin >= 0) s_bus->gpio_set_input_pullup(d->dio1_pin);
    d->spi = s_bus->spi_add_device(board->spi.host, cs, 0, purr_kv(c, "hz", 8000000));
    if (d->spi == 0) return -1;
    if (d->rst_pin >= 0) {                                            /* reset: low for a moment, then let it start */
        s_bus->gpio_set_output(d->rst_pin, 0);
        s_bus->delay_ms(2);
        s_bus->gpio_set_output(d->rst_pin, 1);
        s_bus->delay_ms(10);
    }
    d->in_use = 1;
    int rc = cmd1(d, OP_SET_STANDBY, 0x00);
    if (rc != 0 || !chip_alive(d)) {                                   /* no radio answers here */
        d->in_use = 0;
        return -1;
    }
    int tcxo = purr_kv(c, "tcxo_mv", 0);
    if (tcxo > 0) {
        uint8_t code = tcxo < 1700 ? 0 : tcxo < 1800 ? 1 : tcxo < 2200 ? 2 : tcxo < 2400 ? 3 : tcxo < 2700 ? 4 : tcxo < 3000 ? 5 : tcxo < 3300 ? 6 : 7;
        uint8_t a[4] = {code, 0x00, 0x01, 0x40};                       /* a 20 ms start-up delay in 15.625 us steps */
        rc |= cmd(d, OP_SET_DIO3_TCXO, a, 4);
        rc |= cmd1(d, OP_CALIBRATE, 0x7F);                             /* everything, now that the clock is steady */
        s_bus->delay_ms(5);
    }
    if (purr_kv(c, "dio2sw", 1)) rc |= cmd1(d, OP_SET_DIO2_RFSW, 0x01);
    rc |= cmd1(d, OP_SET_REG_MODE, 0x01);                              /* DC-DC */
    rc |= cmd1(d, OP_SET_PKT_TYPE, 0x01);                              /* LoRa */
    static const uint8_t base[2] = {0x00, 0x00};
    rc |= cmd(d, OP_SET_BUF_BASE, base, 2);
    if (rc != 0) {                                                     /* nothing answered: no radio here */
        d->in_use = 0;
        return -1;
    }
    *out_handle = d;
    return 0;
}

static void l_remove(void *handle)
{
    lora_t *d = handle;
    if (d != NULL && d->in_use) {
        cmd1(d, OP_SET_SLEEP, 0x04);
        d->in_use = 0;
    }
}

static const purr_driver_t s_driver = {
    .name = "lora-sx126x", .compatible = "lora-sx126x",
    .probe = l_probe, .remove = l_remove,
    .bus_abi = PURR_DRIVER_BUS_ABI_VERSION,
    .class_id = PURR_DRV_CLASS_LORA, .ops = &s_ops,
};

__attribute__((used))
const purr_driver_t *lora_sx126x_entry(const purr_driver_bus_table_t *bus)
{
    s_bus = bus;
    return &s_driver;
}
