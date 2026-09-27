/*
 * purr_net.h - the Wi-Fi connection service (Network/SPEC.md section 2).
 *
 * Station mode only. Owns the radio and the saved network list, persisted at /etc/wifi
 * through purr_fs. Reconnects on its own, like a phone: apps (and the shell) only ask
 * it to connect, forget or report status.
 */
#ifndef PURR_NET_H
#define PURR_NET_H

#include <stdint.h>

#include "esp_err.h"

#include "purr_fs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_NET_SSID_LEN 33

typedef struct {
    char ssid[PURR_NET_SSID_LEN];
    int rssi;
    uint8_t open;                  /* no password needed */
} purr_net_ap_t;

typedef struct {
    int connected;
    char ssid[PURR_NET_SSID_LEN];
    int rssi;
    char ip[16];                   /* "0.0.0.0" if not connected */
} purr_net_status_t;

/* Starts the radio in station mode and loads saved networks from /etc/wifi through `fs`.
 * `fs` may be unmounted or NULL: then nothing is saved or loaded, but connecting still
 * works for the session. Safe to call once at boot. */
esp_err_t purr_net_init(purr_fs_t *fs);

/* A blocking scan. `max` is the size of `out`; `*n` gets the count found (may exceed max,
 * capped to it). Returns ESP_OK even when nothing is found. */
esp_err_t purr_net_scan(purr_net_ap_t *out, int max, int *n);

/* Connects, waiting up to timeout_ms for an IP address. Saves the network on success. */
esp_err_t purr_net_connect(const char *ssid, const char *pass, uint32_t timeout_ms);

/* Removes a saved network. Disconnects first if it is the one currently in use. */
esp_err_t purr_net_forget(const char *ssid);

void purr_net_status(purr_net_status_t *out);

/* The reason code (a wifi_err_reason_t value) from the last disconnect, or 0 if there has
 * not been one yet. Mainly for the shell to explain a failed connect. */
uint8_t purr_net_last_disconnect_reason(void);

/* The saved networks, for the shell's "wifi list". */
int purr_net_saved(purr_net_ap_t *out, int max);   /* rssi and open are left at 0 */

#ifdef __cplusplus
}
#endif

#endif /* PURR_NET_H */
