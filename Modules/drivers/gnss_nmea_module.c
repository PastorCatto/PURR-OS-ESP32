/* GPS and other satellite receivers that speak NMEA 0183 over a UART (the T-Deck Plus's u-blox MIA-M10Q, and most modules sold as "GPS").
 *
 * A driver of class PURR_DRV_CLASS_GNSS (purr_driver_class.h). Its device is configured by named values (purr_kv_cfg_t):
 *     port=1     the UART (1 or 2)                          tx=43  rx=44     the pins (tx may be -1: only listening)
 *     baud=38400 the receiver's speed                       pwr=-1           a power-enable pin that is high for "on" (-1: always on)
 * It reads what the receiver has sent whenever the OS asks (poll() and get_fix()), so nothing runs in the background; the UART's own buffer holds
 * a second or two of sentences between calls. It understands GGA (position, altitude, satellites, HDOP), RMC (validity, speed, course, date and
 * time) and GSV (satellites in view). Sentences with a wrong checksum are dropped. No floating point and no libc beyond what every module has.
 */
#include "purr_driver.h"
#include "purr_driver_bus.h"
#include "purr_driver_class.h"

static const purr_driver_bus_table_t *s_bus;

typedef struct {
    int in_use;
    uint32_t uart;
    int pwr_pin;
    char line[100];
    int len;
    int skipping;                           /* the current line was too long: ignore it up to the next newline */
    purr_gnss_fix_t fix;
    uint64_t fix_us;
} gnss_t;

static gnss_t s_dev[2];

/* ---------------------------------------------------------------- small parsers (fixed point, no libc) */

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* "123.456" with `digits` decimals kept: 123456 for digits=3, 1234 for digits=1 (truncated), 123000 for "123". Empty or bad text gives 0 and returns 0. */
static int dec(const char *s, int digits, int32_t *out)
{
    int neg = 0, got = 0, frac = -1;
    int64_t v = 0;
    if (*s == '-') {
        neg = 1;
        s++;
    }
    for (; *s; s++) {
        if (*s == '.') {
            if (frac >= 0) return 0;
            frac = 0;
            continue;
        }
        if (*s < '0' || *s > '9') return 0;
        got = 1;
        if (frac < 0) {
            v = v * 10 + (*s - '0');
        } else if (frac < digits) {
            v = v * 10 + (*s - '0');
            frac++;
        }
    }
    if (!got) {
        *out = 0;
        return 0;
    }
    if (frac < 0) frac = 0;
    for (; frac < digits; frac++) v *= 10;
    *out = (int32_t)(neg ? -v : v);
    return 1;
}

