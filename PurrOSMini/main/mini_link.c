/* mini_link.c - see mini_link.h. */
#include "mini_link.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"

#include "mini_board.h"
#include "mini_gfx.h"
#include "mini_lcd.h"
#include "mini_touch.h"
#include "mini_ui.h"
#include "mini_px.h"
#include "purr_rdisp.h"

static const char *TAG = "mini-link";
#define MAX_PX 4096
#define RX_CAP 16384
#define DEC_CAP (MAX_PX * 2)
#define CORNER_MS 3000

static uint16_t s_seq;

/* ---------------------------------------------------------------- the host */

typedef struct {
    uint8_t id[8];
    struct in_addr addr;
    char name[20];
} host_t;

static bool corner_held(int64_t *since, const mini_touch_t *t)
{
    if (t->down && t->x >= MINI_W - 44 && t->y < 44) {
        if (*since == 0) {
            *since = esp_timer_get_time();
        }
        return (esp_timer_get_time() - *since) / 1000 >= CORNER_MS;
    }
    *since = 0;
    return false;
}

static void host_from(host_t *h, const purr_link_peer_t *p)
{
    memcpy(h->id, p->id, 8);
    h->addr.s_addr = p->ip;
    snprintf(h->name, sizeof(h->name), "%.19s", p->name);
}

bool mini_link_have_last(void)
{
    return mini_px_have_paired();
}

/* ---------------------------------------------------------------- the session */

static int recv_exact(int s, uint8_t *p, int n)
{
    int got = 0;
    while (got < n) {
        int r = recv(s, p + got, n - got, 0);
        if (r <= 0) {
            return -1;
        }
        got += r;
    }
    return 0;
}

static int send_all(int s, const uint8_t *p, int n)
{
    while (n > 0) {
        int r = send(s, p, n, 0);
        if (r <= 0) {
            return -1;
        }
        p += r;
        n -= r;
    }
    return 0;
}

/* Everything after the handshake is sealed by the link, one record per message: a 16-bit length and then the record (the host does the same). */
#define REC_CAP 8400
static int s_stream;
static uint8_t *s_rec, *s_pt, *s_txb;

static int tx_msg(int s, const uint8_t *m, int n)
{
    int rl = mini_px_stream_seal(s_stream, m, (size_t)n, s_txb + 2, 2 + 8 + 128 + 26 + 64 - 2);
    if (rl <= 0) {
        return -1;
    }
    s_txb[0] = (uint8_t)(rl & 0xFF);
    s_txb[1] = (uint8_t)(rl >> 8);
    return send_all(s, s_txb, 2 + rl);
}

static int rx_msg(int s, purr_rd_hdr_t *h, uint8_t *body, int cap)
{
    uint8_t lb[2];
    if (recv_exact(s, lb, 2) != 0) {
        return -1;
    }
    int rl = lb[0] | (lb[1] << 8);
    if (rl < 26 || rl > REC_CAP || recv_exact(s, s_rec, rl) != 0) {
        return -1;
    }
    int pl = mini_px_stream_open(s_stream, s_rec, (size_t)rl, s_pt, REC_CAP);
    if (pl < PURR_RD_HDR_LEN || purr_rd_hdr_get(s_pt, pl, h) < 0 || (int)h->len > cap || PURR_RD_HDR_LEN + (int)h->len != pl) {
        return -1;
    }
    memcpy(body, s_pt + PURR_RD_HDR_LEN, h->len);
    return 0;
}

static int send_msg(int s, uint8_t type, const uint8_t *payload, int len)
{
    uint8_t b[PURR_RD_HDR_LEN + 128];
    if (len > 128) {
        return -1;
    }
    purr_rd_hdr_t h = {type, 0, s_seq++, (uint32_t)len};
    purr_rd_hdr_put(&h, b, sizeof(b));
    if (len > 0) {
        memcpy(b + PURR_RD_HDR_LEN, payload, (size_t)len);
    }
    return tx_msg(s, b, PURR_RD_HDR_LEN + len);
}

