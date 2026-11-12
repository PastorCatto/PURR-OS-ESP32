#include "purr_net.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "purr_fs.h"
#include "purr_net_link.h"
#include "purr_netrec.h"
#include "purr_vm.h"
#include "purr_wifi.h"

static const char *TAG = "net";
#define WIFI_ETC_PATH "/etc/wifi"
#define BIT_GOT_IP BIT0

static purr_fs_t *s_fs;                 /* NULL, or not mounted: no persistence, still usable */
static purr_wifi_list_t s_saved;
static esp_netif_t *s_netif;
static EventGroupHandle_t s_eg;
static uint8_t s_last_reason;           /* the last WIFI_EVENT_STA_DISCONNECTED reason code */
static volatile uint8_t s_auto;         /* a connection has succeeded once: keep it up */
static uint8_t s_no_radio;             /* running in a VM that has no radio (purr_vm.h): the Wi-Fi stack is never started */
static uint8_t s_wired;                /* ... and a loaded Ethernet driver (purr_net_link.c) is the network instead */
static TickType_t s_wire_retry_at;     /* after a failed bring-up of the wire: not again before this tick */
static volatile uint8_t s_retry_now;    /* it dropped: retry as soon as the task wakes */

/* ---------------------------------------------------------------- the save file */

#define SAVE_BUF_LEN (PURR_WIFI_MAX_SAVED * (PURR_WIFI_SSID_LEN + PURR_WIFI_PASS_LEN + 2))

static int s_saved_bad;                 /* the file exists but could not be read: the list is not saved over it */

static void load_saved(void)
{
    purr_wifi_list_init(&s_saved);
    s_saved_bad = 0;
    if (s_fs == NULL || !purr_fs_mounted(s_fs)) {
        return;
    }
    static char buf[SAVE_BUF_LEN + 1];
    uint32_t n = 0;
    int r = purr_fs_slurp(s_fs, WIFI_ETC_PATH, buf, sizeof(buf), &n);
    if (r == 0) {
        purr_wifi_list_parse(&s_saved, buf, n);
        ESP_LOGI(TAG, "%d saved network(s)", s_saved.count);
    } else if (r < 0) {
        s_saved_bad = 1;
        ESP_LOGE(TAG, "the saved networks could not be read: not saving over them");
    }
}

static void save_list(void)
{
    if (s_fs == NULL || !purr_fs_mounted(s_fs)) {
        return;
    }
    if (s_saved_bad) {
        ESP_LOGW(TAG, "not saving the network list: the saved file could not be read, and saving would overwrite it");
        return;
    }
    static char buf[SAVE_BUF_LEN];
    int n = purr_wifi_list_format(&s_saved, buf, sizeof(buf));
    if (n < 0) {
        return;                                    /* should not happen: the list is bounded */
    }
    purr_fs_mkdir(s_fs, "/etc");                    /* ignored if it already exists */
    purr_fs_write_kept(s_fs, WIFI_ETC_PATH, buf, (uint32_t)n);
}

/* ---------------------------------------------------------------- the radio */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IP_EVENT) {
        xEventGroupSetBits(s_eg, BIT_GOT_IP);
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = data;
        s_last_reason = e->reason;
        ESP_LOGW(TAG, "disconnected, reason %u", (unsigned)e->reason);
        if (s_auto) {
            s_retry_now = 1;                        /* a network that has worked before: chase it */
        }
    }
}

