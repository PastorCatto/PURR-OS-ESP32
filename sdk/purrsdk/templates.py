"""Starter sources for drivers: one per driver class, each a complete module that builds, loads and says "not yet" until you fill it in.

A driver is a freestanding module (no libc, no OS calls): it reaches hardware only through the bus table the OS hands it. See sdk/docs/drivers.md.
"""

import re

CLASSES = {
    "gnss": {"title": "GPS / GNSS receiver", "bus": "uart", "const": "PURR_DRV_CLASS_GNSS", "ops": "purr_gnss_ops_t",
             "settings": "port=1 tx=43 rx=44 baud=38400", "compatible": "gnss-%s"},
    "lora": {"title": "LoRa radio", "bus": "spi", "const": "PURR_DRV_CLASS_LORA", "ops": "purr_lora_ops_t",
             "settings": "cs=9 rst=-1 busy=13 dio1=45", "compatible": "lora-%s"},
    "modem": {"title": "Cellular (LTE) modem", "bus": "uart", "const": "PURR_DRV_CLASS_MODEM", "ops": "purr_modem_ops_t",
              "settings": "port=2 tx=17 rx=18 baud=115200 pwr=-1", "compatible": "modem-%s"},
    "wifi": {"title": "Wi-Fi module that is not the chip's own", "bus": "uart", "const": "PURR_DRV_CLASS_WIFI", "ops": "purr_wifi_ops_t",
             "settings": "port=2 tx=17 rx=18 baud=115200 en=-1", "compatible": "wifi-%s"},
    "bluetooth": {"title": "Bluetooth controller (HCI transport)", "bus": "uart", "const": "PURR_DRV_CLASS_HCI", "ops": "purr_hci_ops_t",
                  "settings": "port=2 tx=17 rx=18 baud=921600", "compatible": "hci-%s"},
    "sensor-i2c": {"title": "I2C sensor", "bus": "i2c", "const": None, "ops": None,
                   "settings": "sda=18 scl=8 addr=0x76", "compatible": "%s"},
}

HEAD = """/* {title}: {name}.
 *
 * {intro}
 *
 * Settings come from the board file ({settings}); read them with purr_kv(cfg, "key", fallback).
 * A driver is freestanding: no libc, no OS calls, only the bus table below. Build and sign it with the kit:
 *     purr build   (then `purr serve` to hand it to a device)
 */
#include "purr_driver.h"
#include "purr_driver_bus.h"
{class_include}
static const purr_driver_bus_table_t *s_bus;

typedef struct {{
    int in_use;
{fields}}} dev_t_{ident};

static dev_t_{ident} s_dev[2];

static dev_t_{ident} *take(void)
{{
    for (int i = 0; i < 2; i++) {{
        if (!s_dev[i].in_use) {{
            for (unsigned b = 0; b < sizeof(s_dev[i]); b++) ((char *)&s_dev[i])[b] = 0;
            s_dev[i].in_use = 1;
            return &s_dev[i];
        }}
    }}
    return NULL;
}}
"""

