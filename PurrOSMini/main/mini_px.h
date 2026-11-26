/* mini_px.h - the PURR OS Mini's side of the proximity link (plan1.md): the same link manager the T-Deck's driver module runs (Modules/proximity/purr_px_link.c), with the chip's own
 * ESP-NOW and UDP under it and a blob in NVS. It finds hosts, pairs with a six-digit code both people see, and seals the display stream. */
#ifndef MINI_PX_H
#define MINI_PX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "purr_driver_class.h"

/* Starts the link and its task. Call after Wi-Fi is up. */
void mini_px_start(void);

/* The devices in range that serve a screen (paired or not). Returns how many (up to max). */
int mini_px_hosts(purr_link_peer_t *out, int max);
/* An online paired host, if one is in range. */
bool mini_px_paired_host(purr_link_peer_t *out);
bool mini_px_have_paired(void);
bool mini_px_paired_name(char *out, size_t cap);        /* the name of the first paired device, for the menu */

/* Lets a stranger ask to pair for this many seconds (the person at both devices still compares the six digits); 0 closes it. */
void mini_px_open_window(int seconds);
/* Nodes in range that are asking for setup (they say so in their beacon), and a secure session to a paired one: connect, then send and receive messages on a channel. */
int mini_px_setup_nodes(purr_link_peer_t *out, int max);
int mini_px_connect(const uint8_t id[8]);
bool mini_px_session_up(const uint8_t id[8]);
int mini_px_send(const uint8_t id[8], uint8_t channel, const uint8_t *data, size_t len);
int mini_px_recv(const uint8_t id[8], uint8_t *channel, uint8_t *buf, size_t cap);   /* the length, 0 if nothing, < 0 on error */
int mini_px_pair(const uint8_t id[8]);                 /* asks it to pair; 0 or < 0 */
int mini_px_prompt(purr_link_prompt_t *out);           /* 1 while the six digits wait for a yes or no */
int mini_px_answer(int yes);
bool mini_px_is_paired(const uint8_t id[8]);
void mini_px_forget_all(void);
void mini_px_forget(const uint8_t id[8]);              /* drops one pairing (a host that no longer knows us) */

/* A secure stream over a TCP connection to a paired host (the same handshake the host expects). */
int mini_px_stream_start(const uint8_t host_id[8], uint8_t hello1[152]);
int mini_px_stream_finish(int stream, const uint8_t hello2[144]);
int mini_px_stream_seal(int stream, const uint8_t *pt, size_t n, uint8_t *out, size_t cap);
int mini_px_stream_open(int stream, const uint8_t *rec, size_t n, uint8_t *pt, size_t cap);
void mini_px_stream_close(int stream);

#endif