static esp_err_t do_connect(const char *ssid, const char *pass, uint32_t timeout_ms, int save)
{
    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    size_t sl = strlen(ssid);
    if (sl > sizeof(cfg.sta.ssid)) {
        sl = sizeof(cfg.sta.ssid);
    }
    memcpy(cfg.sta.ssid, ssid, sl);
    size_t pl = pass ? strlen(pass) : 0;
    if (pl > sizeof(cfg.sta.password) - 1) {
        pl = sizeof(cfg.sta.password) - 1;
    }
    memcpy(cfg.sta.password, pass ? pass : "", pl);
    cfg.sta.threshold.authmode = pl > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_wifi_disconnect();
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (e != ESP_OK) {
        return e;
    }
    xEventGroupClearBits(s_eg, BIT_GOT_IP);
    e = esp_wifi_connect();
    if (e != ESP_OK) {
        return e;
    }
    EventBits_t bits = xEventGroupWaitBits(s_eg, BIT_GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    if (!(bits & BIT_GOT_IP)) {
        return ESP_ERR_TIMEOUT;
    }
    s_auto = 1;                                   /* it worked: the service now chases this drop */
    s_retry_now = 0;
    if (save) {
        int r = purr_wifi_list_add(&s_saved, ssid, pass ? pass : "");
        if (r == PURR_WIFI_ADDED || r == PURR_WIFI_UPDATED) {
            save_list();
        }
        /* Also the recovery loader's own record: it has no filesystem, so it cannot read
         * /etc/wifi. Every profile that connects keeps this up to date. */
        purr_netrec_save(ssid, pass ? pass : "");
    }
    return ESP_OK;
}

static void reconnect_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        wifi_ap_record_t info;
        if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
            s_retry_now = 0;
            continue;                              /* already connected */
        }
        if (s_retry_now) {
            /* It has worked before: the driver already holds those credentials, so retry them
             * directly rather than waiting on a fresh scan. */
            ESP_LOGI(TAG, "reconnecting");
            if (esp_wifi_connect() == ESP_OK) {
                continue;
            }
        }
        if (s_saved.count == 0) {
            continue;                              /* nothing saved to fall back to */
        }
        purr_net_ap_t seen[16];
        int n = 0;
        if (purr_net_scan(seen, 16, &n) != ESP_OK || n == 0) {
            continue;
        }
        if (n > 16) {
            n = 16;
        }
        purr_wifi_seen_t ws[16];
        for (int i = 0; i < n; i++) {
            snprintf(ws[i].ssid, sizeof(ws[i].ssid), "%s", seen[i].ssid);
            ws[i].rssi = seen[i].rssi;
        }
        int idx = purr_wifi_pick(&s_saved, ws, n);
        if (idx < 0) {
            continue;
        }
        const purr_wifi_net_t *net = purr_wifi_list_find(&s_saved, ws[idx].ssid);
        if (net != NULL) {
            ESP_LOGI(TAG, "reconnecting to %s", net->ssid);
            do_connect(net->ssid, net->pass, 8000, 0);
        }
    }
}

/* In a VM without a radio the network is a loaded Ethernet driver (a W5500 module): it may be loaded after the first call to purr_net_init, so every entry point
 * that matters asks again, and the wire comes up the first time a driver is there. A failed bring-up is not retried for a few seconds. */
static int wired_now(void)
{
    if (s_wired) {
        return 1;
    }
    if (!s_no_radio || !purr_netlink_available()) {
        return 0;
    }
    TickType_t now = xTaskGetTickCount();
    if (s_wire_retry_at != 0 && (int32_t)(now - s_wire_retry_at) < 0) {
        return 0;
    }
    if (purr_netlink_up() == ESP_OK) {
        s_wired = 1;
        return 1;
    }
    s_wire_retry_at = now + pdMS_TO_TICKS(5000);
    if (s_wire_retry_at == 0) {
        s_wire_retry_at = 1;
    }
    return 0;
}

esp_err_t purr_net_init(purr_fs_t *fs)
{
    s_fs = fs;
    /* Any firmware boots in the emulator: the emulator's VM-info device (purr_vm.h) is the flag. It has no radio, and bringing the PHY up there
     * never finishes ("pll_cal exceeds 2ms"), so the Wi-Fi stack is not started and every Wi-Fi call says so instead of touching it. */
    const purr_vm_info_t *vm = purr_vm_info();
    if (vm->present && !(vm->flags & PURR_VM_HAS_RADIO)) {
        s_no_radio = 1;
        if (wired_now()) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "running in a VM with no Wi-Fi radio: the radio is not started. No Ethernet driver is loaded (yet): the wired network starts as soon as one is");
        return ESP_ERR_NOT_SUPPORTED;
    }
    load_saved();

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    if (e != ESP_OK) {
        return e;
    }
    e = esp_netif_init();
    if (e != ESP_OK) {
        return e;
    }
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {   /* already created is fine */
        return e;
    }
    s_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    e = esp_wifi_init(&wc);
    if (e != ESP_OK) {
        return e;
    }
    s_eg = xEventGroupCreate();
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, NULL, NULL);

    e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e != ESP_OK) {
        return e;
    }
    e = esp_wifi_start();
    if (e != ESP_OK) {
        return e;
    }
    xTaskCreate(reconnect_task, "purr_net", 4096, NULL, 4, NULL);
    return ESP_OK;
}

