/* purr_prox.c - see purr_prox.h. */
#include "purr_prox.h"

#include <stddef.h>
#include <string.h>

/* ---------------------------------------------------------------- frames */

int purr_px_hdr_put(const purr_px_hdr_t *h, uint8_t *out, int cap)
{
    if (h == NULL || out == NULL || cap < PURR_PX_HDR) {
        return -1;
    }
    out[0] = PURR_PX_MAGIC;
    out[1] = PURR_PX_VERSION;
    out[2] = h->type;
    out[3] = h->flags;
    memcpy(out + 4, h->id, 8);
    return PURR_PX_HDR;
}

int purr_px_hdr_get(const uint8_t *in, int n, purr_px_hdr_t *h)
{
    if (in == NULL || h == NULL || n < PURR_PX_HDR) {
        return -1;
    }
    if (in[0] != PURR_PX_MAGIC) {
        return -2;
    }
    if (in[1] != PURR_PX_VERSION) {
        return -3;
    }
    h->type = in[2];
    h->flags = in[3];
    memcpy(h->id, in + 4, 8);
    return PURR_PX_HDR;
}

/* ---------------------------------------------------------------- the beacon */

int purr_px_beacon_put(const purr_px_beacon_t *b, uint8_t *out, int cap)
{
    if (b == NULL || out == NULL || cap < PURR_PX_BEACON_LEN) {
        return -1;
    }
    out[0] = b->roles;
    out[1] = b->klass;
    out[2] = (uint8_t)b->caps;
    out[3] = (uint8_t)(b->caps >> 8);
    out[4] = (uint8_t)b->counter;
    out[5] = (uint8_t)(b->counter >> 8);
    out[6] = (uint8_t)(b->counter >> 16);
    out[7] = (uint8_t)(b->counter >> 24);
    memcpy(out + 8, b->pair_hint, 4);
    memcpy(out + 12, b->name, 16);
    return PURR_PX_BEACON_LEN;
}

int purr_px_beacon_get(const uint8_t *in, int n, purr_px_beacon_t *b)
{
    if (in == NULL || b == NULL || n < PURR_PX_BEACON_LEN) {
        return -1;
    }
    b->roles = in[0];
    b->klass = in[1];
    b->caps = (uint16_t)(in[2] | (in[3] << 8));
    b->counter = (uint32_t)in[4] | ((uint32_t)in[5] << 8) | ((uint32_t)in[6] << 16) | ((uint32_t)in[7] << 24);
    memcpy(b->pair_hint, in + 8, 4);
    memcpy(b->name, in + 12, 16);
    b->name[15] = '\0';                          /* a name off the air is always a string */
    return PURR_PX_BEACON_LEN;
}

/* ---------------------------------------------------------------- the table */

void purr_px_table_init(purr_px_table_t *t)
{
    memset(t, 0, sizeof(*t));
    t->close_dbm = PURR_PX_CLOSE_DBM_DEFAULT;
    t->close_hold_ms = PURR_PX_CLOSE_HOLD_MS_DEFAULT;
}

void purr_px_table_set_close(purr_px_table_t *t, int dbm, uint32_t hold_ms)
{
    t->close_dbm = (int8_t)(dbm > 0 ? 0 : (dbm < -127 ? -127 : dbm));
    t->close_hold_ms = hold_ms;
    for (int i = 0; i < PURR_PX_PEERS; i++) {
        t->p[i].close_since_set = 0;
    }
}

static purr_px_peer_t *find_mut(purr_px_table_t *t, const uint8_t id[8])
{
    for (int i = 0; i < PURR_PX_PEERS; i++) {
        if (t->p[i].used && memcmp(t->p[i].id, id, 8) == 0) {
            return &t->p[i];
        }
    }
    return NULL;
}

purr_px_peer_t *purr_px_table_seen(purr_px_table_t *t, const uint8_t id[8], int rssi, uint32_t now_ms, const purr_px_beacon_t *beacon)
{
    if (rssi > 0) {
        rssi = 0;
    }
    if (rssi < -127) {
        rssi = -127;
    }
    purr_px_peer_t *p = find_mut(t, id);
    if (p == NULL) {
        int slot = -1;
        for (int i = 0; i < PURR_PX_PEERS && slot < 0; i++) {
            if (!t->p[i].used) {
                slot = i;
            }
        }
        if (slot < 0) {                          /* full: the one heard longest ago goes */
            slot = 0;
            for (int i = 1; i < PURR_PX_PEERS; i++) {
                if ((uint32_t)(now_ms - t->p[i].last_seen_ms) > (uint32_t)(now_ms - t->p[slot].last_seen_ms)) {
                    slot = i;
                }
            }
        }
        p = &t->p[slot];
        memset(p, 0, sizeof(*p));
        p->used = 1;
        memcpy(p->id, id, 8);
        p->rssi = (int8_t)rssi;
    } else {
        p->rssi = (int8_t)((3 * p->rssi + rssi) / 4);
    }
    p->last_seen_ms = now_ms;
    if (p->rssi >= t->close_dbm) {               /* the hold starts when the smoothed signal first reaches the threshold, and ends when it falls below */
        if (!p->close_since_set) {
            p->close_since_set = 1;
            p->close_since_ms = now_ms;
        }
    } else {
        p->close_since_set = 0;
    }
    if (beacon != NULL) {
        p->beacon = *beacon;
    }
    return p;
}

int purr_px_table_prune(purr_px_table_t *t, uint32_t now_ms, uint32_t stale_ms)
{
    int n = 0;
    for (int i = 0; i < PURR_PX_PEERS; i++) {
        if (t->p[i].used && (uint32_t)(now_ms - t->p[i].last_seen_ms) > stale_ms) {
            t->p[i].used = 0;
            n++;
        }
    }
    return n;
}

