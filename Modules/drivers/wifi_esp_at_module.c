/* Wi-Fi from a second chip running Espressif's AT firmware (ESP-AT) over a UART: an ESP32-C3 or ESP8266 module, or any board that carries one.
 *
 * A driver of class PURR_DRV_CLASS_WIFI (purr_driver_class.h), configured by named values (purr_kv_cfg_t):
 *     port=2  tx=17  rx=18  baud=115200      the UART
 *     en=-1    the module's enable pin, high for "on" (-1: always on)           rst=-1   its reset pin
 * It scans (AT+CWLAP), joins (AT+CWJAP), reports its state (CWJAP?, CIPSTA?) and gives the module's own TCP sockets (AT+CIPSTART, CIPSEND, passive
 * receive with CIPRECVLEN and CIPRECVDATA), so the network stack can use it without a TCP/IP stack of its own for this link. TCP only. No libc.
 */
#include "purr_driver.h"
#include "purr_driver_bus.h"
#include "purr_driver_class.h"

static const purr_driver_bus_table_t *s_bus;

#define MAX_SOCK 4

typedef struct {
    int in_use;
    uint32_t uart;
    int en_pin, rst_pin;
    uint8_t open[MAX_SOCK];
    uint8_t closed[MAX_SOCK];                      /* the module said "<id>,CLOSED" */
} wifi_t;

static wifi_t s_dev[2];
static const purr_sock_ops_t s_sock_ops;

/* ---------------------------------------------------------------- tiny helpers (no libc) */

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

static int put(char *dst, size_t cap, size_t *n, const char *s)
{
    while (*s) {
        if (*n + 1 >= cap) return -1;
        dst[(*n)++] = *s++;
    }
    dst[*n] = '\0';
    return 0;
}

/* a quoted string argument: ESP-AT wants \, " and , escaped with a backslash */
static int put_quoted(char *dst, size_t cap, size_t *n, const char *s)
{
    if (put(dst, cap, n, "\"") != 0) return -1;
    for (; *s; s++) {
        if (*s == '\\' || *s == '"' || *s == ',') {
            if (*n + 2 >= cap) return -1;
            dst[(*n)++] = '\\';
        }
        if (*n + 1 >= cap) return -1;
        dst[(*n)++] = *s;
    }
    dst[*n] = '\0';
    return put(dst, cap, n, "\"");
}

