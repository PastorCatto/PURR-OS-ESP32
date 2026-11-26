/* purr_link_bus.h - the link services of the driver bus table (bus ABI 10, purr_driver_bus.h): thin primitives for the proximity driver. The kernel only moves
 * bytes and keeps a blob; every decision is in the driver module (Modules/drivers/proximity_module.c). */
#ifndef PURR_LINK_BUS_H
#define PURR_LINK_BUS_H

#include <stddef.h>
#include <stdint.h>

int purr_link_bus_rand(uint8_t *buf, size_t len);
int purr_link_bus_radio_send(const uint8_t *mac, const uint8_t *data, size_t len);
int purr_link_bus_radio_recv(uint8_t mac[6], int8_t *rssi_dbm, uint8_t *buf, size_t cap);
int purr_link_bus_udp_open(uint16_t port);
int purr_link_bus_udp_send(int handle, uint32_t ip, uint16_t port, const uint8_t *data, size_t len);
int purr_link_bus_udp_recv(int handle, uint32_t *ip, uint16_t *port, uint8_t *buf, size_t cap);
void purr_link_bus_udp_close(int handle);
int purr_link_bus_store_load(uint8_t *buf, size_t cap);
int purr_link_bus_store_save(const uint8_t *data, size_t len);
int purr_link_bus_net_info(uint32_t *ip, char ssid[33], uint8_t *channel);
int purr_link_bus_net_saved_get(const char *ssid, char pass[65]);
int purr_link_bus_net_saved_add(const char *ssid, const char *pass, const uint8_t peer_id[8]);

#endif
