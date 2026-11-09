/* SD card in SPI mode (SD, SDHC and SDXC; the T-Deck Plus's slot).
 *
 * A driver of class PURR_DRV_CLASS_BLOCK (purr_driver_class.h): it gives the OS 512-byte sectors, and the OS puts a FAT file system on top (main/purr_sd.c).
 * Configured by named values (purr_kv_cfg_t):
 *     cs=39      the card's chip select (a GPIO this driver drives itself: a card needs the line held low for a command, its answer and the data)
 *     hz=20000000  the speed after start-up (start-up itself is at 400 kHz, as the SD specification says)
 * The SPI bus is the board's own (board->spi.host), shared with the display and the radio. Every conversation holds the bus lock (bus ABI 7: spi_lock), so a
 * screen update cannot land in the middle of a command. The card is started lazily, the first time it is asked: a slot with no card is not an error until
 * someone wants the card, and a card taken out and put back is found again by the next call.
 *
 * Start-up (SD Physical Layer spec, SPI mode): 74+ clocks with the card deselected; CMD0 (go idle, with its fixed CRC); CMD8 (is it a version 2 card, 2.7-3.6 V);
 * ACMD41 until it leaves idle (asking for high capacity if it is version 2); CMD58 (the OCR: is it block addressed, SDHC?); CMD16 (512-byte blocks, for old
 * cards); CMD9 (the CSD, which says how big it is). Reads are CMD17 (CMD18 and CMD12 for several sectors), writes CMD24 (CMD25, with a stop token, for several).
 * CRCs are off in SPI mode after CMD0 and CMD8. No libc.
 */
#include "purr_driver.h"
#include "purr_driver_bus.h"
#include "purr_driver_class.h"

static const purr_driver_bus_table_t *s_bus;

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

#define SECTOR 512
#define IO_BYTES (SECTOR + 16)

typedef struct {
    int in_use;
    int cs;
    int fast_hz;
    uint32_t h_slow;             /* 400 kHz, for start-up (and its 80 clocks); the chip select is the driver's, so the bus layer is given none */
    uint32_t h_fast;             /* the working speed */
    uint32_t h;                  /* whichever is in use */
    uint8_t *ff;                 /* IO_BYTES of 0xFF: what is sent while reading */
    uint8_t *io;                 /* IO_BYTES: where a block is received or assembled */
    int ready;
    uint8_t card;                /* PURR_BLOCK_CARD_* */
    uint32_t sectors;
} sd_t;

static sd_t s_dev[2];

#define CMD0   0
#define CMD8   8
#define CMD9   9
#define CMD12  12
#define CMD16  16
#define CMD17  17
#define CMD18  18
#define CMD24  24
#define CMD25  25
#define CMD55  55
#define CMD58  58
#define ACMD41 41

/* ---------------------------------------------------------------- the wire */

static int xfer(sd_t *d, const uint8_t *tx, uint8_t *rx, size_t n)
{
    return s_bus->spi_transfer(d->h, tx, rx, n);
}

/* One byte out (0xFF to listen) and the byte that came back. */
static uint8_t exch(sd_t *d, uint8_t out)
{
    d->io[IO_BYTES - 1] = out;
    uint8_t in = 0xFF;
    if (s_bus->spi_transfer(d->h, d->io + IO_BYTES - 1, d->io + IO_BYTES - 2, 1) == 0) {
        in = d->io[IO_BYTES - 2];
    }
    return in;
}

static void select_card(sd_t *d)
{
    s_bus->gpio_write(d->cs, 0);
    exch(d, 0xFF);                                       /* one clock with it selected before the first command */
}

static void deselect_card(sd_t *d)
{
    s_bus->gpio_write(d->cs, 1);
    exch(d, 0xFF);                                       /* the card lets go of its output on the next clock */
}

/* Waits until the card is not busy (it holds its output low while it is). */
static int wait_ready(sd_t *d, uint32_t ms)
{
    uint64_t t0 = s_bus->now_us();
    for (;;) {
        if (exch(d, 0xFF) == 0xFF) {
            return 0;
        }
        if ((s_bus->now_us() - t0) / 1000 > ms) {
            return -1;
        }
    }
}