static int send_input(int s, int kind, int x, int y)
{
    uint8_t b[PURR_RD_INPUT_LEN];
    purr_rd_input_t m = {(uint8_t)kind, 0, (uint16_t)x, (uint16_t)y};
    purr_rd_input_put(&m, b, sizeof(b));
    return send_msg(s, PURR_RD_T_INPUT, b, sizeof(b));
}

static int session(const host_t *host, int *ran)
{
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        return MINI_LINK_LOST;
    }
    struct timeval tv = {.tv_sec = 4, .tv_usec = 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(PURR_RD_TCP_PORT), .sin_addr = host->addr};
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);                          /* a host that is gone must not hold the screen for the many seconds a plain connect can take */
    int cr = connect(s, (struct sockaddr *)&to, sizeof(to));
    if (cr != 0 && errno == EINPROGRESS) {
        fd_set ws;
        FD_ZERO(&ws);
        FD_SET(s, &ws);
        struct timeval ct = {.tv_sec = 2, .tv_usec = 0};
        int err = 1;
        if (select(s + 1, NULL, &ws, NULL, &ct) > 0) {
            socklen_t el = sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
        }
        cr = err;
    }
    if (cr != 0) {
        close(s);
        return MINI_LINK_LOST;
    }
    fcntl(s, F_SETFL, fl);
    uint8_t *rx = malloc(RX_CAP), *dec = malloc(DEC_CAP);
    s_rec = malloc(REC_CAP);
    s_pt = malloc(REC_CAP);
    s_txb = malloc(2 + 8 + 128 + 26 + 64);
    s_stream = 0;
    int result = MINI_LINK_LOST;
    if (rx == NULL || dec == NULL || s_rec == NULL || s_pt == NULL || s_txb == NULL) {
        goto done;
    }
    {   /* the handshake: "PRS1" and our hello, the host's hello back; it is signed by the keys we paired with */
        uint8_t hi[4 + 152], ho[144];
        memcpy(hi, "PRS1", 4);
        s_stream = mini_px_stream_start(host->id, hi + 4);
        if (s_stream < 1 || send_all(s, hi, sizeof(hi)) != 0 || recv_exact(s, ho, sizeof(ho)) != 0 || mini_px_stream_finish(s_stream, ho) != 0) {
            ESP_LOGW(TAG, "the host did not prove it is the paired one: the pairing is dropped, pair again");
            mini_px_forget(host->id);                            /* a half pairing (the host never saved its side) must not leave this Mini stuck */
            goto done;
        }
    }
    s_seq = 0;
    purr_rd_hello_t hello;
    memset(&hello, 0, sizeof(hello));
    hello.major = PURR_RD_MAJOR;
    hello.minor = PURR_RD_MINOR;
    hello.width = MINI_W;
    hello.height = MINI_H;
    hello.formats = 1 << PURR_RD_FMT_RGB565;
    hello.codecs = PURR_RD_CODEC_BIT(PURR_RD_CODEC_RAW) | PURR_RD_CODEC_BIT(PURR_RD_CODEC_RLE16) | PURR_RD_CODEC_BIT(PURR_RD_CODEC_LZ4);
    hello.caps = PURR_RD_CAP_FILL | PURR_RD_CAP_TOUCH;
    hello.max_px = MAX_PX;
    hello.rx_buf = RX_CAP;
    strlcpy(hello.board, "cyd_24c", sizeof(hello.board));
    uint8_t hb[PURR_RD_HELLO_LEN];
    purr_rd_hello_put(&hello, hb, sizeof(hb));
    uint8_t big[PURR_RD_HDR_LEN + PURR_RD_HELLO_LEN];
    purr_rd_hdr_t hh = {PURR_RD_T_HELLO, 0, s_seq++, PURR_RD_HELLO_LEN};
    purr_rd_hdr_put(&hh, big, sizeof(big));
    memcpy(big + PURR_RD_HDR_LEN, hb, sizeof(hb));
    if (tx_msg(s, big, sizeof(big)) != 0) {
        goto done;
    }
    purr_rd_hdr_t h;
    if (rx_msg(s, &h, rx, RX_CAP) != 0 || h.type != PURR_RD_T_WELCOME || h.len < PURR_RD_WELCOME_LEN) {
        goto done;
    }
    purr_rd_welcome_t wel;
    purr_rd_welcome_get(rx, (int)h.len, &wel);
    if (wel.width != MINI_W || wel.height != MINI_H) {
        mini_ui_status("Remote display", "Wrong screen size", "the host draws a different size");
        vTaskDelay(pdMS_TO_TICKS(3000));
        goto done;
    }
    ESP_LOGI(TAG, "secure session %08x with %s", (unsigned)wel.session, host->name);
    *ran = 1;
    mini_lcd_fill(0, 0, MINI_W, MINI_H, MINI_WHITE);
    mini_touch_t t, last = {0};
    int64_t corner = 0;
    for (;;) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(s, &rs);
        struct timeval wait = {.tv_sec = 0, .tv_usec = 12000};
        int r = select(s + 1, &rs, NULL, NULL, &wait);
        mini_touch_read(&t);
        if (corner_held(&corner, &t)) {
            result = MINI_LINK_MENU;
            send_msg(s, PURR_RD_T_BYE, (const uint8_t[]){0}, 1);
            goto done;
        }
        if (t.down != last.down || (t.down && (t.x != last.x || t.y != last.y))) {
            int kind = !last.down && t.down ? PURR_RD_IN_TOUCH_DOWN : (t.down ? PURR_RD_IN_TOUCH_MOVE : PURR_RD_IN_TOUCH_UP);
            if (send_input(s, kind, t.down ? t.x : last.x, t.down ? t.y : last.y) != 0) {
                goto done;
            }
            last = t;
        }
        if (r < 0) {
            goto done;
        }
        if (r == 0) {
            continue;
        }
        if (rx_msg(s, &h, rx, RX_CAP) != 0) {
            goto done;
        }
        switch (h.type) {
        case PURR_RD_T_RECT: {
            purr_rd_rect_msg_t rm;
            if (purr_rd_rect_get(rx, (int)h.len, &rm) < 0 || rm.fmt != PURR_RD_FMT_RGB565) {
                break;
            }
            int w = rm.r.w, hgt = rm.r.h, n = w * hgt * 2;
            if (w <= 0 || hgt <= 0 || w * hgt > MAX_PX || rm.r.x + w > MINI_W || rm.r.y + hgt > MINI_H) {
                break;                                           /* a rectangle that does not fit is dropped, never half drawn */
            }
            if (purr_rd_decode(rm.codec, rx + PURR_RD_RECT_HEAD, (int)h.len - PURR_RD_RECT_HEAD, dec, n) == 0) {
                mini_lcd_write_le(rm.r.x, rm.r.y, w, hgt, dec);
            }
            break;
        }
        case PURR_RD_T_FILL: {
            purr_rd_fill_t fm;
            if (purr_rd_fill_get(rx, (int)h.len, &fm) >= 0) {
                mini_lcd_fill(fm.r.x, fm.r.y, fm.r.w, fm.r.h, fm.colour);
            }
            break;
        }
        case PURR_RD_T_PRESENT:
            if (h.len >= 4 && send_msg(s, PURR_RD_T_ACK, rx, 4) != 0) {
                goto done;
            }
            break;
        case PURR_RD_T_PING:
            if (h.len >= 4 && send_msg(s, PURR_RD_T_PONG, rx, 4) != 0) {
                goto done;
            }
            break;
        case PURR_RD_T_BYE:
        case PURR_RD_T_ERROR:
            goto done;
        default:
            break;                                               /* a message this version does not know: its payload is already read */
        }
    }
