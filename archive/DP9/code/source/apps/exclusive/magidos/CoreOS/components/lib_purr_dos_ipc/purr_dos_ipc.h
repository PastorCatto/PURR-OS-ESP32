#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// PURR DOS IPC — INT 0xE0 bridge between the 8086 emulator and PURR OS kernel
//
// DOS app sets AH = command, optional args in other registers / DS:SI buffer,
// then executes INT 0xE0. The emulator intercepts it and calls purr_dos_ipc_dispatch().
// Results are written back into the emulated registers / ES:DI buffer.
//
// Convention mirrors classic DOS INT 21h style: AH=function, carry flag on error.
// ---------------------------------------------------------------------------

// Command codes (AH register value)
typedef enum {
    DOS_IPC_NOP             = 0x00,

    // LoRa
    DOS_IPC_LORA_SEND       = 0x10,   // DS:SI=buf, CX=len
    DOS_IPC_LORA_RECV       = 0x11,   // ES:DI=buf, CX=buflen -> AX=bytes, or CF if none

    // WiFi
    DOS_IPC_WIFI_STATUS     = 0x20,   // ES:DI=PurrDOSWiFiStatus struct
    DOS_IPC_WIFI_SCAN       = 0x21,   // ES:DI=buf, CX=buflen -> JSON null-terminated
    DOS_IPC_WIFI_CONNECT    = 0x22,   // DS:SI=ssid\0pass\0

    // Bluetooth
    DOS_IPC_BT_LIST         = 0x30,   // ES:DI=buf, CX=buflen -> JSON null-terminated
    DOS_IPC_BT_PAIR         = 0x31,   // DS:SI=device_addr (null-terminated)

    // Notifications
    DOS_IPC_NOTIFY_POST     = 0x40,   // DS:SI=text (null-terminated)
    DOS_IPC_NOTIFY_POLL     = 0x41,   // ES:DI=PurrDOSNotif struct -> ZF set if none
} purr_dos_cmd_t;

// Structs written into DOS app memory (little-endian, byte-packed)
typedef struct __attribute__((packed)) {
    uint8_t  connected;
    char     ssid[33];
    int8_t   rssi;
} purr_dos_wifi_status_t;

typedef struct __attribute__((packed)) {
    uint8_t  type;      // 0=none, 1=text, 2=lora
    int8_t   rssi;      // valid when type==2
    uint8_t  len;       // payload length
    uint8_t  data[128];
} purr_dos_notif_t;

// Called by drv_8086 when INT 0xE0 fires.
// mem: pointer to the full 8086 memory array (1MB).
// regs: pointer to the emulated register file (8086tiny layout).
void purr_dos_ipc_dispatch(uint8_t *mem);

// Push an incoming LoRa packet so the next DOS_IPC_LORA_RECV call returns it.
void purr_dos_ipc_push_lora(const uint8_t *data, uint16_t len, int rssi);

// Push a text notification so the next DOS_IPC_NOTIFY_POLL call returns it.
void purr_dos_ipc_push_notification(const char *text);

#ifdef __cplusplus
}
#endif