/* A command frame and the R1 answer (0xFF if none came). The card stays selected: the caller deselects. */
static uint8_t send_cmd(sd_t *d, uint8_t cmd, uint32_t arg)
{
    if (cmd != CMD0 && cmd != CMD12 && wait_ready(d, 500) != 0) {     /* a stop is sent while the card is still sending */
        return 0xFF;
    }
    uint8_t crc = cmd == CMD0 ? 0x95 : cmd == CMD8 ? 0x87 : 0x01;
    uint8_t f[6] = {(uint8_t)(0x40 | cmd), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)arg, crc};
    for (int i = 0; i < 6; i++) {
        exch(d, f[i]);
    }
    if (cmd == CMD12) {
        exch(d, 0xFF);                                   /* a byte the card sends while it stops */
    }
    uint8_t r = 0xFF;
    for (int i = 0; i < 10; i++) {                       /* the answer comes within 8 bytes */
        r = exch(d, 0xFF);
        if ((r & 0x80) == 0) {
            break;
        }
    }
    return r;
}

static uint8_t send_acmd(sd_t *d, uint8_t cmd, uint32_t arg)
{
    uint8_t r = send_cmd(d, CMD55, 0);
    if (r > 1) {
        return r;
    }
    return send_cmd(d, cmd, arg);
}

/* A data block of `n` bytes after a command: waits for the 0xFE token, then reads the bytes and the two CRC bytes. */
static int read_block(sd_t *d, uint8_t *dst, size_t n)
{
    uint64_t t0 = s_bus->now_us();
    uint8_t tok;
    do {
        tok = exch(d, 0xFF);
        if (tok != 0xFF) {
            break;
        }
    } while ((s_bus->now_us() - t0) / 1000 < 300);
    if (tok != 0xFE) {
        return -1;
    }
    if (xfer(d, d->ff, d->io, n + 2) != 0) {
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        dst[i] = d->io[i];
    }
    return 0;
}

/* ---------------------------------------------------------------- start-up */

static int card_start(sd_t *d)
{
    d->ready = 0;
    d->card = PURR_BLOCK_CARD_NONE;
    d->sectors = 0;
    d->h = d->h_slow;
    s_bus->gpio_write(d->cs, 1);
    xfer(d, d->ff, NULL, 10);                            /* 80 clocks with the card deselected */

    select_card(d);
    uint8_t r = 0xFF;
    for (int i = 0; i < 5 && r != 0x01; i++) {           /* into idle */
        r = send_cmd(d, CMD0, 0);
        if (r != 0x01) {
            deselect_card(d);
            s_bus->delay_ms(10);
            select_card(d);
        }
    }
    if (r != 0x01) {
        deselect_card(d);
        return -1;                                       /* no card, or it does not answer */
    }
    int v2 = 0;
    r = send_cmd(d, CMD8, 0x1AA);
    if (r == 0x01) {
        uint8_t ocr[4];
        for (int i = 0; i < 4; i++) {
            ocr[i] = exch(d, 0xFF);
        }
        if (ocr[2] != 0x01 || ocr[3] != 0xAA) {
            deselect_card(d);
            return -1;                                   /* it does not take 3.3 V */
        }
        v2 = 1;
    } else if (r != 0x05) {                              /* anything but "illegal command" is not a card we know */
        deselect_card(d);
        return -1;
    }
    uint64_t t0 = s_bus->now_us();
    do {
        r = send_acmd(d, ACMD41, v2 ? 0x40000000u : 0);
        if (r == 0x00) {
            break;
        }
        s_bus->delay_ms(5);
    } while ((s_bus->now_us() - t0) / 1000 < 1500);
    if (r != 0x00) {
        deselect_card(d);
        return -1;
    }
    d->card = PURR_BLOCK_CARD_SDSC;
    if (v2) {
        r = send_cmd(d, CMD58, 0);
        if (r == 0x00) {
            uint8_t ocr[4];
            for (int i = 0; i < 4; i++) {
                ocr[i] = exch(d, 0xFF);
            }
            if (ocr[0] & 0x40) {
                d->card = PURR_BLOCK_CARD_SDHC;          /* the "card capacity status" bit: block addressed */
            }
        }
    }
    if (d->card == PURR_BLOCK_CARD_SDSC && send_cmd(d, CMD16, SECTOR) != 0x00) {
        deselect_card(d);
        return -1;
    }
    /* how big: the CSD register */
    uint8_t csd[16];
    if (send_cmd(d, CMD9, 0) != 0x00 || read_block(d, csd, 16) != 0) {
        deselect_card(d);
        return -1;
    }
    deselect_card(d);
    if ((csd[0] >> 6) == 1) {                            /* version 2: (C_SIZE + 1) * 512 KB */
        uint32_t c_size = ((uint32_t)(csd[7] & 0x3F) << 16) | ((uint32_t)csd[8] << 8) | csd[9];
        d->sectors = (c_size + 1) * 1024u;
    } else {                                             /* version 1: (C_SIZE + 1) << (C_SIZE_MULT + 2) blocks of 2^READ_BL_LEN bytes */
        uint32_t read_bl = csd[5] & 0x0F;
        uint32_t c_size = ((uint32_t)(csd[6] & 0x03) << 10) | ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
        uint32_t mult = ((csd[9] & 0x03) << 1) | (csd[10] >> 7);
        uint64_t blocks = (uint64_t)(c_size + 1) << (mult + 2);
        uint64_t bytes = blocks << read_bl;
        d->sectors = (uint32_t)(bytes / SECTOR);
    }
    d->h = d->h_fast != 0 ? d->h_fast : d->h_slow;       /* the working speed */
    d->ready = d->sectors > 0;
    return d->ready ? 0 : -1;
}

