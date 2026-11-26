/* purr_px_esp.h - the ESP-IDF side of the link's transports, shared by the kernel (purr_link_bus.c, bus ABI 10) and the PURR OS Mini: hardware random bytes, the chip's own radio link
 * (ESP-NOW, with a receive queue and the signal strength of every frame) and UDP over lwIP. Non-blocking: a receive returns 0 when nothing waits. */
#ifndef PURR_PX_ESP_H
#define PURR_PX_ESP_H

#include <stddef.h>
#include <stdint.h>

int purr_pxe_rand(uint8_t *buf, size_t len);
int purr_pxe_radio_send(const uint8_t *mac, const uint8_t *data, size_t len);
int purr_pxe_radio_recv(uint8_t mac[6], int8_t *rssi_dbm, uint8_t *buf, size_t cap);
int purr_pxe_radio_channel(void);                                   /* the channel the radio is on, or 0 */
int purr_pxe_udp_open(uint16_t port);
int purr_pxe_udp_send(int handle, uint32_t ip, uint16_t port, const uint8_t *data, size_t len);
int purr_pxe_udp_recv(int handle, uint32_t *ip, uint16_t *port, uint8_t *buf, size_t cap);
void purr_pxe_udp_close(int handle);

#endif
