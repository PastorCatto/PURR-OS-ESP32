#include "purr_dos_ipc.h"
#include "esp_log.h"
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef PURR_HAS_LORA
#include "lora_manager.h"
#endif
#ifdef PURR_HAS_WIFI
#include "wifi_manager.h"
#endif
#ifdef PURR_HAS_BT
#include "bt_manager.h"
#endif

static const char *TAG = "purr_dos_ipc";

// Pending inbound buffers (single-slot; extend to queue if needed)
static struct { uint8_t data[256]; uint16_t len; int8_t rssi; bool pending; } s_lora;
static struct { char text[128]; bool pending; } s_notif;

// ---------------------------------------------------------------------------
// Helpers to read/write 8086 memory via flat physical address
// seg:off -> (seg << 4) + off
// ---------------------------------------------------------------------------
static inline uint32_t seg_off(uint16_t seg, uint16_t off)
{
    return ((uint32_t)seg << 4) + off;
}

static inline void mem_write_buf(uint8_t *mem, uint16_t seg, uint16_t off,
                                  const void *src, uint16_t len)
{
    memcpy(mem + seg_off(seg, off), src, len);
}

// ---------------------------------------------------------------------------
// Command handlers — receive register values, operate, write results back
// The register file layout matches 8086tiny's internal struct.
// For now we read AH and DS:SI / ES:DI from well-known offsets.
// (Wire to actual 8086tiny regs struct in drv_8086.c once vendored.)
// ---------------------------------------------------------------------------

#ifdef PURR_HAS_LORA
static void _lora_send(uint8_t *mem, uint16_t ds, uint16_t si, uint16_t cx)
{
    uint8_t *payload = mem + seg_off(ds, si);
    lora_manager_send(payload, cx);
}

static uint16_t _lora_recv(uint8_t *mem, uint16_t es, uint16_t di, uint16_t cx)
{
    if (!s_lora.pending) return 0;
    uint16_t n = s_lora.len < cx ? s_lora.len : cx;
    mem_write_buf(mem, es, di, s_lora.data, n);
    s_lora.pending = false;
    return n;
}
#endif  // PURR_HAS_LORA

#ifdef PURR_HAS_WIFI
static void _wifi_status(uint8_t *mem, uint16_t es, uint16_t di)
{
    purr_dos_wifi_status_t out = {0};
    out.connected = wifi_manager_connected() ? 1 : 0;
    wifi_manager_get_ssid(out.ssid, sizeof(out.ssid));
    out.rssi = (int8_t)wifi_manager_rssi();
    mem_write_buf(mem, es, di, &out, sizeof(out));
}

static void _wifi_scan(uint8_t *mem, uint16_t es, uint16_t di, uint16_t cx)
{
    char *buf = (char *)(mem + seg_off(es, di));
    int n = wifi_manager_scan_count();
    size_t pos = 0;
    for (int i = 0; i < n && pos + 40 < cx; i++) {
        char ssid[33] = {0};
        wifi_manager_scan_get_ssid(i, ssid, sizeof(ssid));
        int rssi = wifi_manager_scan_get_rssi(i);
        int written = snprintf(buf + pos, cx - pos, "%s(%d)\n", ssid, rssi);
        if (written > 0) pos += written;
    }
    if (pos < (size_t)cx) buf[pos] = '\0';
}

static void _wifi_connect(uint8_t *mem, uint16_t ds, uint16_t si)
{
    const char *ssid = (const char *)(mem + seg_off(ds, si));
    const char *pass = ssid + strlen(ssid) + 1;
    wifi_manager_connect(ssid, pass);
}
#endif  // PURR_HAS_WIFI

#ifdef PURR_HAS_BT
static void _bt_list(uint8_t *mem, uint16_t es, uint16_t di, uint16_t cx)
{
    char *buf = (char *)(mem + seg_off(es, di));
    int n = bt_manager_paired_count();
    size_t pos = 0;
    for (int i = 0; i < n && pos + 64 < cx; i++) {
        char name[33] = {0};
        char addr[18] = {0};
        bt_manager_get_paired_name(i, name, sizeof(name));
        bt_manager_get_paired_addr(i, addr, sizeof(addr));
        int written = snprintf(buf + pos, cx - pos, "%s %s\n", name, addr);
        if (written > 0) pos += written;
    }
    if (pos < (size_t)cx) buf[pos] = '\0';
}
#endif  // PURR_HAS_BT