static int ensure(sd_t *d)
{
    if (d->ready) {
        return 0;
    }
    s_bus->spi_lock();
    int rc = card_start(d);
    s_bus->spi_unlock();
    return rc;
}

/* ---------------------------------------------------------------- the operations */

static uint32_t addr_of(sd_t *d, uint32_t lba)
{
    return d->card == PURR_BLOCK_CARD_SDHC ? lba : lba * SECTOR;
}

static int b_info(void *h, purr_block_info_t *out)
{
    sd_t *d = h;
    if (d == NULL || !d->in_use || out == NULL) return -1;
    if (ensure(d) != 0) return -1;
    out->sectors = d->sectors;
    out->sector_size = SECTOR;
    out->removable = 1;
    out->writable = 1;
    out->card = d->card;
    out->reserved = 0;
    return 0;
}

static int b_read(void *h, uint32_t lba, uint8_t *buf, uint32_t count)
{
    sd_t *d = h;
    if (d == NULL || !d->in_use || buf == NULL || count == 0) return -1;
    if (ensure(d) != 0) return -1;
    if (lba >= d->sectors || count > d->sectors - lba) return -1;
    int rc = 0;
    s_bus->spi_lock();
    select_card(d);
    if (count == 1) {
        if (send_cmd(d, CMD17, addr_of(d, lba)) != 0x00 || read_block(d, buf, SECTOR) != 0) rc = -1;
    } else {
        if (send_cmd(d, CMD18, addr_of(d, lba)) != 0x00) {
            rc = -1;
        } else {
            for (uint32_t i = 0; i < count && rc == 0; i++) {
                if (read_block(d, buf + (size_t)i * SECTOR, SECTOR) != 0) rc = -1;
            }
            send_cmd(d, CMD12, 0);                       /* stop; the card answers and then is busy for a moment */
            wait_ready(d, 500);
        }
    }
    deselect_card(d);
    s_bus->spi_unlock();
    if (rc != 0) d->ready = 0;                           /* maybe the card was taken out: start again next time */
    return rc;
}

/* One data block out: token, 512 bytes, two CRC bytes (not checked), then the card's data response and its busy time. */
static int write_block(sd_t *d, uint8_t token, const uint8_t *src)
{
    d->io[0] = token;
    for (int i = 0; i < SECTOR; i++) {
        d->io[1 + i] = src[i];
    }
    d->io[SECTOR + 1] = 0xFF;
    d->io[SECTOR + 2] = 0xFF;
    if (xfer(d, d->io, NULL, SECTOR + 3) != 0) {
        return -1;
    }
    uint8_t resp = exch(d, 0xFF);
    if ((resp & 0x1F) != 0x05) {
        return -1;                                       /* rejected: a bad CRC or a write error */
    }
    return wait_ready(d, 1000);
}