int purr_px_table_count(const purr_px_table_t *t)
{
    int n = 0;
    for (int i = 0; i < PURR_PX_PEERS; i++) {
        n += t->p[i].used ? 1 : 0;
    }
    return n;
}

const purr_px_peer_t *purr_px_table_find(const purr_px_table_t *t, const uint8_t id[8])
{
    for (int i = 0; i < PURR_PX_PEERS; i++) {
        if (t->p[i].used && memcmp(t->p[i].id, id, 8) == 0) {
            return &t->p[i];
        }
    }
    return NULL;
}

/* "Close" has to be held: the table remembers when the smoothed signal first rose to the threshold (purr_px_table_seen) and forgets it when it falls. The table is only told when a frame
 * arrives, so the test is made at the moment of the question (now_ms), and a device that has gone quiet is never close. */
int purr_px_peer_is_close(const purr_px_table_t *t, const uint8_t id[8], uint32_t now_ms)
{
    const purr_px_peer_t *p = purr_px_table_find(t, id);
    if (p == NULL || !p->close_since_set || p->rssi < t->close_dbm || (uint32_t)(now_ms - p->last_seen_ms) > PURR_PX_CLOSE_FRESH_MS) {
        return 0;
    }
    return (uint32_t)(now_ms - p->close_since_ms) >= t->close_hold_ms;
}

const purr_px_peer_t *purr_px_table_closest(const purr_px_table_t *t, uint32_t now_ms)
{
    const purr_px_peer_t *best = NULL;
    for (int i = 0; i < PURR_PX_PEERS; i++) {
        if (t->p[i].used && purr_px_peer_is_close(t, t->p[i].id, now_ms) && (best == NULL || t->p[i].rssi > best->rssi)) {
            best = &t->p[i];
        }
    }
    return best;
}

/* ---------------------------------------------------------------- messages in pieces */

int purr_px_frag_count(int len)
{
    if (len < 0 || len > PURR_PX_MSG_MAX) {
        return -1;
    }
    return len == 0 ? 1 : (len + PURR_PX_FRAG_PAYLOAD - 1) / PURR_PX_FRAG_PAYLOAD;
}

int purr_px_frag_put(uint16_t msg_id, const uint8_t *msg, int len, int index, uint8_t *out, int cap)
{
    int count = purr_px_frag_count(len);
    if (count < 0 || index < 0 || index >= count || out == NULL || (msg == NULL && len > 0)) {
        return -1;
    }
    int off = index * PURR_PX_FRAG_PAYLOAD;
    int n = len - off < PURR_PX_FRAG_PAYLOAD ? len - off : PURR_PX_FRAG_PAYLOAD;
    if (cap < PURR_PX_FRAG_HDR + n) {
        return -1;
    }
    out[0] = (uint8_t)msg_id;
    out[1] = (uint8_t)(msg_id >> 8);
    out[2] = (uint8_t)index;
    out[3] = (uint8_t)count;
    if (n > 0) {
        memcpy(out + PURR_PX_FRAG_HDR, msg + off, (size_t)n);
    }
    return PURR_PX_FRAG_HDR + n;
}

void purr_px_reasm_init(purr_px_reasm_t *r)
{
    memset(r, 0, offsetof(purr_px_reasm_t, buf));
}

int purr_px_reasm_push(purr_px_reasm_t *r, uint32_t now_ms, const uint8_t *piece, int n)
{
    if (piece == NULL || n < PURR_PX_FRAG_HDR) {
        return -1;
    }
    uint16_t id = (uint16_t)(piece[0] | (piece[1] << 8));
    int index = piece[2], count = piece[3], dl = n - PURR_PX_FRAG_HDR;
    if (count < 1 || count > PURR_PX_FRAG_MAX || index >= count || dl > PURR_PX_FRAG_PAYLOAD) {
        return -1;
    }
    int last = index == count - 1;
    if (!last && dl != PURR_PX_FRAG_PAYLOAD) {
        return -1;                               /* only the last piece may be short */
    }
    if (last && count > 1 && dl == 0) {
        return -1;                               /* and it is not empty unless the whole message is */
    }
    if (!r->active || r->msg_id != id || r->count != count) {
        r->active = 1;                           /* a new message: the newer one wins over an unfinished older one */
        r->msg_id = id;
        r->count = (uint8_t)count;
        r->got = 0;
        r->mask = 0;
        r->last_len = 0;
        r->len = 0;
        r->started_ms = now_ms;
    }
    if (r->mask & (1u << index)) {
        return 0;                                /* a repeat */
    }
    memcpy(r->buf + (size_t)index * PURR_PX_FRAG_PAYLOAD, piece + PURR_PX_FRAG_HDR, (size_t)dl);
    r->mask |= 1u << index;
    r->got++;
    if (last) {
        r->last_len = (uint16_t)dl;
    }
    if (r->got == r->count) {
        r->len = (uint16_t)((count - 1) * PURR_PX_FRAG_PAYLOAD + r->last_len);
        r->active = 0;                           /* handed over: the next piece starts afresh */
        return 1;
    }
    return 0;
}

int purr_px_reasm_expire(purr_px_reasm_t *r, uint32_t now_ms, uint32_t timeout_ms)
{
    if (r->active && (uint32_t)(now_ms - r->started_ms) > timeout_ms) {
        r->active = 0;
        r->got = 0;
        r->mask = 0;
        return 1;
    }
    return 0;
}