done:
    if (s_stream > 0) {
        mini_px_stream_close(s_stream);
        s_stream = 0;
    }
    free(rx);
    free(dec);
    free(s_rec);
    free(s_pt);
    free(s_txb);
    s_rec = s_pt = s_txb = NULL;
    close(s);
    return result;
}

/* Waits for a paired host to be in range and runs the session. timeout_ms 0 waits until Cancel. */
static int attempt(int timeout_ms, const char *line1, const char *line2)
{
    mini_ui_search(line1, line2);
    int64_t deadline = timeout_ms > 0 ? esp_timer_get_time() + (int64_t)timeout_ms * 1000 : 0;
    if (!mini_px_have_paired()) {
        return MINI_LINK_NOHOST;
    }
    for (;;) {
        purr_link_peer_t p;
        if (mini_px_paired_host(&p) && p.ip != 0) {
            host_t host;
            host_from(&host, &p);
            int ran = 0;
            int r = session(&host, &ran);
            if (ran) {
                return r;
            }
        }
        int x, y;
        if (mini_ui_tap(&x, &y, 400) && mini_ui_cancel_hit(x, y)) {
            return MINI_LINK_NOHOST;
        }
        if (deadline != 0 && esp_timer_get_time() > deadline) {
            return MINI_LINK_NOHOST;
        }
    }
}