static int b_write(void *h, uint32_t lba, const uint8_t *buf, uint32_t count)
{
    sd_t *d = h;
    if (d == NULL || !d->in_use || buf == NULL || count == 0) return -1;
    if (ensure(d) != 0) return -1;
    if (lba >= d->sectors || count > d->sectors - lba) return -1;
    int rc = 0;
    s_bus->spi_lock();
    select_card(d);
    if (count == 1) {
        if (send_cmd(d, CMD24, addr_of(d, lba)) != 0x00 || write_block(d, 0xFE, buf) != 0) rc = -1;
    } else {
        if (send_cmd(d, CMD25, addr_of(d, lba)) != 0x00) {
            rc = -1;
        } else {
            for (uint32_t i = 0; i < count && rc == 0; i++) {
                if (write_block(d, 0xFC, buf + (size_t)i * SECTOR) != 0) rc = -1;
            }
            exch(d, 0xFD);                               /* the stop-transmission token */
            exch(d, 0xFF);
            if (wait_ready(d, 1000) != 0) rc = -1;
        }
    }
    deselect_card(d);
    s_bus->spi_unlock();
    if (rc != 0) d->ready = 0;
    return rc;
}

static int b_sync(void *h)
{
    sd_t *d = h;
    if (d == NULL || !d->in_use) return -1;
    if (!d->ready) return 0;
    int rc;
    s_bus->spi_lock();
    select_card(d);
    rc = wait_ready(d, 1000);
    deselect_card(d);
    s_bus->spi_unlock();
    return rc;
}

static const purr_block_ops_t s_ops = {.version = 1, .info = b_info, .read = b_read, .write = b_write, .sync = b_sync};

static int b_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    const purr_kv_cfg_t *c = (const purr_kv_cfg_t *)dev->cfg;
    if (c == NULL || board == NULL || s_bus->spi_lock == NULL || s_bus->dma_alloc == NULL) return -1;
    sd_t *d = NULL;
    for (int i = 0; i < 2; i++) {
        if (!s_dev[i].in_use) {
            d = &s_dev[i];
            break;
        }
    }
    if (d == NULL) return -1;
    int cs = purr_kv(c, "cs", PURR_PIN_NONE);
    if (cs < 0) return -1;
    uint8_t *ff = d->ff, *io = d->io;                    /* buffers are never freed (dma_alloc), so a second probe reuses them */
    for (unsigned i = 0; i < sizeof(*d); i++) ((char *)d)[i] = 0;
    d->cs = cs;
    d->fast_hz = purr_kv(c, "hz", 20000000);
    d->ff = ff != NULL ? ff : s_bus->dma_alloc(IO_BYTES);
    d->io = io != NULL ? io : s_bus->dma_alloc(IO_BYTES);
    if (d->ff == NULL || d->io == NULL) return -1;
    for (int i = 0; i < IO_BYTES; i++) d->ff[i] = 0xFF;
    s_bus->gpio_set_output(cs, 1);                       /* deselected: the card must not hear the display */
    d->h_slow = s_bus->spi_add_device(board->spi.host, PURR_PIN_NONE, 0, 400000);
    d->h_fast = s_bus->spi_add_device(board->spi.host, PURR_PIN_NONE, 0, d->fast_hz);
    if (d->h_slow == 0 || d->h_fast == 0) return -1;
    d->h = d->h_slow;
    d->in_use = 1;
    *out_handle = d;                                     /* the card itself is started on first use */
    return 0;
}

static void b_remove(void *handle)
{
    sd_t *d = handle;
    if (d != NULL && d->in_use) {
        s_bus->gpio_write(d->cs, 1);
        d->in_use = 0;
    }
}

static const purr_driver_t s_driver = {
    .name = "sd-spi", .compatible = "sd-spi",
    .probe = b_probe, .remove = b_remove,
    .bus_abi = PURR_DRIVER_BUS_ABI_VERSION,
    .class_id = PURR_DRV_CLASS_BLOCK, .ops = &s_ops,
};

__attribute__((used))
const purr_driver_t *sd_spi_entry(const purr_driver_bus_table_t *bus)
{
    s_bus = bus;
    return &s_driver;
}
