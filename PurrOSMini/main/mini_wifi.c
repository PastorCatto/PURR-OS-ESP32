/* mini_wifi.c - see mini_wifi.h. */
#include "mini_wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "mini-wifi";
#define BIT_GOT_IP (1 << 0)
#define BIT_FAIL   (1 << 1)

static EventGroupHandle_t s_ev;
static int s_retries, s_max_retries;
static bool s_up, s_wanted;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_up = false;
        if (s_wanted && s_retries < s_max_retries) {
            s_retries++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_ev, BIT_FAIL);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_up = true;
        s_retries = 0;
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    }
}

esp_err_t mini_wifi_init(void)
{
    s_ev = xEventGroupCreate();
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK) {
        return e;
    }
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        return e;
    }
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    e = esp_wifi_init(&cfg);
    if (e != ESP_OK) {
        return e;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);                      /* the network is kept by this file, in its own NVS keys, not by the Wi-Fi stack */
    esp_wifi_set_mode(WIFI_MODE_STA);
    return esp_wifi_start();
}

int mini_wifi_scan(mini_ap_t *out, int max)
{
    s_wanted = false;
    esp_wifi_disconnect();
    wifi_scan_config_t sc = {.show_hidden = false};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return 0;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 24) {
        n = 24;
    }
    wifi_ap_record_t recs[24];
    esp_wifi_scan_get_ap_records(&n, recs);                      /* strongest first */
    int count = 0;
    for (int i = 0; i < n && count < max; i++) {
        if (recs[i].ssid[0] == '\0') {
            continue;
        }
        int dup = 0;
        for (int k = 0; k < count; k++) {
            dup |= strcmp(out[k].ssid, (const char *)recs[i].ssid) == 0;
        }
        if (dup) {
            continue;
        }
        strlcpy(out[count].ssid, (const char *)recs[i].ssid, sizeof(out[count].ssid));
        out[count].rssi = recs[i].rssi;
        out[count].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        count++;
    }
    return count;
}

bool mini_wifi_connect(const char *ssid, const char *pass, int timeout_ms)
{
    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass ? pass : "", sizeof(wc.sta.password));
    wc.sta.threshold.authmode = (pass != NULL && pass[0] != '\0') ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    s_retries = 0;
    s_max_retries = 3;
    s_wanted = true;
    xEventGroupClearBits(s_ev, BIT_GOT_IP | BIT_FAIL);
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
    EventBits_t b = xEventGroupWaitBits(s_ev, BIT_GOT_IP | BIT_FAIL, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (!(b & BIT_GOT_IP)) {
        s_wanted = false;
        esp_wifi_disconnect();
        ESP_LOGW(TAG, "could not join %s", ssid);
        return false;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);                               /* no modem sleep: it wakes in bursts and showed up as intermittent lag on the remote display */
    s_max_retries = 1000;                                        /* once on, keep trying to stay on */
    ESP_LOGI(TAG, "joined %s", ssid);
    return true;
}

bool mini_wifi_up(void) { return s_up; }

void mini_wifi_disconnect(void)
{
    s_wanted = false;
    esp_wifi_disconnect();
}

bool mini_wifi_saved(char *ssid, size_t scap, char *pass, size_t pcap)
{
    nvs_handle_t h;
    if (nvs_open("mini", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t s = scap, p = pcap;
    bool ok = nvs_get_str(h, "ssid", ssid, &s) == ESP_OK && ssid[0] != '\0';
    if (ok && nvs_get_str(h, "pass", pass, &p) != ESP_OK) {
        pass[0] = '\0';
    }
    nvs_close(h);
    return ok;
}

void mini_wifi_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open("mini", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", pass ? pass : "");
        nvs_commit(h);
        nvs_close(h);
    }
}

void mini_wifi_forget(void)
{
    nvs_handle_t h;
    if (nvs_open("mini", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "ssid");
        nvs_erase_key(h, "pass");
        nvs_commit(h);
        nvs_close(h);
    }
}