static int digits2(const char *s)
{
    return (s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9') ? (s[0] - '0') * 10 + (s[1] - '0') : -1;
}

/* "ddmm.mmmm" or "dddmm.mmmm" with its N/S or E/W letter, to degrees x 10^7. */
static int coord(const char *v, const char *hemi, int deg_digits, int32_t *out)
{
    int32_t min_e5;
    if (v[0] == '\0' || hemi[0] == '\0') return 0;
    int deg = 0;
    for (int i = 0; i < deg_digits; i++) {
        if (v[i] < '0' || v[i] > '9') return 0;
        deg = deg * 10 + (v[i] - '0');
    }
    if (!dec(v + deg_digits, 5, &min_e5)) return 0;
    int64_t e7 = (int64_t)deg * 10000000 + (int64_t)min_e5 * 100 / 60;
    if (hemi[0] == 'S' || hemi[0] == 'W') e7 = -e7;
    *out = (int32_t)e7;
    return 1;
}

static void set_time(gnss_t *d, const char *t)
{
    int h = digits2(t), m = t[0] ? digits2(t + 2) : -1, s = t[0] && t[2] ? digits2(t + 4) : -1;
    if (h >= 0 && m >= 0 && s >= 0 && h < 24 && m < 60 && s < 62) {
        d->fix.hour = (uint8_t)h;
        d->fix.minute = (uint8_t)m;
        d->fix.second = (uint8_t)s;
    }
}

/* ---------------------------------------------------------------- one sentence */

static int ends_with(const char *name, const char *tail)       /* "GNGGA" ends with "GGA" */
{
    int n = 0, t = 0;
    while (name[n]) n++;
    while (tail[t]) t++;
    if (n < t) return 0;
    for (int i = 0; i < t; i++) {
        if (name[n - t + i] != tail[i]) return 0;
    }
    return 1;
}

static int handle_sentence(gnss_t *d, char *s)
{
    /* s is "$GNGGA,...*HH" with the newline already removed. Check the checksum when there is one. */
    if (s[0] != '$') return 0;
    char *star = 0;
    uint8_t x = 0;
    int i = 1;
    for (; s[i]; i++) {
        if (s[i] == '*') {
            star = &s[i];
            break;
        }
        x ^= (uint8_t)s[i];
    }
    if (star) {
        int hi = hexv(star[1]), lo = hexv(star[2]);
        if (hi < 0 || lo < 0 || (uint8_t)(hi * 16 + lo) != x) return 0;
        *star = '\0';
    }
    char *f[20];
    int nf = 0;
    f[nf++] = s + 1;
    for (char *p = s + 1; *p && nf < 20; p++) {
        if (*p == ',') {
            *p = '\0';
            f[nf++] = p + 1;
        }
    }
    for (int k = nf; k < 20; k++) f[k] = (char *)"";

    if (ends_with(f[0], "GGA")) {
        int32_t q = 0, sats = 0, hdop = 0, alt = 0, lat, lon;
        dec(f[6], 0, &q);
        dec(f[7], 0, &sats);
        dec(f[8], 1, &hdop);
        dec(f[9], 3, &alt);
        set_time(d, f[1]);
        d->fix.time_valid = d->fix.time_valid || (f[1][0] != '\0' && d->fix.year != 0);
        d->fix.quality = (uint8_t)q;
        d->fix.sats_used = (uint8_t)sats;
        d->fix.hdop_x10 = (uint16_t)hdop;
        if (q > 0 && coord(f[2], f[3], 2, &lat) && coord(f[4], f[5], 3, &lon)) {
            d->fix.lat_e7 = lat;
            d->fix.lon_e7 = lon;
            d->fix.alt_mm = alt;
            d->fix.valid = 1;
        } else if (q == 0) {
            d->fix.valid = 0;
        }
        d->fix_us = s_bus->now_us();
        return 1;
    }
    if (ends_with(f[0], "RMC")) {
        int32_t sp = 0, course = 0, lat, lon;
        int active = f[2][0] == 'A';
        set_time(d, f[1]);
        int dd = digits2(f[9]), mm = f[9][0] ? digits2(f[9] + 2) : -1, yy = f[9][0] && f[9][2] ? digits2(f[9] + 4) : -1;
        if (dd > 0 && mm > 0 && yy >= 0) {
            d->fix.day = (uint8_t)dd;
            d->fix.month = (uint8_t)mm;
            d->fix.year = (uint16_t)(yy >= 80 ? 1900 + yy : 2000 + yy);          /* two digits: GPS began in 1980 */
            d->fix.time_valid = active || d->fix.time_valid;
        }
        if (active && coord(f[3], f[4], 2, &lat) && coord(f[5], f[6], 3, &lon)) {
            dec(f[7], 2, &sp);
            dec(f[8], 2, &course);
            d->fix.lat_e7 = lat;
            d->fix.lon_e7 = lon;
            d->fix.speed_mmps = (uint32_t)((int64_t)sp * 5144 / 1000);        /* knots x 100 to mm/s */
            d->fix.course_cdeg = course;
            d->fix.valid = 1;
        } else if (!active) {
            d->fix.valid = 0;
        }
        d->fix_us = s_bus->now_us();
        return 1;
    }
    if (ends_with(f[0], "GSV")) {
        int32_t view = 0;
        if (dec(f[3], 0, &view)) {
            d->fix.sats_view = (uint8_t)view;
        }
        return 1;
    }
    return 0;
}

static int feed(gnss_t *d, const uint8_t *buf, int n)
{
    int handled = 0;
    for (int i = 0; i < n; i++) {
        char c = (char)buf[i];
        if (c == '\n' || c == '\r') {
            if (d->len > 0 && !d->skipping) {
                d->line[d->len] = '\0';
                handled += handle_sentence(d, d->line);
            }
            d->len = 0;
            d->skipping = 0;
        } else if (d->skipping) {
            continue;
        } else if (d->len < (int)sizeof(d->line) - 1) {
            d->line[d->len++] = c;
        } else {
            d->skipping = 1;                           /* a sentence is at most 82 characters: this is noise */
            d->len = 0;
        }
    }
    return handled;
}

/* ---------------------------------------------------------------- the ops */

static int g_power(void *h, int on)
{
    gnss_t *d = h;
    if (d == NULL || !d->in_use) return -1;
    if (d->pwr_pin >= 0) {
        s_bus->gpio_set_output(d->pwr_pin, on ? 1 : 0);
    }
    return 0;
}

static int g_poll(void *h)
{
    gnss_t *d = h;
    if (d == NULL || !d->in_use) return -1;
    uint8_t buf[128];
    int handled = 0;
    for (int round = 0; round < 8; round++) {                  /* at most ~1 KB per call: the UART holds the rest for the next one */
        int n = s_bus->uart_read(d->uart, buf, sizeof(buf), 0);
        if (n <= 0) break;
        handled += feed(d, buf, n);
    }
    return handled;
}

static int g_get_fix(void *h, purr_gnss_fix_t *out)
{
    gnss_t *d = h;
    if (d == NULL || !d->in_use || out == NULL) return -1;
    g_poll(h);
    *out = d->fix;
    uint64_t age = d->fix_us == 0 ? 0 : (s_bus->now_us() - d->fix_us) / 1000;
    out->age_ms = age > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)age;
    return d->fix.valid ? 1 : 0;
}

