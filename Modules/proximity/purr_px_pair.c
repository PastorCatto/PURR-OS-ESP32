/* purr_px_pair.c - see purr_px_pair.h. */
#include "purr_px_pair.h"

#include <string.h>

#include "purr_px_crypto.h"

void purr_px_id_from_pub(const uint8_t pub[64], uint8_t id[8])
{
    uint8_t d[32];
    purr_px_sha256(pub, 64, d);
    memcpy(id, d, 8);
}

static int ct_equal(const uint8_t *a, const uint8_t *b, int n)
{
    uint8_t d = 0;
    for (int i = 0; i < n; i++) {
        d |= (uint8_t)(a[i] ^ b[i]);
    }
    return d == 0;
}

static void copy_name(char *dst, const char *src)
{
    memset(dst, 0, 16);
    if (src != NULL) {
        for (int i = 0; i < 15 && src[i] != '\0'; i++) {
            dst[i] = src[i];
        }
    }
}

/* ---------------------------------------------------------------- the store */

static uint32_t crc32(const uint8_t *p, int n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
        }
    }
    return ~c;
}

void purr_px_store_init(purr_px_store_t *s)
{
    memset(s, 0, sizeof(*s));
}

int purr_px_store_add_peer(purr_px_store_t *s, const uint8_t pub[64], const char *name, uint8_t flags)
{
    uint8_t id[8];
    purr_px_id_from_pub(pub, id);
    int slot = -1;
    for (int i = 0; i < PURR_PX_MAX_PEERS; i++) {
        if (s->peers[i].used && memcmp(s->peers[i].id, id, 8) == 0) {
            slot = i;
            break;
        }
    }
    for (int i = 0; slot < 0 && i < PURR_PX_MAX_PEERS; i++) {
        if (!s->peers[i].used) {
            slot = i;
        }
    }
    if (slot < 0) {
        return -1;
    }
    purr_px_peer_rec_t *p = &s->peers[slot];
    p->used = 1;
    memcpy(p->id, id, 8);
    memcpy(p->pub, pub, 64);
    copy_name(p->name, name);
    p->flags = flags;
    if (flags & PURR_PX_PEER_OWNER) {
        s->has_owner = 1;
        memcpy(s->owner_id, id, 8);
    }
    return slot;
}

const purr_px_peer_rec_t *purr_px_store_find_peer(const purr_px_store_t *s, const uint8_t id[8])
{
    for (int i = 0; i < PURR_PX_MAX_PEERS; i++) {
        if (s->peers[i].used && memcmp(s->peers[i].id, id, 8) == 0) {
            return &s->peers[i];
        }
    }
    return NULL;
}

int purr_px_store_forget_peer(purr_px_store_t *s, const uint8_t id[8])
{
    for (int i = 0; i < PURR_PX_MAX_PEERS; i++) {
        if (s->peers[i].used && memcmp(s->peers[i].id, id, 8) == 0) {
            memset(&s->peers[i], 0, sizeof(s->peers[i]));
            if (s->has_owner && memcmp(s->owner_id, id, 8) == 0) {
                s->has_owner = 0;
                memset(s->owner_id, 0, 8);
            }
            return 1;
        }
    }
    return 0;
}

static int ssid_eq(const char *a, const char *b)
{
    return strncmp(a, b, 32) == 0;
}

int purr_px_store_add_net(purr_px_store_t *s, const char *ssid, const uint8_t from[8], int by_peer)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return -1;
    }
    for (int i = 0; i < PURR_PX_MAX_NETS; i++) {
        if (s->nets[i].used && ssid_eq(s->nets[i].ssid, ssid)) {
            return 0;
        }
    }
    for (int i = 0; i < PURR_PX_MAX_NETS; i++) {
        if (!s->nets[i].used) {
            purr_px_net_rec_t *n = &s->nets[i];
            memset(n, 0, sizeof(*n));
            n->used = 1;
            strncpy(n->ssid, ssid, 32);
            if (from != NULL) {
                memcpy(n->from, from, 8);
            }
            n->by_peer = by_peer ? 1 : 0;
            n->shown = by_peer ? 0 : 1;                     /* only a network a peer gave needs the notice */
            return 1;
        }
    }
    return -1;
}