esp_err_t purr_net_scan(purr_net_ap_t *out, int max, int *n)
{
    if (s_no_radio) {
        *n = 0;                                      /* a wire has no access points */
        return wired_now() ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    }
    wifi_scan_config_t sc;
    memset(&sc, 0, sizeof(sc));
    esp_err_t e = esp_wifi_scan_start(&sc, true);      /* blocking */
    if (e != ESP_OK) {
        return e;
    }
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    *n = num;
    if (num == 0) {
        return ESP_OK;
    }
    wifi_ap_record_t *recs = heap_caps_malloc((size_t)num * sizeof(wifi_ap_record_t), MALLOC_CAP_DEFAULT);
    if (recs == NULL) {
        return ESP_ERR_NO_MEM;
    }
    uint16_t got = num;
    e = esp_wifi_scan_get_ap_records(&got, recs);
    if (e != ESP_OK) {
        free(recs);
        return e;
    }
    int m = got < (uint16_t)max ? got : max;
    for (int i = 0; i < m; i++) {
        snprintf(out[i].ssid, sizeof(out[i].ssid), "%s", (const char *)recs[i].ssid);
        out[i].rssi = recs[i].rssi;
        out[i].open = (recs[i].authmode == WIFI_AUTH_OPEN);
    }
    free(recs);
    *n = got;
    return ESP_OK;
}

esp_err_t purr_net_connect(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    if (s_no_radio) {
        return wired_now() ? purr_netlink_wait_ip(timeout_ms) : ESP_ERR_NOT_SUPPORTED;      /* no network to pick and no password on a wire */
    }
    return do_connect(ssid, pass, timeout_ms, 1);
}

esp_err_t purr_net_forget(const char *ssid)
{
    if (s_no_radio) {
        return ESP_OK;
    }
    if (purr_wifi_list_remove(&s_saved, ssid)) {
        save_list();
    }
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK && strcmp((const char *)info.ssid, ssid) == 0) {
        s_auto = 0;
        s_retry_now = 0;
        esp_wifi_disconnect();
    }
    return ESP_OK;
}

void purr_net_status(purr_net_status_t *out)
{
    memset(out, 0, sizeof(*out));
    strcpy(out->ip, "0.0.0.0");
    if (s_no_radio) {
        if (wired_now()) {
            purr_netlink_status(out);
        }
        return;
    }
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) != ESP_OK) {
        return;
    }
    out->connected = 1;
    snprintf(out->ssid, sizeof(out->ssid), "%s", (const char *)info.ssid);
    out->rssi = info.rssi;
    esp_netif_ip_info_t ip;
    if (s_netif != NULL && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip.ip));
    }
}

uint8_t purr_net_last_disconnect_reason(void)
{
    return s_last_reason;
}

int purr_net_wired(void)
{
    if (s_wired) {
        return 1;
    }
    if (!s_no_radio) {
        /* Asked before purr_net_init() has run: the recovery loader decides between "join the wire" and "ask for a Wi-Fi network" first. A virtual device without a
         * radio (purr_vm.h) has no Wi-Fi to ask about, so the answer must not wait for the wire to be up. */
        const purr_vm_info_t *vm = purr_vm_info();
        if (!vm->present || (vm->flags & PURR_VM_HAS_RADIO)) {
            return 0;
        }
        s_no_radio = 1;
    }
    return wired_now() || purr_netlink_available();       /* an Ethernet driver is bound: this is a wired device, up yet or not */
}

int purr_net_saved(purr_net_ap_t *out, int max)
{
    int n = s_saved.count < max ? s_saved.count : max;
    for (int i = 0; i < n; i++) {
        snprintf(out[i].ssid, sizeof(out[i].ssid), "%s", s_saved.nets[i].ssid);
        out[i].rssi = 0;
        out[i].open = (s_saved.nets[i].pass[0] == '\0');
    }
    return s_saved.count;
}