static int put_num(char *dst, size_t cap, size_t *n, unsigned v)
{
    char t[12];
    int k = 0;
    do {
        t[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0 && k < 11);
    char o[12];
    for (int i = 0; i < k; i++) o[i] = t[k - 1 - i];
    o[k] = '\0';
    return put(dst, cap, n, o);
}

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

static void drain(const wifi_t *d)
{
    uint8_t b[64];
    while (s_bus->uart_read(d->uart, b, sizeof(b), 0) > 0) {
    }
}

static void note_closed(wifi_t *d, const char *text)
{
    for (int id = 0; id < MAX_SOCK; id++) {
        char pat[10] = {(char)('0' + id), ',', 'C', 'L', 'O', 'S', 'E', 'D', 0, 0};
        if (find(text, pat)) {
            d->closed[id] = 1;
            d->open[id] = 0;
        }
    }
}

/* Reads until a line that is OK / ERROR / FAIL / SEND OK / SEND FAIL arrives, the byte `stop` (if not 0) arrives, or time runs out.
 * Returns the length (>= 0) on OK or SEND OK or the stop byte, -1 on ERROR or FAIL, -2 on timeout. */
static int collect(wifi_t *d, char *out, size_t ocap, uint32_t timeout_ms, char stop)
{
    size_t n = 0;
    out[0] = '\0';
    uint64_t t0 = s_bus->now_us();
    for (;;) {
        uint8_t ch;
        int r = s_bus->uart_read(d->uart, &ch, 1, 20);
        if (r > 0) {
            if (n + 1 < ocap) {
                out[n++] = (char)ch;
                out[n] = '\0';
            }
            if (stop && (char)ch == stop) {
                note_closed(d, out);
                return (int)n;
            }
            if (ch == '\n') {
                const char *ls = out + n - 1;
                while (ls > out && ls[-1] != '\n') ls--;
                if (starts(ls, "OK") || starts(ls, "SEND OK")) {
                    note_closed(d, out);
                    return (int)n;
                }
                if (starts(ls, "ERROR") || starts(ls, "FAIL") || starts(ls, "SEND FAIL")) {
                    note_closed(d, out);
                    return -1;
                }
            }
        } else if ((s_bus->now_us() - t0) / 1000 > timeout_ms) {
            note_closed(d, out);
            return -2;
        }
    }
}

static int send_line(wifi_t *d, const char *cmd)
{
    size_t cl = slen(cmd);
    uint8_t line[200];
    if (cl > 190) return -1;
    for (size_t i = 0; i < cl; i++) line[i] = (uint8_t)cmd[i];
    line[cl] = '\r';
    line[cl + 1] = '\n';
    return s_bus->uart_write(d->uart, line, cl + 2) < 0 ? -1 : 0;
}

static int at(wifi_t *d, const char *cmd, char *resp, size_t cap, uint32_t timeout_ms)
{
    char local[200];
    drain(d);
    if (send_line(d, cmd) != 0) return -1;
    return collect(d, resp ? resp : local, resp ? cap : sizeof(local), timeout_ms, 0);
}

/* ---------------------------------------------------------------- operations */

static int w_power(void *h, int on)
{
    wifi_t *d = h;
    if (d == NULL || !d->in_use) return -1;
    if (!on) {
        if (d->en_pin >= 0) s_bus->gpio_set_output(d->en_pin, 0);
        else at(d, "AT+GSLP=0", 0, 0, 500);
        return 0;
    }
    if (d->rst_pin >= 0) {
        s_bus->gpio_set_output(d->rst_pin, 0);
        s_bus->delay_ms(50);
        s_bus->gpio_set_output(d->rst_pin, 1);
    }
    if (d->en_pin >= 0) {
        s_bus->gpio_set_output(d->en_pin, 1);
        s_bus->delay_ms(300);
    }
    int ok = 0;
    for (int i = 0; i < 20 && !ok; i++) {                                    /* up to 10 s for the firmware to start */
        ok = at(d, "AT", 0, 0, 400) >= 0;
        if (!ok) s_bus->delay_ms(500);
    }
    if (!ok) return -1;
    at(d, "ATE0", 0, 0, 500);
    int rc = at(d, "AT+CWMODE=1", 0, 0, 1000);                               /* station */
    rc |= at(d, "AT+CIPMUX=1", 0, 0, 1000) < 0 ? -1 : 0;                     /* several sockets */
    rc |= at(d, "AT+CIPRECVMODE=1", 0, 0, 1000) < 0 ? -1 : 0;                /* passive receive: the data waits in the module until it is asked for */
    return rc < 0 ? -1 : 0;
}

/* One line of +CWLAP:(ecn,"ssid",rssi,"mac",channel,...) into `ap`; returns 1 if it parsed */
static int parse_ap(const char *p, purr_wifi_ap_t *ap)
{
    int ecn, rssi, ch;
    const char *e;
    p += 8;                                                                   /* past "+CWLAP:" */
    while (*p == ' ' || *p == '(') p++;
    if (!num(p, &e, &ecn) || *e != ',') return 0;
    p = e + 1;
    if (*p != '"') return 0;
    p++;
    int k = 0;
    while (*p && *p != '"' && k < 32) {
        if (*p == '\\' && p[1]) p++;
        ap->ssid[k++] = *p++;
    }
    ap->ssid[k] = '\0';
    if (*p != '"') return 0;
    p++;
    if (*p != ',' || !num(p + 1, &e, &rssi) || *e != ',') return 0;
    p = e + 1;
    if (*p != '"') return 0;
    p++;
    while (*p && *p != '"') p++;
    if (*p != '"') return 0;
    p++;
    if (*p != ',' || !num(p + 1, &e, &ch)) return 0;
    ap->secure = ecn != 0;
    ap->rssi_dbm = (int8_t)rssi;
    ap->channel = (uint8_t)ch;
    return 1;
}

static int w_scan(void *h, purr_wifi_ap_t *out, int max, uint32_t timeout_ms)
{
    wifi_t *d = h;
    if (d == NULL || !d->in_use || out == NULL || max <= 0) return -1;
    static char big[2048];
    int r = at(d, "AT+CWLAP", big, sizeof(big), timeout_ms);
    if (r < 0) return -1;
    int count = 0;
    for (const char *p = big; (p = find(p, "+CWLAP:")) != 0; p += 7) {
        if (count < max && parse_ap(p, &out[count])) count++;
    }
    return count;
}

static int w_status(void *h, purr_wifi_status_t *out)
{
    wifi_t *d = h;
    if (d == NULL || !d->in_use || out == NULL) return -1;
    for (unsigned i = 0; i < sizeof(*out); i++) ((char *)out)[i] = 0;
    char r[320];
    if (at(d, "AT+CWJAP?", r, sizeof(r), 2000) < 0) return -1;
    const char *p = find(r, "+CWJAP:");
    if (p == 0) return 0;                                                     /* "No AP": not connected */
    p += 7;
    if (*p == '"') {
        p++;
        int k = 0;
        while (*p && *p != '"' && k < 32) {
            if (*p == '\\' && p[1]) p++;
            out->ssid[k++] = *p++;
        }
        out->ssid[k] = '\0';
        if (*p == '"') p++;
        int v;
        const char *e;
        if (*p == ',' && p[1] == '"') {                                        /* skip the BSSID */
            p += 2;
            while (*p && *p != '"') p++;
            if (*p == '"') p++;
        }
        if (*p == ',' && num(p + 1, &e, &v)) {
            out->channel = (uint8_t)v;
            if (*e == ',' && num(e + 1, &e, &v)) out->rssi_dbm = (int8_t)v;
        }
    }
    if (at(d, "AT+CIPSTA?", r, sizeof(r), 2000) >= 0) {
        const char *q = find(r, "+CIPSTA:ip:\"");
        if (q) {
            q += 12;
            int k = 0;
            while (((*q >= '0' && *q <= '9') || *q == '.') && k < (int)sizeof(out->ip) - 1) out->ip[k++] = *q++;
            out->ip[k] = '\0';
        }
    }
    out->connected = out->ssid[0] != '\0' && out->ip[0] != '\0' && !starts(out->ip, "0.0.0.0");
    return 0;
}

static int w_connect(void *h, const char *ssid, const char *pass, uint32_t timeout_ms)
{
    wifi_t *d = h;
    if (d == NULL || !d->in_use || ssid == NULL) return -1;
    char c[160];
    size_t n = 0;
    c[0] = '\0';
    if (put(c, sizeof(c), &n, "AT+CWJAP=") != 0 || put_quoted(c, sizeof(c), &n, ssid) != 0 || put(c, sizeof(c), &n, ",") != 0 ||
        put_quoted(c, sizeof(c), &n, pass ? pass : "") != 0) return -1;
    if (at(d, c, 0, 0, timeout_ms) < 0) return -1;
    purr_wifi_status_t st;
    for (int i = 0; i < 20; i++) {                                           /* the address can follow the join by a moment */
        if (w_status(h, &st) == 0 && st.connected) return 0;
        s_bus->delay_ms(250);
    }
    return -1;
}

static int w_disconnect(void *h)
{
    wifi_t *d = h;
    if (d == NULL || !d->in_use) return -1;
    return at(d, "AT+CWQAP", 0, 0, 2000) >= 0 ? 0 : -1;
}

/* ---------------------------------------------------------------- sockets */

static int s_open(void *h, const char *host, uint16_t port, int tcp, uint32_t timeout_ms)
{
    wifi_t *d = h;
    if (d == NULL || !d->in_use || host == NULL || !tcp) return -1;           /* TCP only */
    int id = -1;
    for (int i = 0; i < MAX_SOCK; i++) {
        if (!d->open[i]) {
            id = i;
            break;
        }
    }
    if (id < 0) return -1;
    char c[160];
    size_t n = 0;
    c[0] = '\0';
    d->closed[id] = 0;
    if (put(c, sizeof(c), &n, "AT+CIPSTART=") != 0 || put_num(c, sizeof(c), &n, (unsigned)id) != 0 || put(c, sizeof(c), &n, ",\"TCP\",") != 0 ||
        put_quoted(c, sizeof(c), &n, host) != 0 || put(c, sizeof(c), &n, ",") != 0 || put_num(c, sizeof(c), &n, port) != 0) return -1;
    if (at(d, c, 0, 0, timeout_ms) < 0) return -1;
    d->open[id] = 1;
    return id;
}

static int s_send(void *h, int sock, const uint8_t *data, size_t len)
{
    wifi_t *d = h;
    if (d == NULL || !d->in_use || sock < 0 || sock >= MAX_SOCK || !d->open[sock] || data == NULL || len == 0 || len > 2048) return -1;
    char c[48], r[96];
    size_t n = 0;
    c[0] = '\0';
    if (put(c, sizeof(c), &n, "AT+CIPSEND=") != 0 || put_num(c, sizeof(c), &n, (unsigned)sock) != 0 || put(c, sizeof(c), &n, ",") != 0 ||
        put_num(c, sizeof(c), &n, (unsigned)len) != 0) return -1;
    drain(d);
    if (send_line(d, c) != 0) return -1;
    if (collect(d, r, sizeof(r), 2000, '>') < 0) return -1;                    /* the module answers ">" when it is ready for the bytes */
    if (s_bus->uart_write(d->uart, data, len) < 0) return -1;
    return collect(d, r, sizeof(r), 5000, 0) >= 0 ? (int)len : -1;             /* "SEND OK" */
}

static int s_recv(void *h, int sock, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    wifi_t *d = h;
    if (d == NULL || !d->in_use || sock < 0 || sock >= MAX_SOCK || buf == NULL || cap == 0) return -1;
    uint64_t t0 = s_bus->now_us();
    for (;;) {
        char r[96];
        if (at(d, "AT+CIPRECVLEN?", r, sizeof(r), 1000) >= 0) {
            const char *p = find(r, "+CIPRECVLEN:");
            int have = 0;
            if (p) {
                p += 12;
                for (int i = 0; i <= sock; i++) {
                    const char *e;
                    int v = 0;
                    num(p, &e, &v);
                    if (i == sock) have = v;
                    p = *e == ',' ? e + 1 : e;
                }
            }
            if (have > 0) {
                char c[48];
                size_t n = 0;
                c[0] = '\0';
                unsigned want = (unsigned)(have < (int)cap ? have : (int)cap);
                if (want > 1024) want = 1024;
                put(c, sizeof(c), &n, "AT+CIPRECVDATA=");
                put_num(c, sizeof(c), &n, (unsigned)sock);
                put(c, sizeof(c), &n, ",");
                put_num(c, sizeof(c), &n, want);
                drain(d);
                if (send_line(d, c) != 0) return -1;
                /* "+CIPRECVDATA:<len>,<bytes>": read to the comma, then exactly <len> raw bytes */
                char head[24];
                int hn = 0, got_len = -1;
                uint64_t th = s_bus->now_us();
                while ((s_bus->now_us() - th) / 1000 < 2000) {
                    uint8_t ch;
                    if (s_bus->uart_read(d->uart, &ch, 1, 20) <= 0) continue;
                    if (hn < (int)sizeof(head) - 1) head[hn++] = (char)ch;
                    head[hn] = '\0';
                    if (ch == ',' && find(head, "+CIPRECVDATA:")) {
                        const char *p2 = find(head, "+CIPRECVDATA:") + 13;
                        num(p2, 0, &got_len);
                        break;
                    }
                }
                if (got_len < 0) return -1;
                int got = 0;
                uint64_t td = s_bus->now_us();
                while (got < got_len && (s_bus->now_us() - td) / 1000 < 2000) {
                    uint8_t ch;
                    if (s_bus->uart_read(d->uart, &ch, 1, 20) <= 0) continue;
                    if ((size_t)got < cap) buf[got] = ch;
                    got++;
                }
                char tail[32];
                collect(d, tail, sizeof(tail), 500, 0);                         /* the closing OK */
                return got < (int)cap ? got : (int)cap;
            }
        }
        if (d->closed[sock]) return -1;
        if ((s_bus->now_us() - t0) / 1000 >= timeout_ms) return 0;
        s_bus->delay_ms(100);
    }
}

static int s_close(void *h, int sock)
{
    wifi_t *d = h;
    if (d == NULL || !d->in_use || sock < 0 || sock >= MAX_SOCK) return -1;
    char c[32];
    size_t n = 0;
    c[0] = '\0';
    put(c, sizeof(c), &n, "AT+CIPCLOSE=");
    put_num(c, sizeof(c), &n, (unsigned)sock);
    d->open[sock] = 0;
    return at(d, c, 0, 0, 2000) >= 0 ? 0 : -1;
}

static const purr_sock_ops_t s_sock_ops = {.version = 1, .open = s_open, .send = s_send, .recv = s_recv, .close = s_close};

static const purr_wifi_ops_t s_ops = {.version = 1, .power = w_power, .scan = w_scan, .connect = w_connect, .disconnect = w_disconnect, .status = w_status,
                                      .sockets = &s_sock_ops, .send_frame = 0, .recv_frame = 0};

static int w_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    (void)board;
    const purr_kv_cfg_t *c = (const purr_kv_cfg_t *)dev->cfg;
    if (c == NULL || s_bus->uart_open == NULL) return -1;
    wifi_t *d = NULL;
    for (int i = 0; i < 2; i++) {
        if (!s_dev[i].in_use) {
            d = &s_dev[i];
            break;
        }
    }
    if (d == NULL) return -1;
    int rx = purr_kv(c, "rx", PURR_PIN_NONE), tx = purr_kv(c, "tx", PURR_PIN_NONE);
    if (rx < 0 || tx < 0) return -1;
    uint32_t u = s_bus->uart_open(purr_kv(c, "port", 2), tx, rx, purr_kv(c, "baud", 115200), 2048);
    if (u == 0) return -1;
    for (unsigned i = 0; i < sizeof(*d); i++) ((char *)d)[i] = 0;
    d->in_use = 1;
    d->uart = u;
    d->en_pin = purr_kv(c, "en", PURR_PIN_NONE);
    d->rst_pin = purr_kv(c, "rst", PURR_PIN_NONE);
    *out_handle = d;
    return 0;
}

static void w_remove(void *handle)
{
    wifi_t *d = handle;
    if (d != NULL && d->in_use) {
        s_bus->uart_close(d->uart);
        d->in_use = 0;
    }
}

static const purr_driver_t s_driver = {
    .name = "wifi-esp-at", .compatible = "wifi-esp-at",
    .probe = w_probe, .remove = w_remove,
    .bus_abi = PURR_DRIVER_BUS_ABI_VERSION,
    .class_id = PURR_DRV_CLASS_WIFI, .ops = &s_ops,
};

__attribute__((used))
const purr_driver_t *wifi_esp_at_entry(const purr_driver_bus_table_t *bus)
{
    s_bus = bus;
    return &s_driver;
}