int purr_px_store_net_trusted(const purr_px_store_t *s, const char *ssid)
{
    for (int i = 0; ssid != NULL && i < PURR_PX_MAX_NETS; i++) {
        if (s->nets[i].used && ssid_eq(s->nets[i].ssid, ssid)) {
            return 1;
        }
    }
    return 0;
}

int purr_px_store_forget_net(purr_px_store_t *s, const char *ssid)
{
    for (int i = 0; ssid != NULL && i < PURR_PX_MAX_NETS; i++) {
        if (s->nets[i].used && ssid_eq(s->nets[i].ssid, ssid)) {
            memset(&s->nets[i], 0, sizeof(s->nets[i]));
            return 1;
        }
    }
    return 0;
}

const purr_px_net_rec_t *purr_px_store_next_notice(purr_px_store_t *s)
{
    for (int i = 0; i < PURR_PX_MAX_NETS; i++) {
        if (s->nets[i].used && s->nets[i].by_peer && !s->nets[i].shown) {
            s->nets[i].shown = 1;
            return &s->nets[i];
        }
    }
    return NULL;
}

void purr_px_store_set_owner(purr_px_store_t *s, const uint8_t id[8])
{
    s->has_owner = 1;
    memcpy(s->owner_id, id, 8);
}

#define SAVE_FIXED (4 + 1 + 3 + 8)
#define SAVE_PEER 90
#define SAVE_NET 44
#define SAVE_BYTES (SAVE_FIXED + PURR_PX_MAX_PEERS * SAVE_PEER + PURR_PX_MAX_NETS * SAVE_NET + 4)

int purr_px_store_save(const purr_px_store_t *s, uint8_t *out, int cap)
{
    if (cap < SAVE_BYTES) {
        return -1;
    }
    uint8_t *p = out;
    memcpy(p, "PXS1", 4);
    p += 4;
    *p++ = 1;
    *p++ = s->has_owner;
    *p++ = s->allow_any_network;
    *p++ = 0;
    memcpy(p, s->owner_id, 8);
    p += 8;
    for (int i = 0; i < PURR_PX_MAX_PEERS; i++) {
        const purr_px_peer_rec_t *r = &s->peers[i];
        *p++ = r->used;
        memcpy(p, r->id, 8);
        p += 8;
        memcpy(p, r->pub, 64);
        p += 64;
        memcpy(p, r->name, 16);
        p += 16;
        *p++ = r->flags;
    }
    for (int i = 0; i < PURR_PX_MAX_NETS; i++) {
        const purr_px_net_rec_t *r = &s->nets[i];
        *p++ = r->used;
        memcpy(p, r->ssid, 33);
        p += 33;
        memcpy(p, r->from, 8);
        p += 8;
        *p++ = r->by_peer;
        *p++ = r->shown;
    }
    uint32_t c = crc32(out, (int)(p - out));
    *p++ = (uint8_t)c;
    *p++ = (uint8_t)(c >> 8);
    *p++ = (uint8_t)(c >> 16);
    *p++ = (uint8_t)(c >> 24);
    return (int)(p - out);
}

