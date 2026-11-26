/*
 * purr_px_pair.h - pairing, the encrypted session, the list of paired devices, and the rule for when a device may pair or connect by itself (plan1.md "Proximity rebuild",
 * RemoteDisplay/SPEC.md section 4). Plain C over purr_px_crypto.h and an elliptic-curve table the platform supplies (micro-ecc P-256 in the module, in the Mini and in the host tools).
 *
 * Identity: every device has one long-term P-256 key pair; its 8-byte device id is the first bytes of the SHA-256 of its public key, so an id cannot be claimed by anybody who does not hold the key.
 *
 * Pairing (the "bring it close" gesture is outside this file: it decides *when* to start): a commitment first, so a device in the middle cannot pick its key after seeing the other's:
 *   initiator -> REQ(commit = SHA-256(pub_i || nonce_i), name)        responder -> ACC(pub_r, nonce_r, name)        initiator -> REVEAL(pub_i, nonce_i, name)
 * both then compute the same six digits from a hash of both keys and both nonces and show them; each user confirms; each side sends FIN, a MAC over that hash under a key from the ECDH of the two
 * identity keys. Paired when the local user has said yes and the peer's FIN checks out. If anyone interfered the two screens show different digits (one chance in a million to match by luck).
 *
 * Session (after pairing, over any byte transport): ephemeral ECDH, both sides sign the transcript with their identity keys, keys for each direction come from HKDF, every message is ChaCha20-Poly1305
 * with a counter nonce that must grow (a replayed or reordered-back message is refused).
 */
#ifndef PURR_PX_PAIR_H
#define PURR_PX_PAIR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- the platform's elliptic-curve and random functions */

typedef struct {
    void (*random)(uint8_t *out, int n);
    int (*keypair)(uint8_t pub[64], uint8_t priv[32]);                                        /* 1 on success */
    int (*shared)(const uint8_t peer_pub[64], const uint8_t priv[32], uint8_t secret[32]);    /* ECDH, 1 on success (0 for a point that is not on the curve) */
    int (*sign)(const uint8_t priv[32], const uint8_t hash[32], uint8_t sig[64]);
    int (*verify)(const uint8_t pub[64], const uint8_t hash[32], const uint8_t sig[64]);      /* 1 if the signature is good */
} purr_px_ecc_t;

void purr_px_id_from_pub(const uint8_t pub[64], uint8_t id[8]);

/* ---------------------------------------------------------------- the paired devices and trusted networks */

#define PURR_PX_MAX_PEERS 8
#define PURR_PX_MAX_NETS 8
#define PURR_PX_PEER_OWNER     (1u << 0)        /* this peer is my owner */
#define PURR_PX_PEER_SPONSORED (1u << 1)        /* added because my owner said to */
#define PURR_PX_PEER_AUTO      (1u << 2)        /* paired without a person (a claim, a signature, developer mode) */

typedef struct {
    uint8_t used;
    uint8_t id[8];
    uint8_t pub[64];
    char name[16];
    uint8_t flags;
} purr_px_peer_rec_t;

typedef struct {
    uint8_t used;
    char ssid[33];
    uint8_t from[8];                            /* who gave it (all zero: the user chose it) */
    uint8_t by_peer;                            /* 1: a paired device handed it over */
    uint8_t shown;                              /* 0 until the one-line notice about it was shown */
} purr_px_net_rec_t;

typedef struct {
    purr_px_peer_rec_t peers[PURR_PX_MAX_PEERS];
    purr_px_net_rec_t nets[PURR_PX_MAX_NETS];
    uint8_t has_owner;
    uint8_t owner_id[8];
    uint8_t allow_any_network;                  /* "Allow auto-connect on any network" */
} purr_px_store_t;

