#include "purr_ipc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

// Kernel driver headers (from main CoreOS components)
#include "lora_manager.h"
#include "wifi_manager.h"
#include "bt_manager.h"

static const char *TAG = "lib_purr_ipc";

// The shared window — drv_umac maps this into 68k address space at PURR_IPC_BASE
static purr_ipc_frame_t s_ipc __attribute__((aligned(4)));

// Pending push buffers (ring of 1 for now — extend to queue later)
static struct {
    char    text[128];
    bool    pending;
} s_notify;

static struct {
    uint8_t  data[256];
    uint16_t len;
    int      rssi;
    bool     pending;
} s_lora_rx;

static SemaphoreHandle_t s_sem = NULL;

// ---------------------------------------------------------------------------
// Called by drv_umac when Mac writes IPC_STATUS_PENDING to the window.
// Runs from the umac task context — must be fast, just signal.
// ---------------------------------------------------------------------------
void purr_ipc_mac_wrote(void)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

// ---------------------------------------------------------------------------
// Command dispatch
// ---------------------------------------------------------------------------

static void _handle_lora_send(void)
{
    if (s_ipc.payload_len == 0) return;
    lora_send(s_ipc.payload, s_ipc.payload_len);
    s_ipc.response_len = 0;
}

static void _handle_wifi_status(void)
{
    wifi_status_t st = wifi_manager_get_status();
    memcpy(s_ipc.response, &st, sizeof(st));
    s_ipc.response_len = sizeof(st);
}

static void _handle_wifi_scan(void)
{
    // wifi_manager_scan() fills a JSON string into the response buffer directly
    int n = wifi_manager_scan_json((char *)s_ipc.response, sizeof(s_ipc.response));
    s_ipc.response_len = (uint16_t)n;
}

static void _handle_wifi_connect(void)
{
    // payload: null-terminated SSID, then null-terminated password
    const char *ssid = (const char *)s_ipc.payload;
    const char *pass = ssid + strlen(ssid) + 1;
    wifi_manager_connect(ssid, pass);
    s_ipc.response_len = 0;
}

static void _handle_bt_list(void)
{
    int n = bt_manager_list_json((char *)s_ipc.response, sizeof(s_ipc.response));
    s_ipc.response_len = (uint16_t)n;
}

static void _handle_notify_pending(void)
{
    if (s_notify.pending) {
        size_t len = strlen(s_notify.text) + 1;
        memcpy(s_ipc.response, s_notify.text, len);
        s_ipc.response_len = (uint16_t)len;
        s_notify.pending = false;
    } else if (s_lora_rx.pending) {
        // Prefix with 0x01 tag so Mac app knows it's LoRa, not a text notification
        s_ipc.response[0] = 0x01;
        memcpy(&s_ipc.response[1], &s_lora_rx.rssi, sizeof(int));
        memcpy(&s_ipc.response[1 + sizeof(int)], s_lora_rx.data, s_lora_rx.len);
        s_ipc.response_len = 1 + sizeof(int) + s_lora_rx.len;
        s_lora_rx.pending = false;
    } else {
        s_ipc.response_len = 0;
    }
}

static void _dispatch(void)
{
    s_ipc.status = IPC_STATUS_BUSY;

    switch ((purr_ipc_cmd_id_t)s_ipc.cmd) {
        case IPC_CMD_LORA_SEND:      _handle_lora_send();     break;
        case IPC_CMD_WIFI_STATUS:    _handle_wifi_status();   break;
        case IPC_CMD_WIFI_SCAN:      _handle_wifi_scan();     break;
        case IPC_CMD_WIFI_CONNECT:   _handle_wifi_connect();  break;
        case IPC_CMD_BT_LIST:        _handle_bt_list();       break;
        case IPC_CMD_NOTIFY_PENDING: _handle_notify_pending();break;
        default:
            ESP_LOGW(TAG, "unknown cmd 0x%02x", s_ipc.cmd);
            s_ipc.response_len = 0;
            break;
    }

    s_ipc.status = IPC_STATUS_DONE;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void purr_ipc_init(void)
{
    memset(&s_ipc, 0, sizeof(s_ipc));
    s_sem = xSemaphoreCreateBinary();
    // Register the window buffer with drv_umac so the 68k bus is wired to it
    // umac_register_ipc_window(&s_ipc, PURR_IPC_BASE, sizeof(s_ipc));
    ESP_LOGI(TAG, "IPC window at 68k:0x%06X  host:%p  size:%u",
             PURR_IPC_BASE, &s_ipc, sizeof(s_ipc));
}

void purr_ipc_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "dispatch task running");
    while (1) {
        xSemaphoreTake(s_sem, portMAX_DELAY);
        if (s_ipc.status == IPC_STATUS_PENDING) {
            _dispatch();
        }
    }
}

void purr_ipc_push_notification(const char *text)
{
    strncpy(s_notify.text, text, sizeof(s_notify.text) - 1);
    s_notify.text[sizeof(s_notify.text) - 1] = '\0';
    s_notify.pending = true;
}

void purr_ipc_push_lora(const uint8_t *data, uint16_t len, int rssi)
{
    if (len > sizeof(s_lora_rx.data)) len = sizeof(s_lora_rx.data);
    memcpy(s_lora_rx.data, data, len);
    s_lora_rx.len  = len;
    s_lora_rx.rssi = rssi;
    s_lora_rx.pending = true;
}
