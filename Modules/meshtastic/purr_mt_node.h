/* purr_mt_node.h - a Meshtastic node: who is on the mesh, what has been seen, what is waiting to go out, and how packets are passed on (flood routing with acknowledgements).
 * Plain C over purr_mt.h. It owns no radio and no clock: the caller feeds it every frame the radio hears (purr_mt_node_rx), asks it what to transmit and when
 * (purr_mt_node_tick), and reads what happened (purr_mt_node_event). Time is the caller's millisecond counter. */
#ifndef PURR_MT_NODE_H
#define PURR_MT_NODE_H

#include "purr_mt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_MT_MAX_CHANNELS 8
#define PURR_MT_MAX_NODES 48
#define PURR_MT_SEEN 48                 /* packets remembered, to tell a repeat from news */
#define PURR_MT_TXQ 8
#define PURR_MT_PENDING 6               /* messages waiting for an acknowledgement */
#define PURR_MT_EVENTS 12
#define PURR_MT_TEXT_MAX 200            /* bytes of one text message */

typedef struct {
    char name[12];                  /* "LongFast", or what the group calls itself */
    uint8_t key[32];
    int8_t keylen;                  /* 0: not encrypted */
    uint8_t hash;                   /* the header byte */
    uint8_t role;                   /* 0 off, 1 primary, 2 secondary */
} purr_mt_channel_t;

typedef struct {
    uint32_t num;
    char long_name[40];
    char short_name[5];
    uint8_t has_key;
    uint8_t public_key[32];
    int8_t snr;                     /* of the last packet from it, dB */
    uint8_t hops_away;              /* 0 heard directly */
    uint8_t has_pos;
    int32_t lat_e7, lon_e7, alt_m;
    uint32_t last_heard_ms;         /* the caller's clock; 0 = never */
    uint8_t channel;                /* the channel it was last heard on */
} purr_mt_nodeinfo_t;

typedef enum {
    PURR_MT_EV_TEXT = 1,            /* a text message arrived */
    PURR_MT_EV_DELIVERED,           /* a message we sent was acknowledged (id) */
    PURR_MT_EV_FAILED,              /* ... or never was, after every retry (id) */
    PURR_MT_EV_HEARD,               /* a broadcast of ours was passed on by someone else: the implicit acknowledgement (id) */
    PURR_MT_EV_NODE,                /* a node appeared or changed (from = its number) */
    PURR_MT_EV_PRIVATE,             /* a packet on the private application port (256): the bytes are in text[0..len) (not text; they may be anything) */
} purr_mt_event_kind_t;

typedef struct {
    int kind;
    uint32_t from, to, id;
    int channel;                    /* index into the channel table */
    int8_t snr;
    int16_t rssi;
    uint8_t pkc;                    /* it came sealed with the public keys */
    uint16_t len;                   /* PURR_MT_EV_PRIVATE: how many bytes of text are the payload */
    char text[PURR_MT_TEXT_MAX + 1];
} purr_mt_event_t;

typedef struct {
    uint32_t my_num;
    char long_name[40];
    char short_name[5];
    uint8_t hop_limit;              /* for what we send: 3 by default */
    uint8_t rebroadcast;            /* 1 pass others' packets on (the CLIENT role), 0 never (CLIENT_MUTE) */
    const purr_mt_region_t *region;
    const purr_mt_preset_t *preset;
    uint32_t (*rand32)(void);
    uint8_t priv[32], pub[32];      /* this node's key pair for direct messages (purr_mt_pkc.h) */
    uint8_t has_pkc;                /* the pair is set: our public key goes out in our node info, and sealed direct messages are read */
    uint8_t pkc_send;               /* send direct messages sealed when the other node's key is known (VERIFY against a real node first; off by default) */
    purr_mt_channel_t channels[PURR_MT_MAX_CHANNELS];
} purr_mt_cfg_t;

