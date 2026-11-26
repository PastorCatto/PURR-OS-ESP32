/* purr_mt.h - the Meshtastic protocol core (plan9.md, "Meshtastic service"): plain C, no libc, no radio, no clock of its own. It knows the air format (the 16 byte header and the
 * AES-CTR payload), channels and their keys, the protobuf messages a text client needs, the regions and modem presets, and (purr_mt_node.h) who is on the mesh and how packets are
 * passed on. A radio driver (purr_lora_ops_t, any chip) moves the frames and takes the modem settings this file computes; nothing here is specific to a chip.
 *
 * Written from the published protocol (the firmware's source and documentation); the host tests (test_mt.c) check it against the AES standard's vectors and the values a default
 * device is known to use. Any number below that came from memory of the firmware is marked VERIFY and is the first thing to check against a real node.
 */
#ifndef PURR_MT_H
#define PURR_MT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- AES (FIPS 197), encrypt direction only: CTR needs nothing else */

typedef struct {
    uint32_t rk[60];
    int rounds;                 /* 10 for a 128 bit key, 14 for 256 */
} purr_mt_aes_t;

int purr_mt_aes_init(purr_mt_aes_t *a, const uint8_t *key, int keylen);                  /* keylen 16 or 32; 0, or -1 for any other */
void purr_mt_aes_block(const purr_mt_aes_t *a, const uint8_t in[16], uint8_t out[16]);
/* CTR mode as the firmware uses it: the last 4 bytes of the 16 byte counter block are a big-endian counter. data is changed in place. */
void purr_mt_ctr(const purr_mt_aes_t *a, const uint8_t iv[16], uint8_t *data, size_t len);

/* ---------------------------------------------------------------- the air format */

#define PURR_MT_HDR_LEN 16
#define PURR_MT_MAX_PAYLOAD 237                 /* 255 bytes of LoRa payload less the header (VERIFY: the firmware's own limit) */
#define PURR_MT_BROADCAST 0xFFFFFFFFu

typedef struct {
    uint32_t to;                    /* PURR_MT_BROADCAST for everyone */
    uint32_t from;                  /* the sender's node number */
    uint32_t id;                    /* the packet id, chosen by the sender */
    uint8_t hop_limit;              /* 0..7: hops left */
    uint8_t want_ack;
    uint8_t via_mqtt;
    uint8_t hop_start;              /* 0..7: the hop limit the sender started with */
    uint8_t channel;                /* the channel hash (one byte) for an encrypted packet */
    uint8_t next_hop;               /* the node the sender asks to pass it on (0 = anyone); newer firmware, 0 on older */
    uint8_t relay_node;             /* the last byte of the node that sent it on last */
} purr_mt_hdr_t;

int purr_mt_hdr_put(const purr_mt_hdr_t *h, uint8_t out[PURR_MT_HDR_LEN]);
int purr_mt_hdr_get(const uint8_t *in, size_t n, purr_mt_hdr_t *h);                      /* PURR_MT_HDR_LEN, or -1 for a frame that is too short */

/* ---------------------------------------------------------------- channels and keys */

#define PURR_MT_DEFAULT_PSK_LEN 16
extern const uint8_t purr_mt_default_psk[PURR_MT_DEFAULT_PSK_LEN];

/* A channel's key as it is stored (1, 16 or 32 bytes, or none) expanded to what AES needs. 1 byte means "the default key, number n" (1 is the default itself; 2 and up change its last
 * byte); a short key is padded with zeros to 16 or 32. Returns the expanded length (0 = the channel is not encrypted), or -1 for a key longer than 32. */
int purr_mt_expand_psk(const uint8_t *psk, int psk_len, uint8_t out[32]);
/* The one byte hash in the header: the xor of the channel's name bytes and the xor of its expanded key. */
uint8_t purr_mt_channel_hash(const char *name, const uint8_t *key, int keylen);

/* Encrypts or decrypts (the same operation) a payload in place. key/keylen from purr_mt_expand_psk; keylen 0 leaves it alone. */
void purr_mt_crypt(const uint8_t *key, int keylen, uint32_t from, uint32_t id, uint8_t *data, size_t len);

/* ---------------------------------------------------------------- protobuf, as much as a text client needs */

typedef struct {
    uint8_t *buf;
    size_t cap, len;
    int err;                    /* set when something did not fit */
} purr_mt_pb_w_t;

void purr_mt_pb_w_init(purr_mt_pb_w_t *w, uint8_t *buf, size_t cap);
void purr_mt_pb_varint(purr_mt_pb_w_t *w, int field, uint64_t v);                         /* wire type 0; a zero value is written too (the caller leaves out defaults) */
void purr_mt_pb_fixed32(purr_mt_pb_w_t *w, int field, uint32_t v);                        /* wire type 5 */
void purr_mt_pb_bytes(purr_mt_pb_w_t *w, int field, const uint8_t *p, size_t n);          /* wire type 2 */

typedef struct {
    const uint8_t *p;
    size_t n;
    int err;
} purr_mt_pb_r_t;

typedef struct {
    int field, wire;
    uint64_t v;                 /* varint or fixed value */
    const uint8_t *data;        /* wire type 2 */
    size_t len;
} purr_mt_pb_field_t;

