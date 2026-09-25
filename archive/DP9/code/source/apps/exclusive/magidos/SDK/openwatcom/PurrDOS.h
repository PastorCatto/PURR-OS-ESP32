/*
 * PurrDOS.h - PURR OS kernel API for DOS apps running inside MagiDOS
 * Compile with OpenWatcom (16-bit real mode target).
 * Link with PurrDOS.obj (assembled from PurrDOS.asm).
 *
 * All calls use INT 0E0h. Blocking — returns when ESP32 side completes.
 */
#ifndef PURR_DOS_H
#define PURR_DOS_H

typedef unsigned char  BYTE;
typedef unsigned short WORD;

/* WiFi status struct — matches purr_dos_wifi_status_t on ESP32 side */
typedef struct {
    BYTE connected;
    char ssid[33];
    char rssi;          /* signed dBm */
} PurrDOSWiFiStatus;

/* Notification struct — matches purr_dos_notif_t on ESP32 side */
#define PURR_NOTIF_NONE     0
#define PURR_NOTIF_TEXT     1
#define PURR_NOTIF_LORA     2

typedef struct {
    BYTE type;
    char rssi;
    BYTE len;
    BYTE data[128];
} PurrDOSNotif;

/* --- LoRa --- */
void     far PurrLoRaSend(void far *buf, WORD len);
WORD     far PurrLoRaRecv(void far *buf, WORD buflen);  /* returns 0 if no packet */

/* --- WiFi --- */
void     far PurrWiFiStatus(PurrDOSWiFiStatus far *out);
void     far PurrWiFiConnect(const char far *ssid_then_pass); /* "ssid\0pass\0" */

/* --- Notifications --- */
void         PurrNotifyPost(const char far *text);
BYTE     far PurrNotifyPoll(PurrDOSNotif far *out);     /* returns 1 if something arrived */

#endif /* PURR_DOS_H */
