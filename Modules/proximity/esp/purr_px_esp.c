/* purr_px_esp.c - see purr_px_esp.h. ESP-NOW is started the first time it is asked for, so a device that never uses the link never touches it. */
#include "purr_px_esp.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "link";

int purr_pxe_rand(uint8_t *buf, size_t len)
{
    esp_fill_random(buf, len);
    return 0;
}

/* ---------------------------------------------------------------- the radio link (ESP-NOW) */

#define RQ_N 24
typedef struct {
    uint8_t mac[6];
    int8_t rssi;
    uint16_t len;
    uint8_t d[250];
} rframe_t;

static rframe_t s_rq[RQ_N];
static volatile unsigned s_rq_r, s_rq_w;
static portMUX_TYPE s_rq_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_radio_state;                       /* 0 not tried, 1 running, -1 not available */
static uint8_t s_peers[16][6];
static int s_peer_count;

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len <= 0 || len > 250 || info == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_rq_mux);
    if (s_rq_w - s_rq_r < RQ_N) {               /* full: the newest is dropped, as on any radio */
        rframe_t *f = &s_rq[s_rq_w % RQ_N];
        memcpy(f->mac, info->src_addr, 6);
        f->rssi = info->rx_ctrl != NULL ? (int8_t)info->rx_ctrl->rssi : -127;
        f->len = (uint16_t)len;
        memcpy(f->d, data, (size_t)len);
        s_rq_w++;
    }
    portEXIT_CRITICAL(&s_rq_mux);
}

static int radio_up(void)
{
    if (s_radio_state != 0) {
        return s_radio_state > 0 ? 0 : -1;
    }
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK) {
        return -1;                              /* the radio is not started yet: asked again on the next call (the driver also has UDP) */
    }
    if (esp_now_init() != ESP_OK || esp_now_register_recv_cb(on_recv) != ESP_OK) {
        ESP_LOGW(TAG, "ESP-NOW did not start");
        s_radio_state = -1;
        return -1;
    }
    esp_now_peer_info_t p;
    memset(&p, 0, sizeof(p));
    memset(p.peer_addr, 0xFF, 6);
    p.channel = 0;                              /* the channel the radio is on */
    p.ifidx = WIFI_IF_STA;
    esp_now_add_peer(&p);
    s_radio_state = 1;
    ESP_LOGI(TAG, "radio link up");
    return 0;
}

int purr_pxe_radio_send(const uint8_t *mac, const uint8_t *data, size_t len)
{
    if (len == 0 || len > 250 || radio_up() != 0) {
        return -1;
    }
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint8_t *to = mac != NULL ? mac : bcast;
    if (mac != NULL && !esp_now_is_peer_exist(to)) {
        esp_now_peer_info_t p;
        memset(&p, 0, sizeof(p));
        memcpy(p.peer_addr, to, 6);
        p.channel = 0;
        p.ifidx = WIFI_IF_STA;
        if (s_peer_count >= 16) {               /* room is made by dropping the oldest one we added */
            esp_now_del_peer(s_peers[0]);
            memmove(s_peers[0], s_peers[1], (size_t)(--s_peer_count) * 6);
        }
        if (esp_now_add_peer(&p) != ESP_OK) {
            return -1;
        }
        memcpy(s_peers[s_peer_count++], to, 6);
    }
    return esp_now_send(to, data, len) == ESP_OK ? 0 : -1;
}

int purr_pxe_radio_recv(uint8_t mac[6], int8_t *rssi_dbm, uint8_t *buf, size_t cap)
{
    if (s_radio_state <= 0 && radio_up() != 0) {
        return 0;
    }
    int n = 0;
    portENTER_CRITICAL(&s_rq_mux);
    if (s_rq_r != s_rq_w) {
        rframe_t *f = &s_rq[s_rq_r % RQ_N];
        if (f->len <= cap) {
            memcpy(mac, f->mac, 6);
            *rssi_dbm = f->rssi;
            memcpy(buf, f->d, f->len);
            n = f->len;
        } else {
            n = -1;
        }
        s_rq_r++;
    }
    portEXIT_CRITICAL(&s_rq_mux);
    return n;
}

/* ---------------------------------------------------------------- UDP */

#define UDP_MAX 3

int purr_pxe_udp_open(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return fd + 1;                              /* 0 is never a handle */
}

int purr_pxe_udp_send(int handle, uint32_t ip, uint16_t port, const uint8_t *data, size_t len)
{
    if (handle <= 0) {
        return -1;
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = ip;                     /* already in network byte order */
    return sendto(handle - 1, data, len, 0, (struct sockaddr *)&a, sizeof(a)) == (int)len ? 0 : -1;
}

int purr_pxe_udp_recv(int handle, uint32_t *ip, uint16_t *port, uint8_t *buf, size_t cap)
{
    if (handle <= 0) {
        return -1;
    }
    struct sockaddr_in a;
    socklen_t al = sizeof(a);
    int n = recvfrom(handle - 1, buf, cap, 0, (struct sockaddr *)&a, &al);
    if (n < 0) {
        return (errno == EWOULDBLOCK || errno == EAGAIN) ? 0 : -1;
    }
    *ip = a.sin_addr.s_addr;
    *port = ntohs(a.sin_port);
    return n;
}

void purr_pxe_udp_close(int handle)
{
    if (handle > 0) {
        close(handle - 1);
    }
}

int purr_pxe_radio_channel(void)
{
    if (s_radio_state <= 0) {
        return 0;
    }
    uint8_t pc;
    wifi_second_chan_t sc;
    return esp_wifi_get_channel(&pc, &sc) == ESP_OK ? pc : 0;
}
