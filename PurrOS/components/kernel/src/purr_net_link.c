/* See purr_net_link.h. */
#include "purr_net_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "purr_driver.h"
#include "purr_driver_class.h"

static const char *TAG = "netlink";

#define BIT_GOT_IP     BIT0
#define LINK_POLL_MS   250                         /* how often the link is looked at; frames are polled every pass of the receive loop */
#define RX_WAIT_MS     10

typedef struct {
    esp_netif_driver_base_t base;                  /* first member: esp_netif_attach() hands this pointer back to post_attach */
} link_glue_t;

static link_glue_t s_glue;
static esp_netif_t *s_netif;
static EventGroupHandle_t s_eg;
static const purr_eth_ops_t *s_ops;
static void *s_h;
static volatile uint8_t s_active;
static volatile uint8_t s_link;                    /* the last link state passed to the netif */
static volatile uint8_t s_bringing_up;
static uint8_t s_rx[PURR_ETH_MAX_FRAME];

static int find_driver(void)
{
    void *h = NULL;
    const purr_eth_ops_t *ops = purr_class_ops(PURR_DRV_CLASS_ETHERNET, &h);
    if (ops == NULL || ops->version < 1 || ops->start == NULL || ops->send_frame == NULL || ops->recv_frame == NULL || ops->link == NULL) {
        return 0;
    }
    s_ops = ops;
    s_h = h;
    return 1;
}

int purr_netlink_available(void)
{
    return s_active || find_driver();
}

int purr_netlink_active(void)
{
    return s_active;
}

/* ---------------------------------------------------------------- the netif side */

static esp_err_t link_transmit(void *h, void *buffer, size_t len)
{
    (void)h;
    if (!s_active || len > PURR_ETH_MAX_FRAME) {
        return ESP_FAIL;
    }
    return s_ops->send_frame(s_h, buffer, len) == 0 ? ESP_OK : ESP_FAIL;
}

static void link_free_rx(void *h, void *buffer)
{
    (void)h;
    free(buffer);
}

static esp_err_t link_post_attach(esp_netif_t *netif, void *args)
{
    link_glue_t *glue = args;
    glue->base.netif = netif;
    esp_netif_driver_ifconfig_t ifc = {
        .handle = glue, .transmit = link_transmit, .driver_free_rx_buffer = link_free_rx,
    };
    return esp_netif_set_driver_config(netif, &ifc);
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    ip_event_got_ip_t *ev = data;
    if (ev->esp_netif == s_netif) {
        esp_netif_action_got_ip(ev->esp_netif, base, id, data);
        xEventGroupSetBits(s_eg, BIT_GOT_IP);
    }
}

static void set_link(int up, int bits)
{
    if (up == s_link) {
        return;
    }
    s_link = (uint8_t)up;
    if (up) {
        esp_netif_set_link_speed(s_netif, (bits & PURR_ETH_SPEED_100) ? 100000000 : 10000000);
        esp_netif_action_connected(s_netif, NULL, 0, NULL);
        ESP_LOGI(TAG, "link up, %s %s", (bits & PURR_ETH_SPEED_100) ? "100 Mbit/s" : "10 Mbit/s", (bits & PURR_ETH_FULL) ? "full duplex" : "half duplex");
    } else {
        xEventGroupClearBits(s_eg, BIT_GOT_IP);
        esp_netif_action_disconnected(s_netif, NULL, 0, NULL);
        ESP_LOGW(TAG, "link down");
    }
}

/* One task: frames in, link watched. The driver's recv_frame waits up to RX_WAIT_MS, so this is a poll, as the IDF driver was with no interrupt line. */
static void link_task(void *arg)
{
    (void)arg;
    TickType_t next_link = 0;
    for (;;) {
        int n = s_ops->recv_frame(s_h, s_rx, sizeof(s_rx), RX_WAIT_MS);
        if (n > 0) {
            uint8_t *copy = malloc((size_t)n);                 /* the stack owns this buffer until it calls link_free_rx */
            if (copy != NULL) {
                memcpy(copy, s_rx, (size_t)n);
                /* From here the buffer is the stack's, whatever this returns: esp_netif frees it itself when it refuses a frame (netif not up yet, no pbuf), through
                 * link_free_rx. Freeing it here as well was a double free that corrupted the heap. */
                (void)esp_netif_receive(s_netif, copy, (size_t)n, NULL);
            }
            continue;                                         /* more may be waiting: do not sleep */
        }
        if (n < 0) {
            vTaskDelay(pdMS_TO_TICKS(RX_WAIT_MS));             /* a chip error must not spin the CPU */
        }
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - next_link) >= 0) {
            next_link = now + pdMS_TO_TICKS(LINK_POLL_MS);
            int bits = s_ops->link(s_h);
            set_link(bits > 0 && (bits & PURR_ETH_LINK_UP), bits);
        }
    }
}

esp_err_t purr_netlink_up(void)
{
    if (s_active) {
        return ESP_OK;
    }
    if (!find_driver()) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_bringing_up) {
        return ESP_ERR_INVALID_STATE;                         /* another caller is on it */
    }
    s_bringing_up = 1;
    esp_err_t e = esp_netif_init();
    if (e == ESP_OK) {
        e = esp_event_loop_create_default();
        if (e == ESP_ERR_INVALID_STATE) {
            e = ESP_OK;                                       /* already created is fine */
        }
    }
    if (e != ESP_OK) {
        goto out;
    }
    if (s_eg == NULL) {
        s_eg = xEventGroupCreate();
        if (s_eg == NULL) {
            e = ESP_ERR_NO_MEM;
            goto out;
        }
    }
    /* The station address: the W5500 has none burned in, so derive one from the chip's own, as the IDF-driver backend did. */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_ETH);
    if (s_ops->start(s_h, mac) != 0) {
        ESP_LOGE(TAG, "the Ethernet driver did not start (is the chip answering?)");
        e = ESP_FAIL;
        goto out;
    }
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&cfg);
    if (s_netif == NULL) {
        e = ESP_ERR_NO_MEM;
        goto out;
    }
    s_glue.base.post_attach = link_post_attach;
    e = esp_netif_attach(s_netif, &s_glue);
    if (e != ESP_OK) {
        goto out;
    }
    esp_netif_set_mac(s_netif, mac);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_ip, NULL, NULL);
    esp_netif_action_start(s_netif, NULL, 0, NULL);
    s_active = 1;
    if (xTaskCreate(link_task, "purr_eth", 4096, NULL, 5, NULL) != pdPASS) {
        s_active = 0;
        e = ESP_ERR_NO_MEM;
        goto out;
    }
    ESP_LOGI(TAG, "wired network up through the Ethernet driver");
out:
    s_bringing_up = 0;
    return e;
}

esp_err_t purr_netlink_wait_ip(uint32_t timeout_ms)
{
    if (!s_active) {
        return ESP_ERR_INVALID_STATE;
    }
    EventBits_t bits = xEventGroupWaitBits(s_eg, BIT_GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & BIT_GOT_IP) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void purr_netlink_status(purr_net_status_t *out)
{
    memset(out, 0, sizeof(*out));
    strcpy(out->ip, "0.0.0.0");
    if (!s_active || !(xEventGroupGetBits(s_eg) & BIT_GOT_IP)) {
        return;
    }
    out->connected = 1;
    snprintf(out->ssid, sizeof(out->ssid), "ethernet");
    esp_netif_ip_info_t ip;
    if (s_netif != NULL && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip.ip));
    }
}
