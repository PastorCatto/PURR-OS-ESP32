/* Cellular modem driven by AT commands over a UART (SIMCom SIM7xxx, Quectel BG9x / EC2x, u-blox SARA and most others: they share the 3GPP commands).
 *
 * A driver of class PURR_DRV_CLASS_MODEM (purr_driver_class.h), configured by named values (purr_kv_cfg_t):
 *     port=2  tx=17  rx=18  baud=115200      the UART
 *     pwr=-1    the modem's power-key pin: a 1 second high pulse switches it on (-1: it is on whenever the board is powered)
 *     rst=-1    the modem's reset pin, pulled low for 200 ms before power-up (-1: none)
 * Every operation is a short exchange of AT commands. The driver sends one line, then reads lines until it sees OK, ERROR, +CME ERROR or runs out of
 * time; it never blocks longer than the timeout it was given. It handles what a data session needs: registration (CEREG, with CREG as a fall-back),
 * signal (CSQ), operator (COPS), the IMEI, a PDP context (CGDCONT, CGAUTH, CGACT) and its address (CGPADDR). The modem's own socket stack differs
 * between makers, so `sockets` is NULL: the network stack uses the session's IP address through the board's PPP or its own TCP/IP. No libc.
 */
#include "purr_driver.h"
#include "purr_driver_bus.h"
#include "purr_driver_class.h"

static const purr_driver_bus_table_t *s_bus;

typedef struct {
    int in_use;
    uint32_t uart;
    int pwr_pin, rst_pin;
    char imei[18];
} modem_t;

static modem_t s_dev[2];

/* ---------------------------------------------------------------- tiny string helpers (no libc) */

static size_t slen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static int starts(const char *s, const char *p)
{
    while (*p) {
        if (*s++ != *p++) return 0;
    }
    return 1;
}

static const char *find(const char *s, const char *needle)
{
    for (; *s; s++) {
        if (starts(s, needle)) return s;
    }
    return 0;
}

/* appends `s` to dst[*n], keeping a terminator; returns 0, or -1 if it did not fit */
static int put(char *dst, size_t cap, size_t *n, const char *s)
{
    while (*s) {
        if (*n + 1 >= cap) return -1;
        dst[(*n)++] = *s++;
    }
    dst[*n] = '\0';
    return 0;
}

/* the integer at s (after optional spaces), advancing *end past it; 0 if there is none */
static int num(const char *s, const char **end, int *out)
{
    while (*s == ' ') s++;
    int neg = 0, got = 0, v = 0;
    if (*s == '-') {
        neg = 1;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        got = 1;
        s++;
    }
    if (end) *end = s;
    *out = neg ? -v : v;
    return got;
}

/* ---------------------------------------------------------------- AT exchange */

static void drain(const modem_t *d)
{
    uint8_t b[64];
    while (s_bus->uart_read(d->uart, b, sizeof(b), 0) > 0) {
    }
}

/* Sends `cmd` and collects the reply (everything up to and including the final OK/ERROR line) into resp. Returns the reply length (>= 0) when it ended in OK,
 * -1 on ERROR or +CME ERROR (resp holds the text), -2 on timeout. resp may be NULL. */
static int at_exchange(const modem_t *d, const char *cmd, char *resp, size_t cap, uint32_t timeout_ms)
{
    char local[160];
    char *out = resp ? resp : local;
    size_t ocap = resp ? cap : sizeof(local), n = 0;
    out[0] = '\0';
    drain(d);
    size_t cl = slen(cmd);
    if (cl > 120) return -1;
    uint8_t line[128];
    for (size_t i = 0; i < cl; i++) line[i] = (uint8_t)cmd[i];
    line[cl] = '\r';
    if (s_bus->uart_write(d->uart, line, cl + 1) < 0) return -1;
    uint64_t t0 = s_bus->now_us();
    for (;;) {
        uint8_t ch;
        int r = s_bus->uart_read(d->uart, &ch, 1, 20);
        if (r > 0) {
            if (n + 1 < ocap) {
                out[n++] = (char)ch;
                out[n] = '\0';
            }
            if (ch == '\n') {                                     /* a line is complete: is it the last one? */
                const char *p = out + n;
                const char *ls = p - 1;
                while (ls > out && ls[-1] != '\n') ls--;
                if (starts(ls, "OK")) return (int)n;
                if (starts(ls, "ERROR") || starts(ls, "+CME ERROR") || starts(ls, "+CMS ERROR") || starts(ls, "NO CARRIER")) return -1;
            }
        } else if ((s_bus->now_us() - t0) / 1000 > timeout_ms) {
            return -2;
        }
    }
}

static int alive(const modem_t *d)
{
    return at_exchange(d, "AT", 0, 0, 500) >= 0;
}

