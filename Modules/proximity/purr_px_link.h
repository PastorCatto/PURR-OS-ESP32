/* purr_px_link.h - the proximity link manager (plan1.md, "Proximity rebuild"): the part of the link that decides things. Nearby devices, who may pair with whom, pairing with six
 * digits both people see, secure sessions, secure streams over TCP, a Wi-Fi network handed from one device to another. Plain C over the core in this folder, with every
 * contact with the outside (the radio, UDP, storage, random bytes, the saved networks) behind the small table `purr_pxl_io_t`.
 *
 * Two things link it: the T-Deck's driver module (Modules/drivers/proximity_module.c, io = the driver bus table) and the PURR OS Mini (io = the chip's own ESP-NOW and sockets).
 * It is host-tested against both directions (PurrOS/components/coreos/test/host/test_drv_proximity.c). One instance per program: the io table is kept in a file-level pointer.
 *
 * The functions are the ops of the link driver class (purr_driver_class.h, purr_link_ops_t), with the context as the handle.
 */
#ifndef PURR_PX_LINK_H
#define PURR_PX_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "purr_driver_class.h"
#include "purr_prox.h"
#include "purr_px_pair.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PURR_PXL_SESSIONS
#define PURR_PXL_SESSIONS 2             /* datagram sessions (over the radio or UDP) */
#endif
#define PURR_PXL_STREAMS 2              /* secure streams (over TCP) */

/* what travels in a PAIR frame after the pairing messages (1..4): a session's two hellos; and the first byte of a message on the control channel */
#define HELLO1_TAG        0x10
#define HELLO2_TAG        0x11
#define CTL_WIFI_OFFER    1
#define CTL_WIFI_ACK      2
#define CTL_PING          3

typedef struct {
    const purr_px_ecc_t *ecc;           /* the curve, with random(); its random() is what everything here draws on */
    uint64_t (*now_us)(void);
    int (*radio_send)(const uint8_t *mac, const uint8_t *data, size_t len);                      /* NULL mac: broadcast; < 0 when there is no radio */
    int (*radio_recv)(uint8_t mac[6], int8_t *rssi_dbm, uint8_t *buf, size_t cap);
    int (*udp_open)(uint16_t port);
    int (*udp_send)(int handle, uint32_t ip, uint16_t port, const uint8_t *data, size_t len);
    int (*udp_recv)(int handle, uint32_t *ip, uint16_t *port, uint8_t *buf, size_t cap);
    void (*udp_close)(int handle);
    int (*store_load)(uint8_t *buf, size_t cap);
    int (*store_save)(const uint8_t *data, size_t len);
    int (*net_saved_get)(const char *ssid, char pass[65]);                                       /* may be NULL: the device keeps no networks to share */
    int (*net_saved_add)(const char *ssid, const char *pass, const uint8_t peer_id[8]);          /* may be NULL */
    uint8_t klass;                      /* PURR_PX_CLASS_* */
    uint16_t caps;                      /* PURR_PX_CAP_* */
    int headless;                       /* no screen: nobody can be asked, a close first claim is how it gets an owner */
    const char *name_prefix;            /* "PURR" (the name is prefix-xxxx) */
} purr_pxl_io_t;

typedef struct {
    uint8_t used;
    uint8_t id[8];
    uint8_t via;                    /* PURR_LINK_VIA_* of the last frame heard */
    uint8_t mac[6];
    uint32_t ip;
    uint16_t port;
} pxl_addr_t;

typedef struct {
    uint8_t used;
    uint8_t id[8];
    purr_px_sess_t s;
    purr_px_reasm_t reasm;
    uint16_t next_msg;
    uint8_t in_ch;
    int in_len;
    uint8_t in[PURR_PX_MSG_MAX];
    uint32_t started_ms;
} pxl_sess_t;

typedef struct {
    int in_use;
    uint8_t id[8];
    uint8_t pub[64], priv[32];
    char name[16];
    purr_px_store_t store;
    purr_px_table_t table;
    pxl_addr_t addr[PURR_PX_PEERS];
    purr_px_pair_t pair;
    uint8_t pair_peer[8];
    int pair_auto;                  /* the pairing runs without a person */
    int pair_prompted;
    uint8_t pair_last[PURR_PX_PAIR_MAX_MSG];   /* the last pairing message sent, sent again until answered */
    int pair_last_n, pair_resends;
    uint32_t pair_last_ms;
    uint8_t pair_rx[PURR_PX_PAIR_MAX_MSG];     /* the last one received, to tell a repeat from news */
    int pair_rx_n;
    uint8_t pair_flags;
    int udp;
    uint32_t beacon_counter;
    uint32_t last_beacon_ms;
    uint32_t boot_ms;
    uint32_t pair_window_until;     /* ms; 0: closed */
    int dirty;
    pxl_sess_t sess[PURR_PXL_SESSIONS];
    int notice_pending;
    uint8_t rec[PURR_PX_MSG_MAX];   /* scratch for one whole message: in the context, not on a stack */
    uint8_t pt[PURR_PX_MSG_MAX];
    uint8_t blob[2048];
    purr_px_sess_t stream[PURR_PXL_STREAMS];
    uint8_t stream_used[PURR_PXL_STREAMS];
} purr_pxl_t;


extern const purr_link_ops_t purr_pxl_ops;

/* Starts the link in `L` (the caller's storage): loads or makes the identity, opens UDP. 0, or < 0 (no random source, no key). */
int purr_pxl_init(purr_pxl_t *L, const purr_pxl_io_t *io);
void purr_pxl_shutdown(purr_pxl_t *L);

/* Secure streams: a TCP connection carries the same handshake as a session (the client's HELLO1, the server's HELLO2, 152 and 144 bytes) and then sealed records, one per message.
 * The caller does the sockets and the framing; the keys, the signatures and the counters are here. A stream handle is 1 .. PURR_PXL_STREAMS. */
int purr_pxl_stream_accept(purr_pxl_t *L, const uint8_t *hello1, size_t n, uint8_t *hello2, size_t cap, uint8_t peer_id[8]);   /* the server: a handle, < 0 for a device that is not paired */
int purr_pxl_stream_start(purr_pxl_t *L, const uint8_t server_id[8], uint8_t *hello1, size_t cap);                             /* the client: a handle */
int purr_pxl_stream_finish(purr_pxl_t *L, int stream, const uint8_t *hello2, size_t n);                                         /* 0 once the server is the one meant */
int purr_pxl_stream_seal(purr_pxl_t *L, int stream, uint8_t channel, const uint8_t *pt, size_t n, uint8_t *out, size_t cap);   /* the record's length (n + 26) */
int purr_pxl_stream_open(purr_pxl_t *L, int stream, const uint8_t *rec, size_t n, uint8_t *channel, uint8_t *pt, size_t cap);  /* the plaintext length, < 0 for a record that does not check */
void purr_pxl_stream_close(purr_pxl_t *L, int stream);
int purr_pxl_has_peers(const purr_pxl_t *L);                                                                                    /* any paired device at all */

#ifdef __cplusplus
}
#endif

#endif
