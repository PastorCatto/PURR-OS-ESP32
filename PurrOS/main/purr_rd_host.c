/* purr_rd_host.c - see purr_rd_host.h. */
#include "purr_rd_host.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "purr_display.h"
#include "purr_kernel.h"
#include "purr_link.h"
#include "purr_net.h"
#include "purr_rdisp.h"
#include "purr_settings.h"
#include "purr_vm.h"
#include "purr_ui_service.h"
#include "purr_w10m_priv.h"

static const char *TAG = "rd-host";

#define MAX_PX 4096
#define WINDOW 3
#define FRAME_MS 25                      /* at least this long between frames: redraws that come close together go out as one */

static SemaphoreHandle_t s_mu;
static volatile int s_active;            /* a client is connected: the flush hook copies */
static uint16_t *s_fb;                   /* the shadow of the screen (PSRAM), s_w * s_h */
static int s_w, s_h;
static purr_rd_damage_t s_dmg;
static volatile int s_need_full;         /* a rectangle could not be copied in time: the next frame is the whole screen */

static struct {
    volatile uint8_t down;
    volatile uint16_t x, y;
} s_touch;
static lv_indev_t *s_indev;

/* ---------------------------------------------------------------- the settings key */

/* Off on a real device until the owner turns it on; on by default inside a virtual machine (purr_vm.h: the one runtime flag), where it is a test fixture and nobody's screen. */
static int enabled(void)
{
    return purr_settings_kv_int("remote.display", purr_vm_info()->present ? 1 : 0);
}

int purr_rd_host_enabled(void) { return enabled(); }

void purr_rd_host_set_enabled(int on)
{
    purr_settings_kv_set("remote.display", on ? "1" : "0");
}

static int rd_get(char *o, size_t c)
{
    snprintf(o, c, "%s", enabled() ? "on" : "off");
    return 0;
}

static int rd_set(const char *v, char *e, size_t c)
{
    if (strcmp(v, "on") == 0 || strcmp(v, "1") == 0) {
        return purr_settings_kv_set("remote.display", "1");
    }
    if (strcmp(v, "off") == 0 || strcmp(v, "0") == 0) {
        return purr_settings_kv_set("remote.display", "0");
    }
    snprintf(e, c, "on or off");
    return -1;
}

static const purr_setting_t s_setting = {"remote.display", "serve this screen to a PURR OS Mini on the network (a paired device gets an encrypted stream; with none paired, the plain one)", "on|off", 1, rd_get, rd_set};

/* ---------------------------------------------------------------- the flush hook */

void purr_rd_host_flush(int x, int y, int w, int h, const uint16_t *px)
{
    if (!s_active || s_mu == NULL) {
        return;
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(20)) != pdTRUE) {
        s_need_full = 1;                                         /* the sender is busy copying: do not hold the UI, send everything next time */
        return;
    }
    if (s_fb != NULL && x >= 0 && y >= 0 && w > 0 && h > 0 && x + w <= s_w && y + h <= s_h) {
        for (int r = 0; r < h; r++) {
            memcpy(s_fb + (size_t)(y + r) * s_w + x, px + (size_t)r * w, (size_t)w * 2);
        }
        purr_rd_damage_add(&s_dmg, x, y, w, h);
    }
    xSemaphoreGive(s_mu);
}

/* ---------------------------------------------------------------- the client's touch */

static void remote_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    if (s_active && s_touch.down && !w10m_is_locked()) {         /* a locked screen is display only (RemoteDisplay/SPEC.md section 4) */
        data->point.x = s_touch.x;
        data->point.y = s_touch.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void *make_indev(void *arg)
{
    (void)arg;
    if (s_indev == NULL) {
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, remote_touch_read_cb);
    }
    return NULL;
}

/* ---------------------------------------------------------------- sockets */

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

static uint16_t s_seq;

/* The stream of this session when it is secure (a paired device's, sealed record by record by the link driver), 0 when it is the plain one. A secure message on the wire is
 * a 16-bit length and then the record; the protocol message inside is exactly what the plain stream carries. */