void purr_px_store_init(purr_px_store_t *s);
/* Adds a peer or updates the one with the same id. The slot, or -1 when full. */
int purr_px_store_add_peer(purr_px_store_t *s, const uint8_t pub[64], const char *name, uint8_t flags);
const purr_px_peer_rec_t *purr_px_store_find_peer(const purr_px_store_t *s, const uint8_t id[8]);
int purr_px_store_forget_peer(purr_px_store_t *s, const uint8_t id[8]);      /* 1 if it was there; forgetting the owner clears the owner */
/* Marks a network trusted. 1 if it is new (the caller shows the one-line notice), 0 if it was known, -1 if the list is full. from may be NULL (the user chose it). */
int purr_px_store_add_net(purr_px_store_t *s, const char *ssid, const uint8_t from[8], int by_peer);
int purr_px_store_net_trusted(const purr_px_store_t *s, const char *ssid);
int purr_px_store_forget_net(purr_px_store_t *s, const char *ssid);
/* The next peer-added network whose notice has not been shown (and marks it shown); NULL when there is none. */
const purr_px_net_rec_t *purr_px_store_next_notice(purr_px_store_t *s);
void purr_px_store_set_owner(purr_px_store_t *s, const uint8_t id[8]);
#define PURR_PX_STORE_MAX_BYTES 1400
int purr_px_store_save(const purr_px_store_t *s, uint8_t *out, int cap);      /* bytes written, or -1 */
int purr_px_store_load(purr_px_store_t *s, const uint8_t *in, int n);         /* 0, or -1 for anything that is not a good, whole record (the store is left empty) */

/* ---------------------------------------------------------------- the rule: may these two devices pair or connect without asking? */

typedef struct {
    int peer_known;                             /* already paired */
    int peer_close;                             /* brought close (purr_prox.h) */
    int peer_signed_by_trusted;                 /* it showed a certificate signed by a key this device trusts (the developer key) */
    int we_unowned;                             /* factory fresh or reset */
    int claim_window_open;                      /* the short window after boot or a held button */
    int owner_sponsored;                        /* my owner told me to trust it */
    int dev_mode_lan;                           /* developer mode is on for this boot and the peer is on the same network */
    int network_trusted;                        /* the network both are on is one this device trusts */
    int allow_any_network;                      /* the setting */
    int we_have_screen;                         /* someone can be asked */
    int pairing_window_open;                    /* the person opened "pair a new device" on this one: a stranger may be asked even when not close (over the network) */
} purr_px_policy_in_t;

typedef enum {
    PURR_PX_DENY = 0,
    PURR_PX_ASK,                                /* show the six digits and ask */
    PURR_PX_AUTO_PAIR,                          /* pair with no one asked */
    PURR_PX_ALLOW_KNOWN                         /* a paired device may connect */
} purr_px_decision_t;

purr_px_decision_t purr_px_policy(const purr_px_policy_in_t *in);

/* ---------------------------------------------------------------- pairing */

typedef enum {
    PURR_PX_PAIR_IDLE = 0,
    PURR_PX_PAIR_WAIT_ACC,                      /* initiator: REQ sent */
    PURR_PX_PAIR_WAIT_REVEAL,                   /* responder: ACC sent */
    PURR_PX_PAIR_WAIT_USER,                     /* the six digits are known: waiting for the person (and the peer's FIN) */
    PURR_PX_PAIR_DONE,
    PURR_PX_PAIR_FAILED
} purr_px_pair_state_t;

#define PURR_PX_PAIR_TIMEOUT_MS 60000
#define PURR_PX_PAIR_MAX_MSG 100

typedef struct {
    purr_px_pair_state_t state;
    int role;                                   /* 1 initiator, 2 responder */
    const purr_px_ecc_t *ecc;
    uint8_t self_pub[64], self_priv[32];
    char self_name[16];
    uint8_t peer_pub[64];
    char peer_name[16];
    uint8_t nonce_self[16], nonce_peer[16];
    uint8_t commit_peer[32];
    uint8_t h[32], kc[32];
    int code;                                   /* 0..999999, -1 until known */
    int user;                                   /* 0 not yet, 1 yes, 2 no */
    int peer_fin_ok;
    int fin_sent;
    int fail;                                   /* why it failed, PURR_PX_PAIR_E_* */
    uint32_t started_ms;
} purr_px_pair_t;

#define PURR_PX_PAIR_E_NONE 0
#define PURR_PX_PAIR_E_BAD_MSG 1
#define PURR_PX_PAIR_E_COMMIT 2                 /* the revealed key was not the one committed to */
#define PURR_PX_PAIR_E_MAC 3                    /* the confirmation did not check: the two did not share a secret */
#define PURR_PX_PAIR_E_REFUSED 4                /* a person (here or there) said no */
#define PURR_PX_PAIR_E_TIMEOUT 5
#define PURR_PX_PAIR_E_CURVE 6                  /* a public key that is not a point on the curve */
#define PURR_PX_PAIR_E_SELF 7                   /* it was our own key */

