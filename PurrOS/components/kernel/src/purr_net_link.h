/* purr_net_link.h - a wired network made from a loaded driver of class PURR_DRV_CLASS_ETHERNET (purr_driver_class.h), for the Wi-Fi backend (purr_net_wifi.c).
 *
 * The driver moves raw frames over its bus (a W5500 module, plan4.md); this file puts them into the TCP/IP stack as an esp_netif: one task polls the driver for
 * frames and for the link, sends go straight to the driver, DHCP runs on the netif as on any wired interface. Nothing here knows which chip it is.
 */
#pragma once

#include "esp_err.h"
#include "purr_net.h"

/* 1 when a bound Ethernet driver exists (it may not be brought up yet). */
int purr_netlink_available(void);

/* Brings the wired network up once (netif, receive task); ESP_OK also when it already is. ESP_ERR_NOT_FOUND when no Ethernet driver is bound. */
esp_err_t purr_netlink_up(void);

/* 1 once purr_netlink_up() has succeeded. */
int purr_netlink_active(void);

/* Waits for an address from DHCP. ESP_OK, or ESP_ERR_TIMEOUT. */
esp_err_t purr_netlink_wait_ip(uint32_t timeout_ms);

/* connected = 1 once there is an address; ssid reads "ethernet". */
void purr_netlink_status(purr_net_status_t *out);
