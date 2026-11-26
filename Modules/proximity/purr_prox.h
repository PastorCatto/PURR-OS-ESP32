/*
 * purr_prox.h - the core of the proximity stack (plan1.md "Proximity rebuild"; RemoteDisplay/SPEC.md): the frame every nearby-device message starts with, the beacon, the table of devices that
 * are near right now (with ageing and a "brought close" test from the signal strength), and cutting a message into pieces that fit an ESP-NOW frame and putting it back together.
 *
 * Plain C, no dependencies: the same file is compiled into the kernel's proximity module (S3) and into the Mini (classic ESP32), and tested on the host. The radio, the crypto and the pairing
 * rules sit above it; this is only what travels and what is remembered.
 */
#ifndef PURR_PROX_H
#define PURR_PROX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- frames */

#define PURR_PX_MAGIC 0xA7
#define PURR_PX_VERSION 1
#define PURR_PX_MAX_FRAME 250                   /* an ESP-NOW payload */
#define PURR_PX_HDR 12
#define PURR_PX_MAX_PAYLOAD (PURR_PX_MAX_FRAME - PURR_PX_HDR)

#define PURR_PX_T_BEACON 1
#define PURR_PX_T_PAIR   2
#define PURR_PX_T_SECURE 3                      /* an encrypted channel fragment (above this file) */
#define PURR_PX_T_ACK    4

typedef struct {
    uint8_t type, flags;
    uint8_t id[8];                              /* the sender's device id */
} purr_px_hdr_t;

/* 12 bytes written, or -1 (short buffer). */
int purr_px_hdr_put(const purr_px_hdr_t *h, uint8_t *out, int cap);
/* 12; -1 too short; -2 not ours (magic); -3 a version this code does not speak. */
int purr_px_hdr_get(const uint8_t *in, int n, purr_px_hdr_t *h);

/* ---------------------------------------------------------------- the beacon */

#define PURR_PX_ROLE_SCREEN   (1u << 0)         /* has a screen */
#define PURR_PX_ROLE_HEADLESS (1u << 1)         /* no screen: it can be driven only through the link */
#define PURR_PX_ROLE_PAIRING  (1u << 2)         /* accepting a pairing now */
#define PURR_PX_ROLE_UNOWNED  (1u << 3)         /* factory fresh or reset: the first close claim becomes its owner */
#define PURR_PX_ROLE_DEV      (1u << 4)         /* developer mode is active for this boot */
#define PURR_PX_ROLE_SETUP    (1u << 5)         /* has no account yet and is asking a paired device to run its setup (the remote OOBE) */

#define PURR_PX_CLASS_TDECK   0
#define PURR_PX_CLASS_S3      1                 /* any other S3 host */
#define PURR_PX_CLASS_MINI    2                 /* a PURR OS Mini (CYD) */
#define PURR_PX_CLASS_HELTEC  3
#define PURR_PX_CLASS_VM      4                 /* the emulator */

typedef struct {
    uint8_t roles;
    uint8_t klass;
    uint16_t caps;                              /* what it offers: PURR_PX_CAP_* */
    uint32_t counter;                           /* grows with every beacon: a repeat is not news */
    uint8_t pair_hint[4];                       /* the first bytes of a hash of the devices it is paired with: "you are known to me" without saying who */
    char name[16];
} purr_px_beacon_t;

#define PURR_PX_CAP_DISPLAY_HOST (1u << 0)      /* serves its screen */
#define PURR_PX_CAP_DISPLAY      (1u << 1)      /* can be a remote display */
#define PURR_PX_CAP_SHELL        (1u << 2)      /* a terminal can be opened on it */
#define PURR_PX_CAP_SHARE        (1u << 3)      /* takes shared items (apps, themes, modules, Wi-Fi) */
#define PURR_PX_CAP_RELAY        (1u << 4)      /* relays a mesh radio */

#define PURR_PX_BEACON_LEN 28
int purr_px_beacon_put(const purr_px_beacon_t *b, uint8_t *out, int cap);
int purr_px_beacon_get(const uint8_t *in, int n, purr_px_beacon_t *b);

/* ---------------------------------------------------------------- who is near right now */

#define PURR_PX_PEERS 16