OPS = {
    "gnss": """
static int op_power(void *h, int on)
{
    (void)h;
    (void)on;
    return -1;                      /* TODO: switch the receiver on or off (a power pin, or a command) */
}

static int op_poll(void *h)
{
    (void)h;
    return 0;                       /* TODO: read the bytes the receiver sent and parse them; return how many sentences you handled */
}

static int op_get_fix(void *h, purr_gnss_fix_t *out)
{
    (void)h;
    (void)out;
    return 0;                       /* TODO: poll, then fill `out`; 1 for a valid fix, 0 for none yet */
}

static int op_set_rate(void *h, int hz)
{
    (void)h;
    (void)hz;
    return -1;
}

static const purr_gnss_ops_t s_ops = {.version = 1, .power = op_power, .poll = op_poll, .get_fix = op_get_fix, .set_rate_hz = op_set_rate};
""",
    "lora": """
static int op_configure(void *h, const purr_lora_cfg_t *cfg)
{
    (void)h;
    (void)cfg;
    return -1;                      /* TODO: frequency, bandwidth, spreading factor, coding rate, power, sync word */
}

static int op_send(void *h, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    (void)h;
    (void)data;
    (void)len;
    (void)timeout_ms;
    return -1;                      /* TODO: one packet; 0 once it has gone out */
}

static int op_receive(void *h, uint8_t *buf, size_t cap, uint32_t timeout_ms, purr_lora_rxinfo_t *info)
{
    (void)h;
    (void)buf;
    (void)cap;
    (void)timeout_ms;
    (void)info;
    return 0;                       /* TODO: the bytes of one packet, 0 if none arrived in time */
}

static int op_sleep(void *h)
{
    (void)h;
    return 0;
}

static const purr_lora_ops_t s_ops = {.version = 1, .configure = op_configure, .send = op_send, .receive = op_receive, .sleep = op_sleep};
""",
    "modem": """
static int op_power(void *h, int on)
{
    (void)h;
    (void)on;
    return -1;                      /* TODO: power key and reset line, then wait for the modem to answer "AT" */
}

static int op_status(void *h, purr_modem_status_t *out)
{
    (void)h;
    (void)out;
    return -1;                      /* TODO: AT+CEREG?, AT+CSQ, AT+COPS?, AT+CGPADDR */
}

static int op_connect(void *h, const char *apn, const char *user, const char *pass)
{
    (void)h;
    (void)apn;
    (void)user;
    (void)pass;
    return -1;                      /* TODO: AT+CGDCONT, AT+CGACT */
}

static int op_disconnect(void *h)
{
    (void)h;
    return -1;
}

static int op_at(void *h, const char *cmd, char *resp, size_t cap, uint32_t timeout_ms)
{
    (void)h;
    (void)cmd;
    (void)resp;
    (void)cap;
    (void)timeout_ms;
    return -1;                      /* TODO: send the line, collect the reply up to OK or ERROR */
}

static const purr_modem_ops_t s_ops = {.version = 1, .power = op_power, .status = op_status, .connect = op_connect,
                                       .disconnect = op_disconnect, .at = op_at, .sockets = NULL};
""",
    "wifi": """
static int op_power(void *h, int on)
{
    (void)h;
    (void)on;
    return -1;
}

static int op_scan(void *h, purr_wifi_ap_t *out, int max, uint32_t timeout_ms)
{
    (void)h;
    (void)out;
    (void)max;
    (void)timeout_ms;
    return -1;                      /* TODO: list the access points; how many were found */
}

static int op_connect(void *h, const char *ssid, const char *pass, uint32_t timeout_ms)
{
    (void)h;
    (void)ssid;
    (void)pass;
    (void)timeout_ms;
    return -1;                      /* TODO: join, then wait for an address */
}

static int op_disconnect(void *h)
{
    (void)h;
    return -1;
}

static int op_status(void *h, purr_wifi_status_t *out)
{
    (void)h;
    (void)out;
    return -1;
}

static const purr_wifi_ops_t s_ops = {.version = 1, .power = op_power, .scan = op_scan, .connect = op_connect, .disconnect = op_disconnect,
                                      .status = op_status, .sockets = NULL, .send_frame = NULL, .recv_frame = NULL};
""",
    "bluetooth": """
static int op_reset(void *h)
{
    (void)h;
    return -1;                      /* TODO: send HCI_Reset (01 03 0C 00) and wait for its Command Complete event */
}

static int op_write(void *h, uint8_t type, const uint8_t *data, size_t len)
{
    (void)h;
    (void)type;
    (void)data;
    (void)len;
    return -1;                      /* TODO: one HCI packet; on a UART (H4) it is the type byte then the packet */
}

static int op_read(void *h, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    (void)h;
    (void)buf;
    (void)cap;
    (void)timeout_ms;
    return 0;                       /* TODO: one packet with its type as the first byte; its length, 0 if none in time */
}

static const purr_hci_ops_t s_ops = {.version = 1, .reset = op_reset, .write = op_write, .read = op_read};
""",
    "sensor-i2c": "",
}