int purr_px_store_load(purr_px_store_t *s, const uint8_t *in, int n)
{
    purr_px_store_init(s);
    if (in == NULL || n != SAVE_BYTES || memcmp(in, "PXS1", 4) != 0 || in[4] != 1) {
        return -1;
    }
    uint32_t want = (uint32_t)in[n - 4] | ((uint32_t)in[n - 3] << 8) | ((uint32_t)in[n - 2] << 16) | ((uint32_t)in[n - 1] << 24);
    if (crc32(in, n - 4) != want) {
        return -1;
    }
    const uint8_t *p = in + 5;
    s->has_owner = p[0] & 1;
    s->allow_any_network = p[1] & 1;
    p += 3;
    memcpy(s->owner_id, p, 8);
    p += 8;
    for (int i = 0; i < PURR_PX_MAX_PEERS; i++) {
        purr_px_peer_rec_t *r = &s->peers[i];
        r->used = *p++ & 1;
        memcpy(r->id, p, 8);
        p += 8;
        memcpy(r->pub, p, 64);
        p += 64;
        memcpy(r->name, p, 16);
        r->name[15] = '\0';
        p += 16;
        r->flags = *p++;
        if (r->used) {
            uint8_t id[8];
            purr_px_id_from_pub(r->pub, id);
            if (memcmp(id, r->id, 8) != 0) {                /* an id that is not the hash of its key: the record is not to be trusted */
                purr_px_store_init(s);
                return -1;
            }
        }
    }
    for (int i = 0; i < PURR_PX_MAX_NETS; i++) {
        purr_px_net_rec_t *r = &s->nets[i];
        r->used = *p++ & 1;
        memcpy(r->ssid, p, 33);
        r->ssid[32] = '\0';
        p += 33;
        memcpy(r->from, p, 8);
        p += 8;
        r->by_peer = *p++ & 1;
        r->shown = *p++ & 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- the rule */

purr_px_decision_t purr_px_policy(const purr_px_policy_in_t *in)
{
    int net_ok = in->network_trusted || in->allow_any_network;
    if (in->peer_known) {
        return (net_ok || in->peer_close) ? PURR_PX_ALLOW_KNOWN : PURR_PX_DENY;
    }
    if (in->peer_signed_by_trusted && (net_ok || in->peer_close)) {
        return PURR_PX_AUTO_PAIR;                           /* (a) trust by signature */
    }
    if (in->owner_sponsored) {
        return PURR_PX_AUTO_PAIR;                           /* (c) the owner said so */
    }
    if (in->dev_mode_lan) {
        return PURR_PX_AUTO_PAIR;                           /* (d) developer mode, same network */
    }
    if (in->we_unowned && in->claim_window_open && in->peer_close) {
        return PURR_PX_AUTO_PAIR;                           /* (b) the first close claim on an unowned device */
    }
    if ((in->peer_close || in->pairing_window_open) && in->we_have_screen) {
        return PURR_PX_ASK;
    }
    return PURR_PX_DENY;                                    /* an owned device never accepts an unknown peer by itself */
}

/* ---------------------------------------------------------------- pairing */

void purr_px_pair_init(purr_px_pair_t *p, const purr_px_ecc_t *ecc, const uint8_t self_pub[64], const uint8_t self_priv[32], const char *self_name)
{
    memset(p, 0, sizeof(*p));
    p->ecc = ecc;
    memcpy(p->self_pub, self_pub, 64);
    memcpy(p->self_priv, self_priv, 32);
    copy_name(p->self_name, self_name);
    p->code = -1;
}

static void fail(purr_px_pair_t *p, int why)
{
    p->state = PURR_PX_PAIR_FAILED;
    p->fail = why;
}

/* Both keys and both nonces are known: the hash, the six digits and the confirmation key. */
static int derive(purr_px_pair_t *p)
{
    const uint8_t *pub_i = p->role == 1 ? p->self_pub : p->peer_pub, *pub_r = p->role == 1 ? p->peer_pub : p->self_pub;
    const uint8_t *non_i = p->role == 1 ? p->nonce_self : p->nonce_peer, *non_r = p->role == 1 ? p->nonce_peer : p->nonce_self;
    if (memcmp(p->self_pub, p->peer_pub, 64) == 0) {
        fail(p, PURR_PX_PAIR_E_SELF);
        return -1;
    }
    uint8_t secret[32];
    if (!p->ecc->shared(p->peer_pub, p->self_priv, secret)) {
        fail(p, PURR_PX_PAIR_E_CURVE);
        return -1;
    }
    purr_px_sha256_t c;
    purr_px_sha256_init(&c);
    purr_px_sha256_update(&c, "PURR-SAS1", 9);
    purr_px_sha256_update(&c, pub_i, 64);
    purr_px_sha256_update(&c, pub_r, 64);
    purr_px_sha256_update(&c, non_i, 16);
    purr_px_sha256_update(&c, non_r, 16);
    purr_px_sha256_final(&c, p->h);
    uint32_t v = ((uint32_t)p->h[0] << 24) | ((uint32_t)p->h[1] << 16) | ((uint32_t)p->h[2] << 8) | p->h[3];
    p->code = (int)(v % 1000000u);
    purr_px_hkdf_sha256(p->h, 32, secret, 32, (const uint8_t *)"PURR-PAIR-CONFIRM", 17, p->kc, 32);
    memset(secret, 0, sizeof(secret));
    p->state = PURR_PX_PAIR_WAIT_USER;
    return 0;
}

static void fin_mac(const purr_px_pair_t *p, int role, uint8_t mac[32])
{
    uint8_t m[33];
    m[0] = role == 1 ? 'I' : 'R';
    memcpy(m + 1, p->h, 32);
    purr_px_hmac_sha256(p->kc, 32, m, 33, mac);
}

int purr_px_pair_start(purr_px_pair_t *p, uint32_t now_ms, uint8_t *out, int cap)
{
    if (cap < 49 || p->state != PURR_PX_PAIR_IDLE) {
        return -1;
    }
    p->role = 1;
    p->started_ms = now_ms;
    p->ecc->random(p->nonce_self, 16);
    uint8_t buf[80];
    memcpy(buf, p->self_pub, 64);
    memcpy(buf + 64, p->nonce_self, 16);
    out[0] = 1;
    purr_px_sha256(buf, 80, out + 1);                       /* the commitment to our key and nonce */
    memcpy(out + 33, p->self_name, 16);
    p->state = PURR_PX_PAIR_WAIT_ACC;
    return 49;
}

int purr_px_pair_rx(purr_px_pair_t *p, uint32_t now_ms, const uint8_t *in, int n, uint8_t *out, int cap)
{
    if (n < 1 || p->state == PURR_PX_PAIR_FAILED || p->state == PURR_PX_PAIR_DONE) {
        return p->state == PURR_PX_PAIR_FAILED ? -1 : 0;
    }
    switch (in[0]) {
    case 1:                                                 /* REQ: a responder starts */
        if (p->state != PURR_PX_PAIR_IDLE || n != 49 || cap < 97) {
            fail(p, PURR_PX_PAIR_E_BAD_MSG);
            return -1;
        }
        p->role = 2;
        p->started_ms = now_ms;
        memcpy(p->commit_peer, in + 1, 32);
        memcpy(p->peer_name, in + 33, 16);
        p->peer_name[15] = '\0';
        p->ecc->random(p->nonce_self, 16);
        out[0] = 2;
        memcpy(out + 1, p->self_pub, 64);
        memcpy(out + 65, p->nonce_self, 16);
        memcpy(out + 81, p->self_name, 16);
        p->state = PURR_PX_PAIR_WAIT_REVEAL;
        return 97;
    case 2:                                                 /* ACC: the initiator learns the responder's key and reveals its own */
        if (p->state != PURR_PX_PAIR_WAIT_ACC || n != 97 || cap < 97) {
            fail(p, PURR_PX_PAIR_E_BAD_MSG);
            return -1;
        }
        memcpy(p->peer_pub, in + 1, 64);
        memcpy(p->nonce_peer, in + 65, 16);
        memcpy(p->peer_name, in + 81, 16);
        p->peer_name[15] = '\0';
        out[0] = 3;
        memcpy(out + 1, p->self_pub, 64);
        memcpy(out + 65, p->nonce_self, 16);
        memcpy(out + 81, p->self_name, 16);
        if (derive(p) != 0) {
            return -1;
        }
        return 97;
    case 3: {                                               /* REVEAL: the responder checks the commitment */
        if (p->state != PURR_PX_PAIR_WAIT_REVEAL || n != 97) {
            fail(p, PURR_PX_PAIR_E_BAD_MSG);
            return -1;
        }
        uint8_t d[32];
        purr_px_sha256(in + 1, 80, d);
        if (!ct_equal(d, p->commit_peer, 32)) {
            fail(p, PURR_PX_PAIR_E_COMMIT);
            return -1;
        }
        memcpy(p->peer_pub, in + 1, 64);
        memcpy(p->nonce_peer, in + 65, 16);
        memcpy(p->peer_name, in + 81, 16);
        p->peer_name[15] = '\0';
        return derive(p) != 0 ? -1 : 0;
    }
    case 4: {                                               /* FIN: the peer's person said yes and proved the same secret */
        if (p->state != PURR_PX_PAIR_WAIT_USER || n != 33) {
            fail(p, PURR_PX_PAIR_E_BAD_MSG);
            return -1;
        }
        uint8_t want[32];
        fin_mac(p, p->role == 1 ? 2 : 1, want);
        if (!ct_equal(want, in + 1, 32)) {
            fail(p, PURR_PX_PAIR_E_MAC);
            return -1;
        }
        p->peer_fin_ok = 1;
        if (p->user == 1) {
            p->state = PURR_PX_PAIR_DONE;
        }
        return 0;
    }
    case 5:
        fail(p, PURR_PX_PAIR_E_REFUSED);
        return -1;
    default:
        fail(p, PURR_PX_PAIR_E_BAD_MSG);
        return -1;
    }
}

int purr_px_pair_code(const purr_px_pair_t *p)
{
    return p->state == PURR_PX_PAIR_WAIT_USER || p->state == PURR_PX_PAIR_DONE ? p->code : -1;
}

int purr_px_pair_user(purr_px_pair_t *p, int accept, uint8_t *out, int cap)
{
    if (p->state != PURR_PX_PAIR_WAIT_USER || p->user != 0) {
        return -1;
    }
    if (!accept) {
        p->user = 2;
        fail(p, PURR_PX_PAIR_E_REFUSED);
        if (cap < 2) {
            return -1;
        }
        out[0] = 5;
        out[1] = PURR_PX_PAIR_E_REFUSED;
        return 2;
    }
    if (cap < 33) {
        return -1;
    }
    p->user = 1;
    out[0] = 4;
    fin_mac(p, p->role, out + 1);
    p->fin_sent = 1;
    if (p->peer_fin_ok) {
        p->state = PURR_PX_PAIR_DONE;
    }
    return 33;
}

int purr_px_pair_result(const purr_px_pair_t *p, purr_px_peer_rec_t *rec)
{
    if (p->state != PURR_PX_PAIR_DONE) {
        return 0;
    }
    memset(rec, 0, sizeof(*rec));
    rec->used = 1;
    memcpy(rec->pub, p->peer_pub, 64);
    purr_px_id_from_pub(p->peer_pub, rec->id);
    memcpy(rec->name, p->peer_name, 16);
    return 1;
}

int purr_px_pair_tick(purr_px_pair_t *p, uint32_t now_ms)
{
    if ((p->state == PURR_PX_PAIR_WAIT_ACC || p->state == PURR_PX_PAIR_WAIT_REVEAL || p->state == PURR_PX_PAIR_WAIT_USER) && (uint32_t)(now_ms - p->started_ms) > PURR_PX_PAIR_TIMEOUT_MS) {
        fail(p, PURR_PX_PAIR_E_TIMEOUT);
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- the session */

void purr_px_sess_init(purr_px_sess_t *s, const purr_px_ecc_t *ecc, const uint8_t self_pub[64], const uint8_t self_priv[32])
{
    memset(s, 0, sizeof(*s));
    s->ecc = ecc;
    memcpy(s->self_pub, self_pub, 64);
    memcpy(s->self_priv, self_priv, 32);
    purr_px_id_from_pub(self_pub, s->self_id);
}

static void h1(const uint8_t id_c[8], const uint8_t id_s[8], const uint8_t eph[64], const uint8_t nonce[16], uint8_t out[32])
{
    purr_px_sha256_t c;
    purr_px_sha256_init(&c);
    purr_px_sha256_update(&c, "PURR-S1", 7);
    purr_px_sha256_update(&c, id_c, 8);
    purr_px_sha256_update(&c, id_s, 8);
    purr_px_sha256_update(&c, eph, 64);
    purr_px_sha256_update(&c, nonce, 16);
    purr_px_sha256_final(&c, out);
}

static void h2(const uint8_t id_c[8], const uint8_t id_s[8], const uint8_t eph_c[64], const uint8_t nonce_c[16], const uint8_t eph_s[64], const uint8_t nonce_s[16], uint8_t out[32])
{
    purr_px_sha256_t c;
    purr_px_sha256_init(&c);
    purr_px_sha256_update(&c, "PURR-S2", 7);
    purr_px_sha256_update(&c, id_c, 8);
    purr_px_sha256_update(&c, id_s, 8);
    purr_px_sha256_update(&c, eph_c, 64);
    purr_px_sha256_update(&c, nonce_c, 16);
    purr_px_sha256_update(&c, eph_s, 64);
    purr_px_sha256_update(&c, nonce_s, 16);
    purr_px_sha256_final(&c, out);
}

static void keys(const uint8_t secret[32], const uint8_t nonce_c[16], const uint8_t nonce_s[16], uint8_t k_c2s[32], uint8_t k_s2c[32])
{
    uint8_t salt[32], okm[64];
    memcpy(salt, nonce_c, 16);
    memcpy(salt + 16, nonce_s, 16);
    purr_px_hkdf_sha256(salt, 32, secret, 32, (const uint8_t *)"PURR-SESSION-KEYS", 17, okm, 64);
    memcpy(k_c2s, okm, 32);
    memcpy(k_s2c, okm + 32, 32);
}

int purr_px_sess_client_start(purr_px_sess_t *s, const uint8_t server_id[8], uint8_t *out, int cap)
{
    if (cap < PURR_PX_SESS_HELLO1) {
        return -1;
    }
    s->role = 1;
    memcpy(s->peer_id, server_id, 8);
    if (!s->ecc->keypair(s->eph_pub, s->eph_priv)) {
        return -1;
    }
    s->ecc->random(s->nonce, 16);
    uint8_t hash[32];
    h1(s->self_id, server_id, s->eph_pub, s->nonce, hash);
    memcpy(out, s->self_id, 8);
    memcpy(out + 8, s->eph_pub, 64);
    memcpy(out + 72, s->nonce, 16);
    if (!s->ecc->sign(s->self_priv, hash, out + 88)) {
        return -1;
    }
    memcpy(s->hello1, out, PURR_PX_SESS_HELLO1);
    return PURR_PX_SESS_HELLO1;
}

int purr_px_sess_server_accept(purr_px_sess_t *s, const purr_px_store_t *store, const uint8_t *in, int n, uint8_t *out, int cap)
{
    if (n != PURR_PX_SESS_HELLO1 || cap < PURR_PX_SESS_HELLO2) {
        return -1;
    }
    const purr_px_peer_rec_t *peer = purr_px_store_find_peer(store, in);
    if (peer == NULL) {
        return -1;                                          /* not a paired device */
    }
    uint8_t hash[32];
    h1(in, s->self_id, in + 8, in + 72, hash);
    if (!s->ecc->verify(peer->pub, hash, in + 88)) {
        return -1;
    }
    s->role = 2;
    memcpy(s->peer_id, in, 8);
    uint8_t eph_priv[32], eph_pub[64], nonce_s[16], secret[32];
    if (!s->ecc->keypair(eph_pub, eph_priv) || !s->ecc->shared(in + 8, eph_priv, secret)) {
        return -1;
    }
    s->ecc->random(nonce_s, 16);
    h2(in, s->self_id, in + 8, in + 72, eph_pub, nonce_s, hash);
    memcpy(out, eph_pub, 64);
    memcpy(out + 64, nonce_s, 16);
    if (!s->ecc->sign(s->self_priv, hash, out + 80)) {
        return -1;
    }
    keys(secret, in + 72, nonce_s, s->key_rx, s->key_tx);   /* the server receives what the client sends */
    memset(secret, 0, sizeof(secret));
    memset(eph_priv, 0, sizeof(eph_priv));
    s->ctr_tx = 0;
    s->rx_any = 0;
    s->ready = 1;
    return PURR_PX_SESS_HELLO2;
}

int purr_px_sess_client_finish(purr_px_sess_t *s, const purr_px_store_t *store, const uint8_t *in, int n)
{
    if (s->role != 1 || s->ready || n != PURR_PX_SESS_HELLO2) {
        return -1;
    }
    const purr_px_peer_rec_t *server = purr_px_store_find_peer(store, s->peer_id);
    if (server == NULL) {
        return -1;
    }
    uint8_t hash[32];
    h2(s->self_id, s->peer_id, s->hello1 + 8, s->hello1 + 72, in, in + 64, hash);
    if (!s->ecc->verify(server->pub, hash, in + 80)) {
        return -1;                                          /* not the device we meant to reach */
    }
    uint8_t secret[32];
    if (!s->ecc->shared(in, s->eph_priv, secret)) {
        return -1;
    }
    keys(secret, s->hello1 + 72, in + 64, s->key_tx, s->key_rx);
    memset(secret, 0, sizeof(secret));
    memset(s->eph_priv, 0, sizeof(s->eph_priv));
    s->ctr_tx = 0;
    s->rx_any = 0;
    s->ready = 1;
    return 0;
}

static void nonce_for(uint64_t ctr, uint8_t nonce[12])
{
    memset(nonce, 0, 12);
    for (int i = 0; i < 8; i++) {
        nonce[4 + i] = (uint8_t)(ctr >> (8 * i));
    }
}

int purr_px_sess_seal(purr_px_sess_t *s, uint8_t channel, uint8_t flags, const uint8_t *pt, int n, uint8_t *out, int cap)
{
    if (!s->ready || n < 0 || cap < n + PURR_PX_SESS_OVERHEAD) {
        return -1;
    }
    uint64_t ctr = s->ctr_tx++;
    out[0] = channel;
    out[1] = flags;
    for (int i = 0; i < 8; i++) {
        out[2 + i] = (uint8_t)(ctr >> (8 * i));
    }
    uint8_t nonce[12];
    nonce_for(ctr, nonce);
    purr_px_aead_seal(s->key_tx, nonce, out, PURR_PX_SESS_HDR, pt, (size_t)n, out + PURR_PX_SESS_HDR);
    return n + PURR_PX_SESS_OVERHEAD;
}

int purr_px_sess_open(purr_px_sess_t *s, const uint8_t *in, int n, uint8_t *channel, uint8_t *flags, uint8_t *pt, int cap)
{
    if (!s->ready || n < PURR_PX_SESS_OVERHEAD || cap < n - PURR_PX_SESS_OVERHEAD) {
        return -1;
    }
    uint64_t ctr = 0;
    for (int i = 0; i < 8; i++) {
        ctr |= (uint64_t)in[2 + i] << (8 * i);
    }
    if (s->rx_any && ctr <= s->ctr_rx) {
        return -1;                                          /* a repeat, or a message from before one already seen */
    }
    uint8_t nonce[12];
    nonce_for(ctr, nonce);
    if (purr_px_aead_open(s->key_rx, nonce, in, PURR_PX_SESS_HDR, in + PURR_PX_SESS_HDR, (size_t)(n - PURR_PX_SESS_HDR), pt) != 0) {
        return -1;
    }
    s->ctr_rx = ctr;
    s->rx_any = 1;
    *channel = in[0];
    *flags = in[1];
    return n - PURR_PX_SESS_OVERHEAD;
}