/* ---------------------------------------------------------------- operations */

static int m_power(void *h, int on)
{
    modem_t *d = h;
    if (d == NULL || !d->in_use) return -1;
    if (!on) {
        at_exchange(d, "AT+CFUN=0", 0, 0, 5000);
        return 0;
    }
    if (!alive(d)) {
        if (d->rst_pin >= 0) {
            s_bus->gpio_set_output(d->rst_pin, 0);
            s_bus->delay_ms(200);
            s_bus->gpio_set_output(d->rst_pin, 1);
        }
        if (d->pwr_pin >= 0) {
            s_bus->gpio_set_output(d->pwr_pin, 1);
            s_bus->delay_ms(1000);
            s_bus->gpio_set_output(d->pwr_pin, 0);
        }
        int ok = 0;
        for (int i = 0; i < 40 && !ok; i++) {                      /* up to 20 s for it to start answering */
            ok = alive(d);
            if (!ok) s_bus->delay_ms(500);
        }
        if (!ok) return -1;
    }
    at_exchange(d, "ATE0", 0, 0, 500);                             /* no echo */
    at_exchange(d, "AT+CMEE=1", 0, 0, 500);                        /* numeric error codes */
    return 0;
}

/* The state in "+CEREG: 0,1" or "+CEREG: 1": the last number of the line. */
static int reg_state(const char *resp, const char *tag)
{
    const char *p = find(resp, tag);
    if (!p) return -1;
    p += slen(tag);
    int a = 0, b = 0;
    const char *e;
    if (!num(p, &e, &a)) return -1;
    if (*e == ',' && num(e + 1, &e, &b)) return b;
    return a;
}

static int m_status(void *h, purr_modem_status_t *out)
{
    modem_t *d = h;
    if (d == NULL || !d->in_use || out == NULL) return -1;
    for (unsigned i = 0; i < sizeof(*out); i++) ((char *)out)[i] = 0;
    char r[160];
    if (at_exchange(d, "AT", 0, 0, 500) < 0) return -1;
    int st = -1;
    if (at_exchange(d, "AT+CEREG?", r, sizeof(r), 1000) >= 0) st = reg_state(r, "+CEREG:");
    if (st != 1 && st != 5 && at_exchange(d, "AT+CREG?", r, sizeof(r), 1000) >= 0) {
        int s2 = reg_state(r, "+CREG:");
        if (s2 == 1 || s2 == 5) st = s2;
    }
    out->registered = (st == 1 || st == 5);
    out->roaming = (st == 5);
    if (at_exchange(d, "AT+CSQ", r, sizeof(r), 1000) >= 0) {
        const char *p = find(r, "+CSQ:");
        int q;
        if (p && num(p + 5, 0, &q)) out->rssi_dbm = (int16_t)((q >= 0 && q < 99) ? -113 + 2 * q : 0);
    }
    if (out->registered && at_exchange(d, "AT+COPS?", r, sizeof(r), 2000) >= 0) {
        const char *p = find(r, "+COPS:");
        if (p) {
            const char *q1 = p;
            while (*q1 && *q1 != '"') q1++;
            if (*q1 == '"') {
                const char *q2 = q1 + 1;
                int k = 0;
                while (*q2 && *q2 != '"' && k < (int)sizeof(out->operator_name) - 1) out->operator_name[k++] = *q2++;
                out->operator_name[k] = '\0';
                if (*q2 == '"') {
                    int act = -1;
                    const char *e;
                    if (q2[1] == ',' && num(q2 + 2, &e, &act)) {
                        out->tech = act == 7 ? PURR_MODEM_TECH_LTE : act == 8 ? PURR_MODEM_TECH_LTEM : act == 9 ? PURR_MODEM_TECH_NBIOT : (act == 0 || act == 2 || act == 3) ? PURR_MODEM_TECH_GSM : PURR_MODEM_TECH_NONE;
                    }
                }
            }
        }
    }
    if (d->imei[0] == '\0' && at_exchange(d, "AT+CGSN", r, sizeof(r), 1000) >= 0) {
        const char *p = r;
        while (*p && (*p < '0' || *p > '9')) p++;
        int k = 0;
        while (*p >= '0' && *p <= '9' && k < (int)sizeof(d->imei) - 1) d->imei[k++] = *p++;
        d->imei[k] = '\0';
    }
    for (int i = 0; i < 18; i++) out->imei[i] = d->imei[i];
    if (at_exchange(d, "AT+CGPADDR=1", r, sizeof(r), 1000) >= 0) {
        const char *p = find(r, "+CGPADDR:");
        if (p) {
            while (*p && *p != '"' && *p != ',') p++;
            if (*p == ',' || *p == '"') {
                p++;
                if (*p == '"') p++;
                int k = 0;
                while (((*p >= '0' && *p <= '9') || *p == '.') && k < (int)sizeof(out->ip) - 1) out->ip[k++] = *p++;
                out->ip[k] = '\0';
                if (k > 0 && !starts(out->ip, "0.0.0.0")) out->data_up = 1;
                else out->ip[0] = '\0';
            }
        }
    }
    return 0;
}