void purr_mt_pb_r_init(purr_mt_pb_r_t *r, const uint8_t *p, size_t n);
int purr_mt_pb_next(purr_mt_pb_r_t *r, purr_mt_pb_field_t *f);                            /* 1 a field, 0 the end, -1 malformed */

/* ---------------------------------------------------------------- the messages */

#define PURR_MT_PORT_TEXT 1
#define PURR_MT_PORT_POSITION 3
#define PURR_MT_PORT_NODEINFO 4
#define PURR_MT_PORT_ROUTING 5
#define PURR_MT_PORT_ADMIN 6
#define PURR_MT_PORT_TELEMETRY 67
#define PURR_MT_PORT_TRACEROUTE 70

typedef struct {
    uint32_t portnum;
    const uint8_t *payload;
    size_t payload_len;
    uint8_t want_response;
    uint32_t dest, source;          /* 0 = absent */
    uint32_t request_id, reply_id;  /* 0 = absent */
    uint32_t emoji;
    uint32_t bitfield;
    uint8_t has_bitfield;
} purr_mt_data_t;

int purr_mt_data_put(const purr_mt_data_t *d, uint8_t *out, size_t cap);                  /* bytes written, or -1 */
int purr_mt_data_get(const uint8_t *in, size_t n, purr_mt_data_t *d);                     /* 0, or -1 */

typedef struct {
    char id[16];                    /* "!aabbccdd" */
    char long_name[40];
    char short_name[5];
    uint32_t hw_model;
    uint32_t role;                  /* CLIENT = 0 */
    uint8_t public_key[32];
    uint8_t has_key;
} purr_mt_user_t;

int purr_mt_user_put(const purr_mt_user_t *u, uint8_t *out, size_t cap);
int purr_mt_user_get(const uint8_t *in, size_t n, purr_mt_user_t *u);

typedef struct {
    int32_t lat_e7, lon_e7;         /* degrees times 1e7 */
    int32_t alt_m;
    uint32_t time;                  /* seconds since 1970, 0 if unknown */
    uint8_t has_alt;
} purr_mt_pos_t;

int purr_mt_pos_put(const purr_mt_pos_t *p, uint8_t *out, size_t cap);
int purr_mt_pos_get(const uint8_t *in, size_t n, purr_mt_pos_t *p);

/* A Routing message carrying only an error code (0 = an acknowledgement). */
int purr_mt_routing_put(uint32_t error_reason, uint8_t *out, size_t cap);
int purr_mt_routing_get(const uint8_t *in, size_t n, uint32_t *error_reason);              /* 0, or -1 */

/* ---------------------------------------------------------------- regions and modem presets */

typedef struct {
    const char *name;               /* "US" */
    uint32_t start_hz, end_hz;
    int8_t max_dbm;
    uint8_t duty_percent;           /* 100 = no limit */
} purr_mt_region_t;

typedef struct {
    const char *name;               /* "LongFast" */
    uint32_t bw_hz;
    uint8_t sf, cr;                 /* cr is the denominator of 4/x: 5 to 8 */
    uint16_t preamble;
} purr_mt_preset_t;

int purr_mt_region_count(void);
const purr_mt_region_t *purr_mt_region(int i);
const purr_mt_region_t *purr_mt_region_by_name(const char *name);
int purr_mt_preset_count(void);
const purr_mt_preset_t *purr_mt_preset(int i);
const purr_mt_preset_t *purr_mt_preset_by_name(const char *name);

uint32_t purr_mt_hash(const char *s);                                                       /* the firmware's string hash (djb2) */
/* The slot a channel name picks on a region, and its frequency in Hz. slot_override 0 = pick by the name's hash (the default); otherwise the 1-based slot the user chose. */
int purr_mt_slot(const purr_mt_region_t *r, const purr_mt_preset_t *p, const char *channel_name, int slot_override);
uint32_t purr_mt_freq_hz(const purr_mt_region_t *r, const purr_mt_preset_t *p, int slot);   /* slot is 0-based, as purr_mt_slot returns it */

#define PURR_MT_SYNC_WORD 0x2B          /* the LoRa sync word the mesh uses (VERIFY) */
#define PURR_MT_SX126X_SYNC 0x24B4      /* the same as an SX126x register value (what purr_lora_cfg_t.sync_word takes) (VERIFY) */

/* ---------------------------------------------------------------- channel share links (https://meshtastic.org/e/#...) */

/* A link carries a set of channels (the first is the primary) and, usually, the radio settings. It is how a private group is joined: paste the link, or show it as a code. */
typedef struct {
    char name[12];
    uint8_t psk[32];
    int psk_len;
} purr_mt_url_channel_t;

typedef struct {
    purr_mt_url_channel_t ch[8];
    int count;
    char region[12];                /* "" when the link does not say */
    char preset[16];
} purr_mt_url_t;

int purr_mt_url_parse(const char *url, purr_mt_url_t *out);                                 /* the whole link, or only what follows the #; 0, or -1 */
int purr_mt_url_make(const purr_mt_url_t *in, char *out, size_t cap);                       /* the whole link; its length, or -1 if it does not fit */

#ifdef __cplusplus
}
#endif

#endif