typedef struct {
    uint8_t frame[255];
    uint8_t len;
    uint32_t due_ms;
    uint32_t id, from;              /* of the packet inside, so a copy heard from someone else cancels it */
    uint8_t used;
    uint8_t is_relay;
} purr_mt_tx_t;

typedef struct {
    uint32_t id, to;
    uint8_t channel;
    uint8_t want_ack;
    uint8_t tries_left;
    uint32_t retry_ms;              /* when to send it again */
    uint8_t used;
    uint8_t text_len;
    char text[PURR_MT_TEXT_MAX + 1];
} purr_mt_pending_t;

typedef struct {
    purr_mt_cfg_t cfg;
    purr_mt_nodeinfo_t nodes[PURR_MT_MAX_NODES];
    struct { uint32_t from, id, at_ms; } seen[PURR_MT_SEEN];
    int seen_next;
    purr_mt_tx_t txq[PURR_MT_TXQ];
    purr_mt_pending_t pending[PURR_MT_PENDING];
    purr_mt_event_t events[PURR_MT_EVENTS];
    int ev_r, ev_w;
    uint32_t next_id;
    uint32_t next_nodeinfo_ms;
    uint32_t tx_free_ms;            /* the earliest the next frame may start (a pause after each transmission) */
} purr_mt_t;

/* Sets up the node; the channel table is the caller's (hashes are filled in here). */
void purr_mt_node_init(purr_mt_t *m, const purr_mt_cfg_t *cfg, uint32_t now_ms);
/* Fills a channel entry: the stored key (1, 16 or 32 bytes) is expanded and the hash worked out. role 1 is the primary channel. 0, or -1 for a bad key. */
int purr_mt_channel_set(purr_mt_t *m, int index, const char *name, const uint8_t *psk, int psk_len, int role);

/* One frame the radio heard. snr in dB and rssi in dBm are the radio's. */
void purr_mt_node_rx(purr_mt_t *m, uint32_t now_ms, const uint8_t *frame, size_t len, int rssi, int snr);
/* A text to a node (PURR_MT_BROADCAST for a channel) on a channel. A message to one node asks for an acknowledgement and is sent again if none comes. Returns the packet id, or 0. */
uint32_t purr_mt_node_send_text(purr_mt_t *m, uint32_t now_ms, uint32_t to, int channel, const char *text);
/* Sends a packet on any port (the private application port, say) to a node or to everyone, on a channel, encrypted with the channel's key. No acknowledgement is asked for or waited
 * for: whoever needs one asks again. 0 if it was queued, -1 if the channel is not one we have or the payload is too long. */
int purr_mt_node_send_data(purr_mt_t *m, uint32_t now_ms, uint32_t to, int channel, uint32_t portnum, const uint8_t *payload, size_t len);
/* Our own details on the mesh now (otherwise they go out at the start and every three hours). */
void purr_mt_node_announce(purr_mt_t *m, uint32_t now_ms);
/* Asks for a frame to transmit now: 1 and the bytes if one is due, 0 if not. Call it when the radio is idle and the channel is clear. */
int purr_mt_node_tick(purr_mt_t *m, uint32_t now_ms, uint8_t *frame, size_t cap, size_t *len);
/* The airtime of a frame on the current preset, in milliseconds (the node waits this long, plus a margin, before the next one). */
uint32_t purr_mt_airtime_ms(const purr_mt_preset_t *p, size_t len);
/* The next thing that happened: 1 and the event, or 0. */
int purr_mt_node_event(purr_mt_t *m, purr_mt_event_t *ev);

const purr_mt_nodeinfo_t *purr_mt_node_find(const purr_mt_t *m, uint32_t num);
int purr_mt_node_count(const purr_mt_t *m);
const purr_mt_nodeinfo_t *purr_mt_node_at(const purr_mt_t *m, int i);                       /* the i-th known node, i < count */

#ifdef __cplusplus
}
#endif

#endif
