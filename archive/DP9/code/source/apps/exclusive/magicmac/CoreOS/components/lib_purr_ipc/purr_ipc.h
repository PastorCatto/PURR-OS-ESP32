#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// PURR IPC — memory-mapped bridge between the 68k address space and the
// PURR OS kernel. The emulator maps this window at 0xF00000 in 68k space.
// Mac apps write a purr_ipc_cmd_t here; the ESP32 side dispatches and
// writes the response back before setting status = IPC_STATUS_DONE.
// ---------------------------------------------------------------------------

#define PURR_IPC_BASE       0xF00000    // 68k address the Mac app writes to
#define PURR_IPC_WINDOW     0x1000      // 4KB window

// Command IDs
typedef enum {
    IPC_CMD_NOP             = 0x00,

    // LoRa
    IPC_CMD_LORA_SEND       = 0x01,   // Mac → ESP32: send LoRa packet
    IPC_CMD_LORA_RECV       = 0x02,   // ESP32 → Mac: incoming LoRa packet

    // WiFi
    IPC_CMD_WIFI_STATUS     = 0x10,   // Mac → ESP32: get WiFi status
    IPC_CMD_WIFI_SCAN       = 0x11,   // Mac → ESP32: scan networks
    IPC_CMD_WIFI_CONNECT    = 0x12,   // Mac → ESP32: connect to SSID

    // Bluetooth
    IPC_CMD_BT_LIST         = 0x20,   // Mac → ESP32: list paired devices
    IPC_CMD_BT_PAIR         = 0x21,   // Mac → ESP32: pair new device
    IPC_CMD_BT_DISCONNECT   = 0x22,   // Mac → ESP32: disconnect device

    // Notifications
    IPC_CMD_NOTIFY_POST     = 0x30,   // Mac → ESP32: post a notification
    IPC_CMD_NOTIFY_PENDING  = 0x31,   // ESP32 → Mac: deliver pending notification
} purr_ipc_cmd_id_t;

// Status field values
typedef enum {
    IPC_STATUS_IDLE     = 0x00,   // window is free
    IPC_STATUS_PENDING  = 0x01,   // Mac wrote a command, ESP32 not yet dispatched
    IPC_STATUS_BUSY     = 0x02,   // ESP32 is processing
    IPC_STATUS_DONE     = 0x03,   // response written, Mac may read
    IPC_STATUS_ERROR    = 0xFF,
} purr_ipc_status_t;

// Shared command/response struct — lives in the 4KB window
// Keep total size under PURR_IPC_WINDOW
typedef struct __attribute__((packed)) {
    uint8_t  status;        // purr_ipc_status_t — Mac writes PENDING, ESP32 writes DONE
    uint8_t  cmd;           // purr_ipc_cmd_id_t
    uint8_t  seq;           // sequence number (Mac increments each call)
    uint8_t  _pad;
    uint16_t payload_len;   // bytes of valid data in payload[]
    uint16_t response_len;  // bytes of valid data in response[]
    uint8_t  payload[512];  // command arguments (Mac writes)
    uint8_t  response[512]; // return data (ESP32 writes)
} purr_ipc_frame_t;

// ---------------------------------------------------------------------------
// ESP32-side API
// ---------------------------------------------------------------------------

// Call once during shell init. Registers the IPC window with drv_umac so the
// 68k memory bus is wired to this buffer.
void purr_ipc_init(void);

// Poll loop — call from a FreeRTOS task. Blocks on a semaphore signaled by
// drv_umac when the Mac writes IPC_STATUS_PENDING.
void purr_ipc_task(void *arg);

// Deliver an unsolicited notification into the Mac (called from any task).
// The next time the Mac polls IPC_CMD_NOTIFY_PENDING, it receives this.
void purr_ipc_push_notification(const char *text);

// Deliver an incoming LoRa packet into the Mac.
void purr_ipc_push_lora(const uint8_t *data, uint16_t len, int rssi);

#ifdef __cplusplus
}
#endif
