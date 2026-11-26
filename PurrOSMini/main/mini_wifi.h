/* mini_wifi.h - Wi-Fi for PURR OS Mini: scan, connect, and the one saved network (NVS). */
#ifndef MINI_WIFI_H
#define MINI_WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} mini_ap_t;

esp_err_t mini_wifi_init(void);
int mini_wifi_scan(mini_ap_t *out, int max);                    /* blocks a few seconds; strongest first, each name once; returns how many */
bool mini_wifi_connect(const char *ssid, const char *pass, int timeout_ms);
bool mini_wifi_saved(char *ssid, size_t scap, char *pass, size_t pcap);
void mini_wifi_save(const char *ssid, const char *pass);
void mini_wifi_forget(void);
bool mini_wifi_up(void);
void mini_wifi_disconnect(void);

#endif