static int m_connect(void *h, const char *apn, const char *user, const char *pass)
{
    modem_t *d = h;
    if (d == NULL || !d->in_use || apn == NULL) return -1;
    char c[140];
    size_t n = 0;
    c[0] = '\0';
    if (put(c, sizeof(c), &n, "AT+CGDCONT=1,\"IP\",\"") != 0 || put(c, sizeof(c), &n, apn) != 0 || put(c, sizeof(c), &n, "\"") != 0) return -1;
    if (at_exchange(d, c, 0, 0, 2000) < 0) return -1;
    if (user != NULL && user[0] != '\0') {                         /* 1 = PAP; not every modem has the command, so a refusal is not fatal */
        n = 0;
        c[0] = '\0';
        if (put(c, sizeof(c), &n, "AT+CGAUTH=1,1,\"") != 0 || put(c, sizeof(c), &n, pass ? pass : "") != 0 ||
            put(c, sizeof(c), &n, "\",\"") != 0 || put(c, sizeof(c), &n, user) != 0 || put(c, sizeof(c), &n, "\"") != 0) return -1;
        at_exchange(d, c, 0, 0, 2000);
    }
    if (at_exchange(d, "AT+CGACT=1,1", 0, 0, 30000) < 0) return -1;
    return 0;
}

static int m_disconnect(void *h)
{
    modem_t *d = h;
    if (d == NULL || !d->in_use) return -1;
    return at_exchange(d, "AT+CGACT=0,1", 0, 0, 10000) >= 0 ? 0 : -1;
}

static int m_at(void *h, const char *cmd, char *resp, size_t cap, uint32_t timeout_ms)
{
    modem_t *d = h;
    if (d == NULL || !d->in_use || cmd == NULL || resp == NULL || cap == 0) return -1;
    int r = at_exchange(d, cmd, resp, cap, timeout_ms);
    return r >= 0 ? r : (r == -1 ? (int)slen(resp) : -1);
}

static const purr_modem_ops_t s_ops = {.version = 1, .power = m_power, .status = m_status, .connect = m_connect, .disconnect = m_disconnect, .at = m_at, .sockets = 0};

static int m_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    (void)board;
    const purr_kv_cfg_t *c = (const purr_kv_cfg_t *)dev->cfg;
    if (c == NULL || s_bus->uart_open == NULL) return -1;
    modem_t *d = NULL;
    for (int i = 0; i < 2; i++) {
        if (!s_dev[i].in_use) {
            d = &s_dev[i];
            break;
        }
    }
    if (d == NULL) return -1;
    int rx = purr_kv(c, "rx", PURR_PIN_NONE), tx = purr_kv(c, "tx", PURR_PIN_NONE);
    if (rx < 0 || tx < 0) return -1;
    uint32_t u = s_bus->uart_open(purr_kv(c, "port", 2), tx, rx, purr_kv(c, "baud", 115200), 1024);
    if (u == 0) return -1;
    for (unsigned i = 0; i < sizeof(*d); i++) ((char *)d)[i] = 0;
    d->in_use = 1;
    d->uart = u;
    d->pwr_pin = purr_kv(c, "pwr", PURR_PIN_NONE);
    d->rst_pin = purr_kv(c, "rst", PURR_PIN_NONE);
    if (d->rst_pin >= 0) s_bus->gpio_set_output(d->rst_pin, 1);
    if (d->pwr_pin >= 0) s_bus->gpio_set_output(d->pwr_pin, 0);
    *out_handle = d;                                               /* the modem may be off: power(1) brings it up and checks that it answers */
    return 0;
}

static void m_remove(void *handle)
{
    modem_t *d = handle;
    if (d != NULL && d->in_use) {
        s_bus->uart_close(d->uart);
        d->in_use = 0;
    }
}

static const purr_driver_t s_driver = {
    .name = "modem-at", .compatible = "modem-at",
    .probe = m_probe, .remove = m_remove,
    .bus_abi = PURR_DRIVER_BUS_ABI_VERSION,
    .class_id = PURR_DRV_CLASS_MODEM, .ops = &s_ops,
};

__attribute__((used))
const purr_driver_t *modem_at_entry(const purr_driver_bus_table_t *bus)
{
    s_bus = bus;
    return &s_driver;
}
