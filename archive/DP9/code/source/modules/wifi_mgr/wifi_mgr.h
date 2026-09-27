#pragma once
// wifi_mgr.h — PURR OS WiFi station-mode manager (public API)
//
// Thin wrapper around esp_wifi_*/esp_event — esp_wifi_init()/esp_netif_init()
// themselves already ran once at boot (kernel_tdp_boot.c), this module only
// drives scan/connect/disconnect and tracks status for the Settings UI.
//
// Also starts SNTP (pool.ntp.org, hardcoded — no per-device server or
// timezone configuration yet, tracked as follow-up UI work) on the first
// successful connection each boot, feeding purr_kernel_time_set() with
// PURR_TIME_SOURCE_NTP. See wifi_mgr.c's start_sntp_once()/on_sntp_synced().

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MGR_IDLE = 0,
    WIFI_MGR_CONNECTING,
    WIFI_MGR_CONNECTED,
    WIFI_MGR_FAILED,
} wifi_mgr_status_t;

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    bool    secured;   // false only for open (no-auth) networks
} wifi_scan_result_t;

int  wifi_mgr_init(void);
void wifi_mgr_deinit(void);

// Blocking scan (a few hundred ms) — fills the internal result list, read
// back via wifi_mgr_scan_count()/wifi_mgr_scan_at().
int  wifi_mgr_scan(void);
int  wifi_mgr_scan_count(void);
bool wifi_mgr_scan_at(int idx, wifi_scan_result_t *out);

// Connects (async — status transitions via the WIFI_EVENT/IP_EVENT handler)
// and, on successful connection, persists ssid/password to NVS for
// auto-reconnect on next boot.
void wifi_mgr_connect(const char *ssid, const char *password);
void wifi_mgr_disconnect(void);

wifi_mgr_status_t wifi_mgr_status(void);
// Empty string if not connected.
const char *wifi_mgr_ip_str(void);

#ifdef __cplusplus
}
#endif
