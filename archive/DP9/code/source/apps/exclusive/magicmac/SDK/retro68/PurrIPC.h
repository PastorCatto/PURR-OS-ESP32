/*
 * PurrIPC.h — Mac-side IPC header for MagicMac / PURR OS
 * Include in Retro68 app sources. Targets Mac Plus / System 6.
 */
#ifndef PURR_IPC_H
#define PURR_IPC_H

#include <Types.h>

/* 68k address of the IPC window (must match PURR_IPC_BASE on ESP32 side) */
#define kPurrIPCBase    0x00F00000L

/* Command IDs — keep in sync with purr_ipc.h */
#define kIPCCmdNop              0x00
#define kIPCCmdLoRaSend         0x01
#define kIPCCmdLoRaRecv         0x02
#define kIPCCmdWiFiStatus       0x10
#define kIPCCmdWiFiScan         0x11
#define kIPCCmdWiFiConnect      0x12
#define kIPCCmdBTList           0x20
#define kIPCCmdBTPair           0x21
#define kIPCCmdBTDisconnect     0x22
#define kIPCCmdNotifyPost       0x30
#define kIPCCmdNotifyPending    0x31

/* Status values */
#define kIPCStatusIdle      0x00
#define kIPCStatusPending   0x01
#define kIPCStatusBusy      0x02
#define kIPCStatusDone      0x03
#define kIPCStatusError     0xFF

/* Shared frame layout — must match purr_ipc_frame_t on ESP32 side */
#pragma options align=packed
typedef struct {
    unsigned char  status;
    unsigned char  cmd;
    unsigned char  seq;
    unsigned char  pad;
    unsigned short payloadLen;
    unsigned short responseLen;
    unsigned char  payload[512];
    unsigned char  response[512];
} PurrIPCFrame;
#pragma options align=reset

/* Notification types returned by PurrNotifyPoll */
#define kPurrNotifyNone     0
#define kPurrNotifyText     1
#define kPurrNotifyLoRa     2

typedef struct {
    unsigned char type;           /* kPurrNotifyText or kPurrNotifyLoRa */
    unsigned char data[127];      /* null-terminated text, or raw LoRa bytes */
    short         rssi;           /* valid when type == kPurrNotifyLoRa */
    unsigned char loraLen;
} PurrNotification;

typedef struct {
    unsigned char connected;
    unsigned char ssid[33];
    signed char   rssi;
} PurrWiFiStatus;

/* ---- public API ---- */

/* Send raw bytes over LoRa */
void PurrLoRaSend(const void *data, unsigned short len);

/* Poll for a pending LoRa message or text notification. Returns 1 if one arrived. */
short PurrNotifyPoll(PurrNotification *out);

/* Post a text notification (shown on ESP32 side / other apps) */
void PurrNotifyPost(const char *text);

/* WiFi */
void  PurrWiFiGetStatus(PurrWiFiStatus *out);
void  PurrWiFiConnect(const char *ssid, const char *password);

/* Returns a null-terminated JSON string of scanned networks into buf */
void  PurrWiFiScan(char *buf, unsigned short bufLen);

/* Bluetooth — returns JSON list of paired devices */
void  PurrBTList(char *buf, unsigned short bufLen);

#endif /* PURR_IPC_H */