int mini_link_reconnect(int timeout_ms)
{
    return attempt(timeout_ms, "Connecting", "to your paired host");
}

/* The pairing flow: hosts in range, tap one; a new one is paired with six digits both people check, then connected. */
static int pair_with(const purr_link_peer_t *p)
{
    if (mini_px_pair(p->id) != 0) {
        return 0;
    }
    mini_ui_status("Pair", "Waiting for the host", "open Settings > Network > Pair a new device");
    int64_t t0 = esp_timer_get_time();
    bool answered = false;
    while ((esp_timer_get_time() - t0) / 1000 < 30000) {
        purr_link_prompt_t pr;
        if (!answered && mini_px_prompt(&pr) == 1 && pr.code >= 0) {
            int a = mini_ui_code(pr.name, pr.code, 500);
            if (a >= 0) {
                mini_px_answer(a);
                answered = true;
                if (!a) {
                    return 0;
                }
                mini_ui_status("Pair", "Checking", "the host is confirming too");
            }
        }
        if (mini_px_is_paired(p->id)) {
            return 1;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    mini_ui_status("Pair", "No answer", "on the host: Settings > Network > Pair a new device, then try again");
    vTaskDelay(pdMS_TO_TICKS(3500));
    return 0;
}

int mini_link_search(void)
{
    mini_px_open_window(120);                               /* while the host list is up, another device may start a pairing with this one */
    for (;;) {
        purr_link_peer_t hosts[7];
        int n = mini_px_hosts(hosts, 7);
        char names[7][20];
        unsigned char paired[7];
        for (int i = 0; i < n; i++) {
            snprintf(names[i], sizeof(names[i]), "%.19s", hosts[i].name);
            paired[i] = hosts[i].paired;
        }
        int pick = mini_ui_hosts("Virtual display", names, paired, n, 1200);
        if (pick == -1) {
            return MINI_LINK_NOHOST;
        }
        if (pick < 0) {
            continue;
        }
        if (!hosts[pick].paired && !pair_with(&hosts[pick])) {
            continue;
        }
        host_t host;
        host_from(&host, &hosts[pick]);
        if (host.addr.s_addr == 0) {
            mini_ui_status("Virtual display", "Not on this network", "the host must be on the same Wi-Fi");
            vTaskDelay(pdMS_TO_TICKS(2500));
            continue;
        }
        int ran = 0;
        int r = session(&host, &ran);
        if (ran) {
            return r;
        }
    }
}

bool mini_link_pair(const purr_link_peer_t *p)
{
    return pair_with(p) != 0;
}

int mini_link_pair_interrupt(void)
{
    purr_link_prompt_t pr;
    if (mini_px_prompt(&pr) != 1 || pr.code < 0) {
        return 0;
    }
    uint8_t id[8];
    char name[20];
    memcpy(id, pr.id, 8);
    snprintf(name, sizeof(name), "%.19s", pr.name);
    for (;;) {
        if (mini_px_prompt(&pr) != 1) {
            break;                                           /* the other end said no, or it timed out */
        }
        int a = mini_ui_code(name, pr.code, 400);
        if (a >= 0) {
            mini_px_answer(a);
            if (a) {
                mini_ui_status("Pair", "Checking", "the other device is confirming too");
                for (int i = 0; i < 80 && !mini_px_is_paired(id); i++) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                if (mini_px_is_paired(id)) {
                    mini_ui_status("Pair", "Paired", name);
                } else {
                    mini_ui_status("Pair", "Not paired", "the other device did not confirm");
                }
                vTaskDelay(pdMS_TO_TICKS(1500));
            }
            break;
        }
    }
    return 1;
}