/* UBX-CFG-RATE: the measurement period in milliseconds. A receiver that is not a u-blox ignores bytes it does not understand. */
static int g_set_rate(void *h, int hz)
{
    gnss_t *d = h;
    if (d == NULL || !d->in_use || hz < 1 || hz > 10) return -1;
    uint16_t ms = (uint16_t)(1000 / hz);
    uint8_t m[14] = {0xB5, 0x62, 0x06, 0x08, 0x06, 0x00, (uint8_t)(ms & 0xFF), (uint8_t)(ms >> 8), 0x01, 0x00, 0x01, 0x00, 0, 0};
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 12; i++) {                              /* the checksum covers class, id, length and payload */
        a = (uint8_t)(a + m[i]);
        b = (uint8_t)(b + a);
    }
    m[12] = a;
    m[13] = b;
    return s_bus->uart_write(d->uart, m, sizeof(m)) == (int)sizeof(m) ? 0 : -1;
}

static const purr_gnss_ops_t s_ops = {.version = 1, .power = g_power, .poll = g_poll, .get_fix = g_get_fix, .set_rate_hz = g_set_rate};

static int g_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    (void)board;
    const purr_kv_cfg_t *c = (const purr_kv_cfg_t *)dev->cfg;
    if (c == NULL || s_bus->uart_open == NULL) return -1;
    gnss_t *d = NULL;
    for (int i = 0; i < 2; i++) {
        if (!s_dev[i].in_use) {
            d = &s_dev[i];
            break;
        }
    }
    if (d == NULL) return -1;
    int port = purr_kv(c, "port", 1), tx = purr_kv(c, "tx", PURR_PIN_NONE), rx = purr_kv(c, "rx", PURR_PIN_NONE);
    if (rx < 0) return -1;                                       /* it has to be heard */
    uint32_t u = s_bus->uart_open(port, tx, rx, purr_kv(c, "baud", 38400), 2048);
    if (u == 0) return -1;
    for (unsigned i = 0; i < sizeof(*d); i++) ((char *)d)[i] = 0;
    d->in_use = 1;
    d->uart = u;
    d->pwr_pin = purr_kv(c, "pwr", PURR_PIN_NONE);
    if (d->pwr_pin >= 0) {
        s_bus->gpio_set_output(d->pwr_pin, 1);
        s_bus->delay_ms(50);
    }
    *out_handle = d;
    return 0;
}

static void g_remove(void *handle)
{
    gnss_t *d = handle;
    if (d != NULL && d->in_use) {
        s_bus->uart_close(d->uart);
        d->in_use = 0;
    }
}

static const purr_driver_t s_driver = {
    .name = "gnss-nmea", .compatible = "gnss-nmea",
    .probe = g_probe, .remove = g_remove,
    .bus_abi = PURR_DRIVER_BUS_ABI_VERSION,
    .class_id = PURR_DRV_CLASS_GNSS, .ops = &s_ops,
};

__attribute__((used))
const purr_driver_t *gnss_nmea_entry(const purr_driver_bus_table_t *bus)
{
    s_bus = bus;
    return &s_driver;
}
