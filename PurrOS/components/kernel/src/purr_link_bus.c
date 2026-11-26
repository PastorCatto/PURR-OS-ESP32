/* The link services behind bus ABI 10 (see purr_link_bus.h). Random bytes, the radio link and UDP are the shared ESP-IDF side (Modules/proximity/esp/purr_px_esp.c, which the PURR OS Mini
 * links as well); what is here is what is the kernel's own: the blob the module keeps, and the saved networks. */
#include "purr_link_bus.h"

#include <string.h>

#include "esp_wifi.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "purr_net.h"
#include "purr_px_esp.h"

int purr_link_bus_rand(uint8_t *buf, size_t len) { return purr_pxe_rand(buf, len); }
int purr_link_bus_radio_send(const uint8_t *mac, const uint8_t *data, size_t len) { return purr_pxe_radio_send(mac, data, len); }
int purr_link_bus_radio_recv(uint8_t mac[6], int8_t *rssi_dbm, uint8_t *buf, size_t cap) { return purr_pxe_radio_recv(mac, rssi_dbm, buf, cap); }
int purr_link_bus_udp_open(uint16_t port) { return purr_pxe_udp_open(port); }
int purr_link_bus_udp_send(int handle, uint32_t ip, uint16_t port, const uint8_t *data, size_t len) { return purr_pxe_udp_send(handle, ip, port, data, len); }
int purr_link_bus_udp_recv(int handle, uint32_t *ip, uint16_t *port, uint8_t *buf, size_t cap) { return purr_pxe_udp_recv(handle, ip, port, buf, cap); }
void purr_link_bus_udp_close(int handle) { purr_pxe_udp_close(handle); }

/* ---------------------------------------------------------------- the blob the module keeps */

#define STORE_MAX 2048

static int nvs_ready(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        return -1;                              /* the network service owns recovering the store; the link does not erase it */
    }
    return e == ESP_OK ? 0 : -1;
}

int purr_link_bus_store_load(uint8_t *buf, size_t cap)
{
    if (nvs_ready() != 0) {
        return -1;
    }
    nvs_handle_t h;
    if (nvs_open("purrlink", NVS_READONLY, &h) != ESP_OK) {
        return 0;                               /* nothing saved yet */
    }
    size_t n = 0;
    esp_err_t e = nvs_get_blob(h, "blob", NULL, &n);
    int r = 0;
    if (e == ESP_OK && n > 0 && n <= cap && n <= STORE_MAX) {
        r = nvs_get_blob(h, "blob", buf, &n) == ESP_OK ? (int)n : -1;
    } else if (e == ESP_OK && n > cap) {
        r = -1;
    }
    nvs_close(h);
    return r;
}

int purr_link_bus_store_save(const uint8_t *data, size_t len)
{
    if (len == 0 || len > STORE_MAX || nvs_ready() != 0) {
        return -1;
    }
    nvs_handle_t h;
    if (nvs_open("purrlink", NVS_READWRITE, &h) != ESP_OK) {
        return -1;
    }
    esp_err_t e = nvs_set_blob(h, "blob", data, len);
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}

/* ---------------------------------------------------------------- the network */

int purr_link_bus_net_info(uint32_t *ip, char ssid[33], uint8_t *channel)
{
    purr_net_status_t st;
    purr_net_status(&st);
    *ip = 0;
    ssid[0] = '\0';
    *channel = 0;
    if (!st.connected) {
        return 0;
    }
    struct in_addr a;
    if (inet_aton(st.ip, &a)) {
        *ip = a.s_addr;
    }
    strncpy(ssid, st.ssid, 32);
    ssid[32] = '\0';
    *channel = (uint8_t)purr_pxe_radio_channel();
    return 1;
}

int purr_link_bus_net_saved_get(const char *ssid, char pass[65])
{
    return purr_net_saved_pass(ssid, pass);
}

int purr_link_bus_net_saved_add(const char *ssid, const char *pass, const uint8_t peer_id[8])
{
    (void)peer_id;                              /* who gave it is kept by the module (Settings lists it from there); the saved list has no such field */
    return purr_net_save(ssid, pass) == ESP_OK ? 0 : -1;
}