PROBE_UART = """
static int d_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{{
    (void)board;
    const purr_kv_cfg_t *c = (const purr_kv_cfg_t *)dev->cfg;
    if (c == NULL || s_bus->uart_open == NULL) return -1;
    dev_t_{ident} *d = take();
    if (d == NULL) return -1;
    d->uart = s_bus->uart_open(purr_kv(c, "port", 1), purr_kv(c, "tx", PURR_PIN_NONE), purr_kv(c, "rx", PURR_PIN_NONE), purr_kv(c, "baud", 115200), 2048);
    if (d->uart == 0) {{
        d->in_use = 0;
        return -1;
    }}
    *out_handle = d;
    return 0;
}}

static void d_remove(void *handle)
{{
    dev_t_{ident} *d = handle;
    if (d != NULL && d->in_use) {{
        s_bus->uart_close(d->uart);
        d->in_use = 0;
    }}
}}
"""

PROBE_SPI = """
static int d_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{{
    (void)board;
    const purr_kv_cfg_t *c = (const purr_kv_cfg_t *)dev->cfg;
    if (c == NULL) return -1;
    dev_t_{ident} *d = take();
    if (d == NULL) return -1;
    d->cs = purr_kv(c, "cs", PURR_PIN_NONE);
    d->rst = purr_kv(c, "rst", PURR_PIN_NONE);
    d->busy = purr_kv(c, "busy", PURR_PIN_NONE);
    /* TODO: use s_bus->spi_* and s_bus->gpio_* to reset the chip and check that it answers; return -1 if it does not. */
    *out_handle = d;
    return 0;
}}

static void d_remove(void *handle)
{{
    dev_t_{ident} *d = handle;
    if (d != NULL) d->in_use = 0;
}}
"""

PROBE_I2C = """
static int d_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{{
    (void)board;
    const purr_i2c_dev_cfg_t *c = (const purr_i2c_dev_cfg_t *)dev->cfg;
    if (c == NULL) return -1;
    dev_t_{ident} *d = take();
    if (d == NULL) return -1;
    d->addr = c->addr;
    /* TODO: use s_bus->i2c_* to read the chip's id register and return -1 if it is not the part you expect. */
    *out_handle = d;
    return 0;
}}

static void d_remove(void *handle)
{{
    dev_t_{ident} *d = handle;
    if (d != NULL) d->in_use = 0;
}}
"""

FIELDS = {"uart": "    uint32_t uart;\n", "spi": "    int cs, rst, busy;\n", "i2c": "    uint8_t addr;\n"}
PROBES = {"uart": PROBE_UART, "spi": PROBE_SPI, "i2c": PROBE_I2C}

TAIL = """
static const purr_driver_t s_driver = {{
    .name = "{compatible}", .compatible = "{compatible}",
    .probe = d_probe, .remove = d_remove,
    .bus_abi = PURR_DRIVER_BUS_ABI_VERSION,
{class_fields}}};

__attribute__((used))
const purr_driver_t *{entry}(const purr_driver_bus_table_t *bus)
{{
    s_bus = bus;
    return &s_driver;
}}
"""

NAME_RE = re.compile(r"^[a-z][a-z0-9_-]{1,30}$")


def ident(name):
    return re.sub(r"[^a-z0-9]", "_", name.lower())


def driver_source(name, cls, compatible=None):
    """The C source of a starter driver. Raises ValueError for a bad name or class."""
    if not NAME_RE.match(name):
        raise ValueError("Name the driver with lowercase letters, digits, - or _ (2 to 31 characters), starting with a letter.")
    if cls not in CLASSES:
        raise ValueError("Unknown driver class %r. Known: %s." % (cls, ", ".join(CLASSES)))
    spec = CLASSES[cls]
    ident_ = ident(name)
    compatible = compatible or name
    entry = ident_ + "_entry"
    bus = spec["bus"]
    intro = ("A driver of class %s (purr_driver_class.h): fill in the TODOs in the operations below." % cls) if spec["ops"] else \
            "A plain driver for a chip on the I2C bus; the OS binds it to a board device whose `compatible` matches."
    src = HEAD.format(title=spec["title"], name=name, intro=intro, settings=spec["settings"], ident=ident_,
                      class_include='#include "purr_driver_class.h"\n' if spec["ops"] else "", fields=FIELDS[bus])
    src += OPS[cls]
    src += PROBES[bus].format(ident=ident_)
    class_fields = ""
    if spec["ops"]:
        class_fields = "    .class_id = %s, .ops = &s_ops,\n" % spec["const"]
    src += TAIL.format(compatible=compatible, entry=entry, class_fields=class_fields)
    return src, entry, compatible, bus


APP_SOURCE = None