void purr_px_pair_init(purr_px_pair_t *p, const purr_px_ecc_t *ecc, const uint8_t self_pub[64], const uint8_t self_priv[32], const char *self_name);
/* The initiator's first message (written to out). Returns its length, or -1. */
int purr_px_pair_start(purr_px_pair_t *p, uint32_t now_ms, uint8_t *out, int cap);
/* A pairing message arrived. Returns the length of the reply to send (0: none), or -1 when the pairing failed (p->fail says why). */
int purr_px_pair_rx(purr_px_pair_t *p, uint32_t now_ms, const uint8_t *in, int n, uint8_t *out, int cap);
/* The six digits, or -1 until both keys are known. */
int purr_px_pair_code(const purr_px_pair_t *p);
/* The person at this device answered (or the policy answered for them). Returns the length of the FIN to send (0: none to send), or -1. */
int purr_px_pair_user(purr_px_pair_t *p, int accept, uint8_t *out, int cap);
/* 1 and the peer's record when the pairing is done. */
int purr_px_pair_result(const purr_px_pair_t *p, purr_px_peer_rec_t *rec);
/* Gives up on a pairing that has taken too long. 1 if it did. */
int purr_px_pair_tick(purr_px_pair_t *p, uint32_t now_ms);

/* ---------------------------------------------------------------- the encrypted session */

#define PURR_PX_SESS_HELLO1 152                 /* client id (8), ephemeral key (64), nonce (16), signature (64) */
#define PURR_PX_SESS_HELLO2 144                 /* ephemeral key (64), nonce (16), signature (64) */
#define PURR_PX_SESS_HDR 10                     /* channel (1), flags (1), counter (8) */
#define PURR_PX_SESS_OVERHEAD (PURR_PX_SESS_HDR + 16)

#define PURR_PX_CH_CONTROL 0
#define PURR_PX_CH_DISPLAY 1
#define PURR_PX_CH_TERMINAL 2
#define PURR_PX_CH_SETUP 3                      /* the provisioning channel (the remote OOBE) */
#define PURR_PX_CH_SHARE 4
#define PURR_PX_CH_RPC 5

typedef struct {
    int role;                                   /* 1 client, 2 server */
    int ready;
    const purr_px_ecc_t *ecc;
    uint8_t self_pub[64], self_priv[32], self_id[8];
    uint8_t peer_id[8];
    uint8_t eph_priv[32], eph_pub[64], nonce[16];
    uint8_t hello1[PURR_PX_SESS_HELLO1];        /* kept by the client: the transcript the server's signature covers */
    uint8_t key_tx[32], key_rx[32];
    uint64_t ctr_tx, ctr_rx;
    int rx_any;
} purr_px_sess_t;

void purr_px_sess_init(purr_px_sess_t *s, const purr_px_ecc_t *ecc, const uint8_t self_pub[64], const uint8_t self_priv[32]);
/* The client's first message for the server with this id. Returns its length or -1. */
int purr_px_sess_client_start(purr_px_sess_t *s, const uint8_t server_id[8], uint8_t *out, int cap);
/* The server got a HELLO1. `store` finds the client by id; an unknown id is refused. Writes HELLO2 and starts the keys. Returns its length, or -1. */
int purr_px_sess_server_accept(purr_px_sess_t *s, const purr_px_store_t *store, const uint8_t *in, int n, uint8_t *out, int cap);
/* The client got the HELLO2. 0 when the server is the paired device it meant to reach and the keys are ready, else -1. */
int purr_px_sess_client_finish(purr_px_sess_t *s, const purr_px_store_t *store, const uint8_t *in, int n);
/* Seals pt for a channel: header, ciphertext, tag. Returns the length (n + PURR_PX_SESS_OVERHEAD) or -1. */
int purr_px_sess_seal(purr_px_sess_t *s, uint8_t channel, uint8_t flags, const uint8_t *pt, int n, uint8_t *out, int cap);
/* Opens a sealed message. Returns the plaintext length (and *channel, *flags), or -1 for a message that fails its tag, repeats or goes back in the counter, or arrives before the keys are ready. */
int purr_px_sess_open(purr_px_sess_t *s, const uint8_t *in, int n, uint8_t *channel, uint8_t *flags, uint8_t *pt, int cap);

#ifdef __cplusplus
}
#endif

#endif /* PURR_PX_PAIR_H */