static int s_stream;
static uint8_t *s_txb;                   /* 2 + the longest message + the record's overhead */
static uint8_t *s_rxb;
static uint8_t *s_ptb;
#define SEAL_EXTRA 26
#define TXB_LEN (2 + PURR_RD_HDR_LEN + PURR_RD_RECT_HEAD + MAX_PX * 2 + 16 + SEAL_EXTRA)
#define RXB_LEN (2 + 512 + SEAL_EXTRA)

static int tx_msg(int s, const uint8_t *m, int n)
{
    if (s_stream == 0) {
        return send_all(s, m, n);
    }
    int rl = purr_link_stream_seal(s_stream, 0, m, (size_t)n, s_txb + 2, TXB_LEN - 2);
    if (rl <= 0) {
        return -1;
    }
    s_txb[0] = (uint8_t)(rl & 0xFF);
    s_txb[1] = (uint8_t)(rl >> 8);
    return send_all(s, s_txb, 2 + rl);
}

/* One protocol message from the peer: its header and body (at most 256 bytes of it). */
static int rx_msg(int s, purr_rd_hdr_t *h, uint8_t *body, int cap)
{
    uint8_t hdr[PURR_RD_HDR_LEN];
    if (s_stream == 0) {
        if (recv_exact(s, hdr, sizeof(hdr)) != 0 || purr_rd_hdr_get(hdr, sizeof(hdr), h) < 0 || (int)h->len > cap || recv_exact(s, body, (int)h->len) != 0) {
            return -1;
        }
        return 0;
    }
    uint8_t lb[2];
    if (recv_exact(s, lb, 2) != 0) {
        return -1;
    }
    int rl = lb[0] | (lb[1] << 8);
    if (rl < SEAL_EXTRA || rl > RXB_LEN - 2 || recv_exact(s, s_rxb, rl) != 0) {
        return -1;
    }
    uint8_t ch;
    int pl = purr_link_stream_open(s_stream, s_rxb, (size_t)rl, &ch, s_ptb, RXB_LEN);
    if (pl < PURR_RD_HDR_LEN || purr_rd_hdr_get(s_ptb, pl, h) < 0 || (int)h->len > cap || PURR_RD_HDR_LEN + (int)h->len != pl) {
        return -1;                                          /* did not authenticate, or was not one whole message: the connection ends */
    }
    memcpy(body, s_ptb + PURR_RD_HDR_LEN, h->len);
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

static void devid(uint8_t id[8])
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    memset(id, 0, 8);
    memcpy(id, mac, 6);
}

static void answer_probe(int udp, const struct sockaddr_in *to)
{
    uint8_t pk[40];
    memset(pk, 0, sizeof(pk));
    memcpy(pk, "PRD1", 4);
    pk[4] = 1;                                                   /* kind: announce */
    pk[5] = (uint8_t)(PURR_RD_TCP_PORT & 0xFF);
    pk[6] = (uint8_t)(PURR_RD_TCP_PORT >> 8);
    pk[7] = 1;                                                   /* accepting (the setting is on) */
    devid(pk + 8);
    snprintf((char *)pk + 16, 24, "PURR OS %02X%02X", pk[12], pk[13]);
    sendto(udp, pk, sizeof(pk), 0, (const struct sockaddr *)to, sizeof(*to));
}

/* ---------------------------------------------------------------- one session */