static void _notify_post(uint8_t *mem, uint16_t ds, uint16_t si)
{
    const char *text = (const char *)(mem + seg_off(ds, si));
    ESP_LOGI(TAG, "notification from DOS app: %s", text);
    // Could forward to PURR notification system here
}

static void _notify_poll(uint8_t *mem, uint16_t es, uint16_t di)
{
    purr_dos_notif_t out = {0};
    if (s_lora.pending) {
        out.type = 2;
        out.rssi = s_lora.rssi;
        out.len  = s_lora.len > 128 ? 128 : (uint8_t)s_lora.len;
        memcpy(out.data, s_lora.data, out.len);
        s_lora.pending = false;
    } else if (s_notif.pending) {
        out.type = 1;
        out.len  = (uint8_t)strlen(s_notif.text);
        memcpy(out.data, s_notif.text, out.len);
        s_notif.pending = false;
    }
    mem_write_buf(mem, es, di, &out, sizeof(out));
}

// ---------------------------------------------------------------------------
// Main dispatch — called by drv_8086 on INT 0xE0
// Reads AH and register operands from 8086tiny's register file
// ---------------------------------------------------------------------------
void purr_dos_ipc_dispatch(uint8_t *mem)
{
    // Access 8086tiny's register file
    // regs8 and regs16 are declared in 8086tiny.c and exported via drv_8086
    // The register file layout uses 8086tiny's conventions:
    // - regs16[0] = AX, regs8[1] = AH
    // - regs16[11] = DS
    // - regs16[6] = SI, regs16[7] = DI
    // - regs16[8] = ES
    // - regs16[1] = CX

    // Read the command from AH register
    // AH is the high byte of AX: regs8[1] (8086tiny packs AH at regs8[1])
    extern unsigned char regs8[];
    extern unsigned short *regs16;

    uint8_t  ah = regs8[1];         // AH (high byte of AX)
    (void)regs16[0];                // AX unused
    uint16_t ds = regs16[11];       // DS
    uint16_t si = regs16[6];        // SI
    uint16_t es = regs16[8];        // ES
    uint16_t di = regs16[7];        // DI
    uint16_t cx = regs16[1];        // CX

    switch ((purr_dos_cmd_t)ah) {
#ifdef PURR_HAS_LORA
        case DOS_IPC_LORA_SEND:     _lora_send(mem, ds, si, cx);        break;
        case DOS_IPC_LORA_RECV:     _lora_recv(mem, es, di, cx);        break;
#endif
#ifdef PURR_HAS_WIFI
        case DOS_IPC_WIFI_STATUS:   _wifi_status(mem, es, di);          break;
        case DOS_IPC_WIFI_SCAN:     _wifi_scan(mem, es, di, cx);        break;
        case DOS_IPC_WIFI_CONNECT:  _wifi_connect(mem, ds, si);         break;
#endif
#ifdef PURR_HAS_BT
        case DOS_IPC_BT_LIST:       _bt_list(mem, es, di, cx);          break;
#endif
        case DOS_IPC_NOTIFY_POST:   _notify_post(mem, ds, si);          break;
        case DOS_IPC_NOTIFY_POLL:   _notify_poll(mem, es, di);          break;
        default:
            ESP_LOGW(TAG, "unknown DOS IPC cmd 0x%02x", ah);
            break;
    }
}

void purr_dos_ipc_push_lora(const uint8_t *data, uint16_t len, int rssi)
{
    if (len > sizeof(s_lora.data)) len = sizeof(s_lora.data);
    memcpy(s_lora.data, data, len);
    s_lora.len  = len;
    s_lora.rssi = (int8_t)rssi;
    s_lora.pending = true;
}

void purr_dos_ipc_push_notification(const char *text)
{
    strncpy(s_notif.text, text, sizeof(s_notif.text) - 1);
    s_notif.text[sizeof(s_notif.text) - 1] = '\0';
    s_notif.pending = true;
}
