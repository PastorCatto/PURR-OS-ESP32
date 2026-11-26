/*
 * purr_link.h - the OS side of the proximity link (plan1.md, "Proximity rebuild"). The work (nearby devices, pairing, encryption, what may talk to whom) is the driver
 * module Modules/drivers/proximity_module.c; this file only pumps it, asks the person its questions (a pairing shows six digits on both devices), and gives Settings and the
 * remote display a safe way to call it. Every function takes the driver's lock, so any task may call them. With no driver loaded they do nothing and say so.
 */
#ifndef PURR_LINK_H
#define PURR_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "purr_driver_class.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the pump task (its stack in PSRAM). Safe to call once, after the display is up. */
void purr_link_start(void);

/* 1 when the proximity driver is loaded and running. */
int purr_link_present(void);

int purr_link_self(uint8_t id[8], char name[17]);
int purr_link_nearby(purr_link_peer_t *out, int max);
int purr_link_paired(purr_link_peer_t *out, int max);
int purr_link_pair(const uint8_t id[8]);
int purr_link_forget(const uint8_t id[8]);
int purr_link_nets(char ssid[][33], int max);
int purr_link_forget_net(const char *ssid);
int purr_link_option(int opt, int value);                       /* PURR_LINK_OPT_*; value -1000 only reads */
int purr_link_connect(const uint8_t id[8]);
int purr_link_send(const uint8_t id[8], uint8_t channel, const uint8_t *data, size_t len);
int purr_link_recv(const uint8_t id[8], uint8_t *channel, uint8_t *buf, size_t cap);
int purr_link_share_wifi(const uint8_t id[8], const char *ssid);

/* Secure streams (a TCP connection that carries sealed records; purr_driver_class.h, purr_link_ops_t version 2). -1 with no driver. */
int purr_link_has_peers(void);
int purr_link_stream_accept(const uint8_t *hello1, size_t n, uint8_t *hello2, size_t cap, uint8_t peer_id[8]);
int purr_link_stream_seal(int stream, uint8_t channel, const uint8_t *pt, size_t n, uint8_t *out, size_t cap);
int purr_link_stream_open(int stream, const uint8_t *rec, size_t n, uint8_t *channel, uint8_t *pt, size_t cap);
void purr_link_stream_close(int stream);

#ifdef __cplusplus
}
#endif

#endif