typedef struct {
    uint8_t used;
    uint8_t id[8];
    int8_t rssi;                                /* smoothed: three parts old, one part new */
    uint32_t last_seen_ms;
    uint32_t close_since_ms;
    uint8_t close_since_set;
    purr_px_beacon_t beacon;
} purr_px_peer_t;

typedef struct {
    purr_px_peer_t p[PURR_PX_PEERS];
    int8_t close_dbm;                           /* "close" means at least this strong (smoothed) ... */
    uint32_t close_hold_ms;                     /* ... for at least this long */
} purr_px_table_t;

#define PURR_PX_CLOSE_DBM_DEFAULT (-50)
#define PURR_PX_CLOSE_HOLD_MS_DEFAULT 600
#define PURR_PX_CLOSE_FRESH_MS 1500             /* and heard within this long ago: a device that went quiet is not close */

void purr_px_table_init(purr_px_table_t *t);
/* The "brought close" gesture's threshold, to be tuned on the bench (a stronger signal is nearer). Restarts every device's hold. */
void purr_px_table_set_close(purr_px_table_t *t, int dbm, uint32_t hold_ms);
/* A frame from `id` arrived with this signal strength (dBm, negative). Adds or updates the entry (a beacon, if given, replaces its description); when the table is full the entry that was
 * heard longest ago makes room. Returns the entry. */
purr_px_peer_t *purr_px_table_seen(purr_px_table_t *t, const uint8_t id[8], int rssi, uint32_t now_ms, const purr_px_beacon_t *beacon);
/* Removes what has not been heard for stale_ms. Returns how many went. */
int purr_px_table_prune(purr_px_table_t *t, uint32_t now_ms, uint32_t stale_ms);
int purr_px_table_count(const purr_px_table_t *t);
const purr_px_peer_t *purr_px_table_find(const purr_px_table_t *t, const uint8_t id[8]);
/* "Brought close" (the Apple-style gesture): the smoothed signal has stayed at or above the table's threshold for its hold time, and the device was heard a moment ago. */
int purr_px_peer_is_close(const purr_px_table_t *t, const uint8_t id[8], uint32_t now_ms);
/* The strongest device that is close by that test, or NULL. */
const purr_px_peer_t *purr_px_table_closest(const purr_px_table_t *t, uint32_t now_ms);

/* ---------------------------------------------------------------- messages in pieces */

#define PURR_PX_FRAG_HDR 4                      /* message id (u16), index (u8), count (u8) */
#define PURR_PX_FRAG_PAYLOAD (PURR_PX_MAX_PAYLOAD - PURR_PX_FRAG_HDR)
#define PURR_PX_MSG_MAX 4096
#define PURR_PX_FRAG_MAX 18                     /* ceil(PURR_PX_MSG_MAX / PURR_PX_FRAG_PAYLOAD) */

/* How many pieces a message of len bytes needs (1 for an empty one), or -1 if it is over PURR_PX_MSG_MAX or negative. */
int purr_px_frag_count(int len);
/* Writes piece `index` (its 4-byte header and its share of the message) to out. Returns the bytes written or -1. */
int purr_px_frag_put(uint16_t msg_id, const uint8_t *msg, int len, int index, uint8_t *out, int cap);

typedef struct {
    uint8_t active;
    uint16_t msg_id;
    uint8_t count, got;
    uint32_t mask;                              /* pieces received */
    uint16_t last_len;
    uint32_t started_ms;
    uint16_t len;                               /* the whole message, once complete */
    uint8_t buf[PURR_PX_MSG_MAX];
} purr_px_reasm_t;

void purr_px_reasm_init(purr_px_reasm_t *r);
/* A piece arrived. 1 when the message is now complete (r->buf, r->len), 0 when more are needed (or this one was a repeat), -1 for a piece that cannot be right (bad count or index, a short piece
 * that is not the last, over the limit). A piece of another message restarts the reassembly: the newer message wins. */
int purr_px_reasm_push(purr_px_reasm_t *r, uint32_t now_ms, const uint8_t *piece, int n);
/* Forgets an unfinished message that has been waiting for timeout_ms. 1 if it did. */
int purr_px_reasm_expire(purr_px_reasm_t *r, uint32_t now_ms, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* PURR_PROX_H */