static void session_inner(int cs)
{
    wifi_ps_type_t old_ps = WIFI_PS_MIN_MODEM;
    int ps_set = 0;
    uint8_t body[256];
    purr_rd_hdr_t h;
    purr_display_info_t info;
    const purr_display_v2_t *d = purr_kernel_display();
    if (d == NULL || d->get_info(&info) != ESP_OK) {
        return;
    }
    s_w = info.width;
    s_h = info.height;
    struct timeval tv = {.tv_sec = 3};
    setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    s_seq = 0;

    /* A paired device opens with "PRS1" and the link's hello and gets a secure stream; once any device is paired, nothing else is let in. Until then the plain stream still works. */
    uint8_t first[4];
    if (recv(cs, first, sizeof(first), MSG_PEEK) != (int)sizeof(first)) {
        return;
    }
    if (memcmp(first, "PRS1", 4) == 0) {
        uint8_t hello1[4 + 152], hello2[144], who[8];
        if (recv_exact(cs, hello1, sizeof(hello1)) != 0) {
            return;
        }
        s_stream = purr_link_stream_accept(hello1 + 4, 152, hello2, sizeof(hello2), who);
        if (s_stream < 1 || send_all(cs, hello2, sizeof(hello2)) != 0) {
            ESP_LOGW(TAG, "refused a display that is not paired with this device");
            return;
        }
        s_txb = heap_caps_malloc(TXB_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_rxb = heap_caps_malloc(RXB_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_ptb = heap_caps_malloc(RXB_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_txb == NULL || s_rxb == NULL || s_ptb == NULL) {
            return;
        }
        ESP_LOGI(TAG, "a paired display connected (secure stream)");
    } else if (purr_link_has_peers() > 0) {
        ESP_LOGW(TAG, "refused a plain display: a device is paired with this one, so only paired devices may look");
        return;
    }
    purr_rd_hello_t hello;
    if (rx_msg(cs, &h, body, sizeof(body)) != 0 || h.type != PURR_RD_T_HELLO || h.len < PURR_RD_HELLO_LEN || purr_rd_hello_get(body, (int)h.len, &hello) < 0) {
        return;
    }
    if (hello.major != PURR_RD_MAJOR || hello.width != s_w || hello.height != s_h || !(hello.formats & (1 << PURR_RD_FMT_RGB565))) {
        const uint8_t err[] = {1, 's', 'i', 'z', 'e'};
        send_msg(cs, PURR_RD_T_ERROR, err, sizeof(err));
        ESP_LOGW(TAG, "refused a client: %dx%d (major %d), this screen is %dx%d", hello.width, hello.height, hello.major, s_w, s_h);
        return;
    }
    if (hello.max_px < s_w) {                                    /* a piece smaller than one row of the screen: not worth the thousands of messages it would take */
        const uint8_t err[] = {2, 'b', 'u', 'f'};
        send_msg(cs, PURR_RD_T_ERROR, err, sizeof(err));
        return;
    }
    unsigned codecs = (hello.codecs & 7u) | PURR_RD_CODEC_BIT(PURR_RD_CODEC_RAW);
    int max_px = hello.max_px < MAX_PX ? hello.max_px : MAX_PX;
    ESP_LOGI(TAG, "display '%.15s' %dx%d, codecs %u, up to %d pixels a piece", hello.board, hello.width, hello.height, codecs, max_px);

    purr_rd_welcome_t wel = {PURR_RD_MAJOR, PURR_RD_MINOR, PURR_RD_FMT_RGB565, (uint8_t)codecs, (uint16_t)s_w, (uint16_t)s_h, (uint16_t)max_px, WINDOW, 0, esp_random()};
    uint8_t wb[PURR_RD_WELCOME_LEN];
    purr_rd_welcome_put(&wel, wb, sizeof(wb));
    if (send_msg(cs, PURR_RD_T_WELCOME, wb, sizeof(wb)) != 0) {
        return;
    }

    uint8_t *raw = heap_caps_malloc(MAX_PX * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *out = heap_caps_malloc(PURR_RD_HDR_LEN + PURR_RD_RECT_HEAD + MAX_PX * 2 + 16, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *table = heap_caps_malloc(PURR_RD_LZ4_TABLE * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *fb = heap_caps_malloc((size_t)s_w * s_h * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (raw == NULL || out == NULL || table == NULL || fb == NULL) {
        ESP_LOGW(TAG, "no memory for a session");
        goto done;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    purr_rd_damage_init(&s_dmg, s_w, s_h);
    memset(fb, 0xFF, (size_t)s_w * s_h * 2);
    s_fb = fb;
    s_need_full = 0;
    s_active = 1;
    xSemaphoreGive(s_mu);
    purr_ui_service_run(make_indev, NULL, NULL);
    purr_ui_service_repaint();                                   /* the whole screen is drawn again: every rectangle of it goes through the flush hook into the shadow */

    ps_set = esp_wifi_get_ps(&old_ps) == ESP_OK && esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK;      /* no modem sleep while a display is connected: it wakes in bursts and shows up as lag */
    int inflight = 0;
    uint32_t frame = 0, st_frames = 0, st_bytes = 0, st_stalls = 0;
    int64_t last_frame = 0, st_t0 = esp_timer_get_time();
    for (;;) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(cs, &rs);
        struct timeval wait = {.tv_sec = 0, .tv_usec = 10000};
        int r = select(cs + 1, &rs, NULL, NULL, &wait);
        if (r < 0) {
            break;
        }
        if (r > 0) {
            if (rx_msg(cs, &h, body, sizeof(body)) != 0) {
                break;
            }
            if (h.type == PURR_RD_T_ACK) {
                if (inflight > 0) {
                    inflight--;
                }
            } else if (h.type == PURR_RD_T_INPUT) {
                purr_rd_input_t in;
                if (purr_rd_input_get(body, (int)h.len, &in) >= 0) {
                    if (in.kind == PURR_RD_IN_TOUCH_DOWN || in.kind == PURR_RD_IN_TOUCH_MOVE) {
                        s_touch.x = in.x < s_w ? in.x : s_w - 1;
                        s_touch.y = in.y < s_h ? in.y : s_h - 1;
                        s_touch.down = 1;
                    } else if (in.kind == PURR_RD_IN_TOUCH_UP) {
                        s_touch.down = 0;
                    }
                }
            } else if (h.type == PURR_RD_T_PING && h.len >= 4) {
                if (send_msg(cs, PURR_RD_T_PONG, body, 4) != 0) {
                    break;
                }
            } else if (h.type == PURR_RD_T_BYE || h.type == PURR_RD_T_ERROR) {
                break;
            }
        }
        if (esp_timer_get_time() - st_t0 > 5000000) {            /* how the stream is doing, every five seconds (frames, kilobytes, times the window was full) */
            ESP_LOGI(TAG, "stream: %u frames, %u KB, window full %u times in 5 s", (unsigned)st_frames, (unsigned)(st_bytes / 1024), (unsigned)st_stalls);
            st_frames = st_bytes = st_stalls = 0;
            st_t0 = esp_timer_get_time();
        }
        if (inflight >= WINDOW) {
            st_stalls++;
            continue;
        }
        if ((esp_timer_get_time() - last_frame) / 1000 < FRAME_MS) {
            continue;
        }
        purr_rd_rect_t rects[PURR_RD_DAMAGE_MAX];
        int n;
        xSemaphoreTake(s_mu, portMAX_DELAY);
        if (s_need_full) {
            s_need_full = 0;
            purr_rd_damage_add(&s_dmg, 0, 0, s_w, s_h);
        }
        n = purr_rd_damage_take(&s_dmg, rects, PURR_RD_DAMAGE_MAX);
        xSemaphoreGive(s_mu);
        if (n == 0) {
            continue;
        }
        int failed = 0;
        for (int i = 0; i < n && !failed; i++) {
            purr_rd_rect_t pieces[256];
            int np = purr_rd_split(rects[i], max_px, pieces, 256);
            if (np < 0) {
                np = 0;
                pieces[0] = rects[i];                              /* cannot happen with a 4096 piece on a 320 x 240 screen; send nothing rather than guess */
            }
            for (int k = 0; k < np && !failed; k++) {
                purr_rd_rect_t p = pieces[k];
                int px = p.w * p.h;
                xSemaphoreTake(s_mu, portMAX_DELAY);
                for (int row = 0; row < p.h; row++) {
                    memcpy(raw + (size_t)row * p.w * 2, s_fb + (size_t)(p.y + row) * s_w + p.x, (size_t)p.w * 2);
                }
                xSemaphoreGive(s_mu);
                const uint16_t *pp = (const uint16_t *)raw;
                int flat = 1;
                for (int q = 1; q < px && flat; q++) {
                    flat = pp[q] == pp[0];
                }
                uint8_t *m = out + PURR_RD_HDR_LEN;
                int len, type;
                if (flat) {
                    purr_rd_fill_t f = {p, pp[0]};
                    len = purr_rd_fill_put(&f, m, PURR_RD_FILL_LEN);
                    type = PURR_RD_T_FILL;
                } else {
                    uint8_t codec;
                    int e = purr_rd_encode_best(raw, px * 2, PURR_RD_FMT_RGB565, codecs, m + PURR_RD_RECT_HEAD, px * 2 + 16, table, &codec);
                    if (e < 0) {
                        failed = 1;
                        break;
                    }
                    purr_rd_rect_msg_t rm = {p, codec, PURR_RD_FMT_RGB565};
                    purr_rd_rect_put(&rm, m, PURR_RD_RECT_HEAD);
                    len = PURR_RD_RECT_HEAD + e;
                    type = PURR_RD_T_RECT;
                }
                purr_rd_hdr_t mh = {(uint8_t)type, 0, s_seq++, (uint32_t)len};
                purr_rd_hdr_put(&mh, out, PURR_RD_HDR_LEN);
                if (tx_msg(cs, out, PURR_RD_HDR_LEN + len) != 0) {
                    failed = 1;
                }
                st_bytes += (uint32_t)(PURR_RD_HDR_LEN + len);
            }
        }
        if (failed) {
            break;
        }
        uint8_t pf[4] = {(uint8_t)frame, (uint8_t)(frame >> 8), (uint8_t)(frame >> 16), (uint8_t)(frame >> 24)};
        frame++;
        if (send_msg(cs, PURR_RD_T_PRESENT, pf, 4) != 0) {
            break;
        }
        inflight++;
        st_frames++;
        last_frame = esp_timer_get_time();
    }
done:
    if (ps_set) {
        esp_wifi_set_ps(old_ps);
    }
    s_active = 0;
    s_touch.down = 0;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_fb = NULL;
    xSemaphoreGive(s_mu);
    heap_caps_free(fb);
    heap_caps_free(raw);
    heap_caps_free(out);
    heap_caps_free(table);
    ESP_LOGI(TAG, "session ended");
}

static void session(int cs)
{
    s_stream = 0;
    s_txb = s_rxb = s_ptb = NULL;
    session_inner(cs);
    if (s_stream > 0) {
        purr_link_stream_close(s_stream);
    }
    s_stream = 0;
    heap_caps_free(s_txb);
    heap_caps_free(s_rxb);
    heap_caps_free(s_ptb);
    s_txb = s_rxb = s_ptb = NULL;
}

static void serve(void)
{
    int udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int lis = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (udp < 0 || lis < 0) {
        goto out;
    }
    int one = 1;
    setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(lis, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY)};
    a.sin_port = htons(PURR_RD_DISCOVERY_PORT);
    if (bind(udp, (struct sockaddr *)&a, sizeof(a)) != 0) {
        goto out;
    }
    a.sin_port = htons(PURR_RD_TCP_PORT);
    if (bind(lis, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(lis, 1) != 0) {
        goto out;
    }
    ESP_LOGI(TAG, "listening: discovery %d, display %d", PURR_RD_DISCOVERY_PORT, PURR_RD_TCP_PORT);
    for (;;) {
        purr_net_status_t ns;
        purr_net_status(&ns);
        if (!enabled() || !ns.connected) {
            break;
        }
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(udp, &rs);
        FD_SET(lis, &rs);
        struct timeval wait = {.tv_sec = 0, .tv_usec = 500000};
        int mx = (udp > lis ? udp : lis) + 1;
        if (select(mx, &rs, NULL, NULL, &wait) <= 0) {
            continue;
        }
        if (FD_ISSET(udp, &rs)) {
            uint8_t pk[64];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int n = recvfrom(udp, pk, sizeof(pk), 0, (struct sockaddr *)&from, &fl);
            if (n >= 12 && memcmp(pk, "PRD1", 4) == 0) {
                answer_probe(udp, &from);
            }
        }
        if (FD_ISSET(lis, &rs)) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int cs = accept(lis, (struct sockaddr *)&from, &fl);
            if (cs >= 0) {
                ESP_LOGI(TAG, "a display connected from %s", inet_ntoa(from.sin_addr));
                session(cs);
                close(cs);
            }
        }
    }
out:
    if (udp >= 0) {
        close(udp);
    }
    if (lis >= 0) {
        close(lis);
    }
}

static void host_task(void *arg)
{
    (void)arg;
    for (;;) {
        purr_net_status_t ns;
        purr_net_status(&ns);
        if (enabled() && ns.connected) {
            serve();
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void purr_rd_host_start(void)
{
    if (s_mu != NULL) {
        return;
    }
    s_mu = xSemaphoreCreateMutex();
    purr_settings_register(&s_setting);
    if (xTaskCreateWithCaps(host_task, "rd-host", 8192, NULL, 3, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGW(TAG, "the service task did not start");
    }
}
