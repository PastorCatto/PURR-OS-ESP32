/* purr_mt_node.c - see purr_mt_node.h. */
#include "purr_mt_node.h"
#include "purr_mt_pkc.h"

#include <string.h>

#define NODEINFO_FIRST_MS 30000u
#define NODEINFO_EVERY_MS (3u * 3600u * 1000u)
#define RETRIES 3
#define SEEN_FOR_MS (10u * 60u * 1000u)

static uint32_t rnd(purr_mt_t *m) { return m->cfg.rand32 != NULL ? m->cfg.rand32() : 4; }

/* ---------------------------------------------------------------- airtime, in integers (the Semtech formula, explicit header, CRC on) */

uint32_t purr_mt_airtime_ms(const purr_mt_preset_t *p, size_t len)
{
    uint64_t tsym_us = ((uint64_t)1000000u << p->sf) / p->bw_hz;
    int de = tsym_us > 16000 ? 1 : 0;
    uint64_t pre_us = ((uint64_t)(4u * p->preamble + 17u) * tsym_us) / 4u;
    int num = 8 * (int)len - 4 * p->sf + 28 + 16;
    int den = 4 * (p->sf - 2 * de);
    int syms = 8;
    if (num > 0) {
        syms += ((num + den - 1) / den) * p->cr;
    }
    uint64_t total = pre_us + (uint64_t)syms * tsym_us;
    return (uint32_t)((total + 999u) / 1000u);
}

/* ---------------------------------------------------------------- setup */

int purr_mt_channel_set(purr_mt_t *m, int index, const char *name, const uint8_t *psk, int psk_len, int role)
{
    if (index < 0 || index >= PURR_MT_MAX_CHANNELS) {
        return -1;
    }
    purr_mt_channel_t *c = &m->cfg.channels[index];
    memset(c, 0, sizeof(*c));
    int kl = purr_mt_expand_psk(psk, psk_len, c->key);
    if (kl < 0) {
        return -1;
    }
    c->keylen = (int8_t)kl;
    size_t i = 0;
    for (; name[i] != '\0' && i < sizeof(c->name) - 1; i++) {
        c->name[i] = name[i];
    }
    c->name[i] = '\0';
    c->hash = purr_mt_channel_hash(c->name, c->key, kl);
    c->role = (uint8_t)role;
    return 0;
}

void purr_mt_node_init(purr_mt_t *m, const purr_mt_cfg_t *cfg, uint32_t now_ms)
{
    uint32_t keep_next = 0;
    (void)keep_next;
    memset(m, 0, sizeof(*m));
    m->cfg = *cfg;
    for (int i = 0; i < PURR_MT_MAX_CHANNELS; i++) {
        purr_mt_channel_t *c = &m->cfg.channels[i];
        c->hash = purr_mt_channel_hash(c->name, c->key, c->keylen);
    }
    if (m->cfg.hop_limit == 0 || m->cfg.hop_limit > 7) {
        m->cfg.hop_limit = 3;
    }
    m->next_id = rnd(m) & 0x3FFu;
    m->next_nodeinfo_ms = now_ms + NODEINFO_FIRST_MS;
}

/* ---------------------------------------------------------------- the pieces */

static void ev_push(purr_mt_t *m, const purr_mt_event_t *e)
{
    if (m->ev_w - m->ev_r >= PURR_MT_EVENTS) {
        return;
    }
    m->events[m->ev_w % PURR_MT_EVENTS] = *e;
    m->ev_w++;
}

int purr_mt_node_event(purr_mt_t *m, purr_mt_event_t *ev)
{
    if (m->ev_r == m->ev_w) {
        return 0;
    }
    *ev = m->events[m->ev_r % PURR_MT_EVENTS];
    m->ev_r++;
    return 1;
}

static int primary_channel(const purr_mt_t *m)
{
    for (int i = 0; i < PURR_MT_MAX_CHANNELS; i++) {
        if (m->cfg.channels[i].role == 1) {
            return i;
        }
    }
    for (int i = 0; i < PURR_MT_MAX_CHANNELS; i++) {
        if (m->cfg.channels[i].role != 0) {
            return i;
        }
    }
    return -1;
}

static purr_mt_nodeinfo_t *node_get(purr_mt_t *m, uint32_t num, uint32_t now_ms, int create)
{
    purr_mt_nodeinfo_t *oldest = &m->nodes[0];
    for (int i = 0; i < PURR_MT_MAX_NODES; i++) {
        if (m->nodes[i].num == num && num != 0) {
            return &m->nodes[i];
        }
        if (m->nodes[i].num == 0) {
            oldest = &m->nodes[i];
            break;
        }
        if (m->nodes[i].last_heard_ms < oldest->last_heard_ms) {
            oldest = &m->nodes[i];
        }
    }
    if (!create) {
        return NULL;
    }
    memset(oldest, 0, sizeof(*oldest));
    oldest->num = num;
    oldest->last_heard_ms = now_ms;
    return oldest;
}

const purr_mt_nodeinfo_t *purr_mt_node_find(const purr_mt_t *m, uint32_t num)
{
    for (int i = 0; i < PURR_MT_MAX_NODES; i++) {
        if (m->nodes[i].num == num && num != 0) {
            return &m->nodes[i];
        }
    }
    return NULL;
}

int purr_mt_node_count(const purr_mt_t *m)
{
    int n = 0;
    for (int i = 0; i < PURR_MT_MAX_NODES; i++) {
        n += m->nodes[i].num != 0;
    }
    return n;
}

const purr_mt_nodeinfo_t *purr_mt_node_at(const purr_mt_t *m, int i)
{
    for (int k = 0; k < PURR_MT_MAX_NODES; k++) {
        if (m->nodes[k].num != 0 && i-- == 0) {
            return &m->nodes[k];
        }
    }
    return NULL;
}

/* A frame for the air: the header, then the Data message encrypted with the channel's key, or (a direct message to a node whose key we know, if allowed) sealed with the two keys. */
static int build_frame_ex(purr_mt_t *m, uint32_t to, uint32_t id, int ch, int want_ack, int hop_limit, const purr_mt_data_t *d, uint8_t *out, size_t cap, int allow_pkc)
{
    if (ch < 0 || ch >= PURR_MT_MAX_CHANNELS || m->cfg.channels[ch].role == 0 || cap < PURR_MT_HDR_LEN + 8) {
        return -1;
    }
    const purr_mt_channel_t *c = &m->cfg.channels[ch];
    uint8_t plain[PURR_MT_MAX_PAYLOAD];
    int n = purr_mt_data_put(d, plain, sizeof(plain));
    if (n < 0) {
        return -1;
    }
    uint8_t hash = c->hash;
    const purr_mt_nodeinfo_t *to_node = to != PURR_MT_BROADCAST ? purr_mt_node_find(m, to) : NULL;
    if (allow_pkc && m->cfg.has_pkc && m->cfg.pkc_send && to_node != NULL && to_node->has_key && (size_t)n + PURR_MT_PKC_OVERHEAD <= PURR_MT_MAX_PAYLOAD && cap >= PURR_MT_HDR_LEN + (size_t)n + PURR_MT_PKC_OVERHEAD) {
        int sl = purr_mt_pkc_seal(m->cfg.priv, to_node->public_key, m->cfg.my_num, id, rnd(m), plain, (size_t)n, out + PURR_MT_HDR_LEN);
        if (sl < 0) {
            return -1;
        }
        n = sl;
        hash = 0;
    } else {
        if ((size_t)n > cap - PURR_MT_HDR_LEN) {
            return -1;
        }
        memcpy(out + PURR_MT_HDR_LEN, plain, (size_t)n);
        purr_mt_crypt(c->key, c->keylen, m->cfg.my_num, id, out + PURR_MT_HDR_LEN, (size_t)n);
    }
    purr_mt_hdr_t h = {.to = to, .from = m->cfg.my_num, .id = id, .hop_limit = (uint8_t)hop_limit, .want_ack = (uint8_t)want_ack, .hop_start = (uint8_t)hop_limit,
                       .channel = hash, .next_hop = 0, .relay_node = (uint8_t)(m->cfg.my_num & 0xFF)};
    purr_mt_hdr_put(&h, out);
    return PURR_MT_HDR_LEN + n;
}

static int build_frame(purr_mt_t *m, uint32_t to, uint32_t id, int ch, int want_ack, int hop_limit, const purr_mt_data_t *d, uint8_t *out, size_t cap)
{
    return build_frame_ex(m, to, id, ch, want_ack, hop_limit, d, out, cap, 0);
}

static uint32_t new_id(purr_mt_t *m)
{
    m->next_id++;
    uint32_t id = ((rnd(m) & 0x3FFu) << 22) | (m->next_id & 0x3FFFFFu);   /* the firmware's shape: a random top, a counter below */
    return id == 0 ? 1 : id;
}

static void queue(purr_mt_t *m, const uint8_t *frame, size_t len, uint32_t due_ms, uint32_t id, uint32_t from, int is_relay)
{
    int slot = -1;
    for (int i = 0; i < PURR_MT_TXQ; i++) {
        if (!m->txq[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (is_relay) {
            return;                                                     /* a full queue drops what it was only passing on */
        }
        slot = 0;                                                       /* ... but not what the person sent */
        for (int i = 1; i < PURR_MT_TXQ; i++) {
            if (m->txq[i].is_relay) {
                slot = i;
                break;
            }
        }
    }
    purr_mt_tx_t *t = &m->txq[slot];
    memcpy(t->frame, frame, len);
    t->len = (uint8_t)len;
    t->due_ms = due_ms;
    t->id = id;
    t->from = from;
    t->used = 1;
    t->is_relay = (uint8_t)is_relay;
}

/* ---------------------------------------------------------------- what we say */

static void queue_text(purr_mt_t *m, uint32_t now_ms, uint32_t to, int ch, uint32_t id, const char *text, size_t n, int want_ack)
{
    purr_mt_data_t d;
    memset(&d, 0, sizeof(d));
    d.portnum = PURR_MT_PORT_TEXT;
    d.payload = (const uint8_t *)text;
    d.payload_len = n;
    uint8_t f[255];
    int fl = build_frame_ex(m, to, id, ch, want_ack, m->cfg.hop_limit, &d, f, sizeof(f), 1);
    if (fl > 0) {
        queue(m, f, (size_t)fl, now_ms + (rnd(m) % 100u), id, m->cfg.my_num, 0);
    }
}

uint32_t purr_mt_node_send_text(purr_mt_t *m, uint32_t now_ms, uint32_t to, int channel, const char *text)
{
    size_t n = strlen(text);
    if (n == 0 || n > PURR_MT_TEXT_MAX) {
        return 0;
    }
    if (channel < 0) {
        channel = primary_channel(m);
    }
    if (channel < 0 || channel >= PURR_MT_MAX_CHANNELS || m->cfg.channels[channel].role == 0) {
        return 0;
    }
    int slot = -1;
    for (int i = 0; i < PURR_MT_PENDING; i++) {
        if (!m->pending[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return 0;                                                       /* too many waiting: the caller tries again */
    }
    uint32_t id = new_id(m);
    int want_ack = to != PURR_MT_BROADCAST;
    purr_mt_pending_t *p = &m->pending[slot];
    memset(p, 0, sizeof(*p));
    p->used = 1;
    p->id = id;
    p->to = to;
    p->channel = (uint8_t)channel;
    p->want_ack = (uint8_t)want_ack;
    p->tries_left = want_ack ? RETRIES : 0;
    p->text_len = (uint8_t)n;
    memcpy(p->text, text, n);
    uint32_t wait = purr_mt_airtime_ms(m->cfg.preset, PURR_MT_HDR_LEN + n + 8) * 6u + 4000u;
    p->retry_ms = now_ms + wait;
    queue_text(m, now_ms, to, channel, id, text, n, want_ack);
    return id;
}

static void queue_nodeinfo(purr_mt_t *m, uint32_t now_ms, uint32_t to, int want_response, uint32_t reply_to_id)
{
    int ch = primary_channel(m);
    if (ch < 0) {
        return;
    }
    purr_mt_user_t u;
    memset(&u, 0, sizeof(u));
    u.id[0] = '!';
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        u.id[1 + i] = hx[(m->cfg.my_num >> (28 - 4 * i)) & 15];
    }
    strncpy(u.long_name, m->cfg.long_name, sizeof(u.long_name) - 1);
    strncpy(u.short_name, m->cfg.short_name, sizeof(u.short_name) - 1);
    u.hw_model = 255;                                                   /* PRIVATE_HW: this is not a model the firmware list knows */
    if (m->cfg.has_pkc) {
        memcpy(u.public_key, m->cfg.pub, 32);
        u.has_key = 1;
    }
    uint8_t body[110];
    int bn = purr_mt_user_put(&u, body, sizeof(body));
    if (bn < 0) {
        return;
    }
    purr_mt_data_t d;
    memset(&d, 0, sizeof(d));
    d.portnum = PURR_MT_PORT_NODEINFO;
    d.payload = body;
    d.payload_len = (size_t)bn;
    d.want_response = (uint8_t)want_response;
    d.reply_id = reply_to_id;
    uint8_t f[255];
    uint32_t id = new_id(m);
    int fl = build_frame(m, to, id, ch, 0, m->cfg.hop_limit, &d, f, sizeof(f));
    if (fl > 0) {
        queue(m, f, (size_t)fl, now_ms + 200u + (rnd(m) % 2000u), id, m->cfg.my_num, 0);
    }
}

int purr_mt_node_send_data(purr_mt_t *m, uint32_t now_ms, uint32_t to, int channel, uint32_t portnum, const uint8_t *payload, size_t len)
{
    if (channel < 0) {
        channel = primary_channel(m);
    }
    if (channel < 0 || len > PURR_MT_TEXT_MAX) {
        return -1;
    }
    purr_mt_data_t d;
    memset(&d, 0, sizeof(d));
    d.portnum = portnum;
    d.payload = payload;
    d.payload_len = len;
    uint8_t f[255];
    uint32_t id = new_id(m);
    int fl = build_frame_ex(m, to, id, channel, 0, m->cfg.hop_limit, &d, f, sizeof(f), 0);
    if (fl <= 0) {
        return -1;
    }
    queue(m, f, (size_t)fl, now_ms + (rnd(m) % 200u), id, m->cfg.my_num, 0);
    return 0;
}

void purr_mt_node_announce(purr_mt_t *m, uint32_t now_ms)
{
    queue_nodeinfo(m, now_ms, PURR_MT_BROADCAST, 1, 0);
    m->next_nodeinfo_ms = now_ms + NODEINFO_EVERY_MS;
}

static void queue_ack(purr_mt_t *m, uint32_t now_ms, uint32_t to, int ch, uint32_t request_id, int pkc)
{
    uint8_t body[4];
    int bn = purr_mt_routing_put(0, body, sizeof(body));
    purr_mt_data_t d;
    memset(&d, 0, sizeof(d));
    d.portnum = PURR_MT_PORT_ROUTING;
    d.payload = body;
    d.payload_len = (size_t)bn;
    d.request_id = request_id;
    uint8_t f[255];
    uint32_t id = new_id(m);
    int fl = build_frame_ex(m, to, id, ch, 0, m->cfg.hop_limit, &d, f, sizeof(f), pkc);
    if (fl > 0) {
        queue(m, f, (size_t)fl, now_ms + 100u + (rnd(m) % 300u), id, m->cfg.my_num, 0);
    }
}

/* ---------------------------------------------------------------- what we hear */

static int seen_before(purr_mt_t *m, uint32_t from, uint32_t id, uint32_t now_ms)
{
    for (int i = 0; i < PURR_MT_SEEN; i++) {
        if (m->seen[i].id == id && m->seen[i].from == from && m->seen[i].at_ms != 0 && now_ms - m->seen[i].at_ms < SEEN_FOR_MS) {
            return 1;
        }
    }
    m->seen[m->seen_next].from = from;
    m->seen[m->seen_next].id = id;
    m->seen[m->seen_next].at_ms = now_ms == 0 ? 1 : now_ms;
    m->seen_next = (m->seen_next + 1) % PURR_MT_SEEN;
    return 0;
}

/* A stronger signal waits longer, so the node that heard it faintest (and so is likely farthest) passes it on first. */
static uint32_t relay_delay_ms(purr_mt_t *m, int snr)
{
    int cw = 3 + ((snr + 20) * 5) / 30;                                 /* snr -20..10 dB gives a window of 3..8 bits */
    cw = cw < 3 ? 3 : (cw > 8 ? 8 : cw);
    uint32_t slot_ms = 20;
    return (rnd(m) & ((1u << cw) - 1u)) * slot_ms + 50u;
}

static void cancel_relay(purr_mt_t *m, uint32_t from, uint32_t id)
{
    for (int i = 0; i < PURR_MT_TXQ; i++) {
        if (m->txq[i].used && m->txq[i].is_relay && m->txq[i].id == id && m->txq[i].from == from) {
            m->txq[i].used = 0;
        }
    }
}

static void handle_data(purr_mt_t *m, uint32_t now_ms, const purr_mt_hdr_t *h, int ch, const purr_mt_data_t *d, int rssi, int snr, int pkc)
{
    int for_me = h->to == m->cfg.my_num;
    int bcast = h->to == PURR_MT_BROADCAST;
    purr_mt_event_t e;
    memset(&e, 0, sizeof(e));
    e.from = h->from;
    e.to = h->to;
    e.id = h->id;
    e.channel = ch;
    e.snr = (int8_t)snr;
    e.rssi = (int16_t)rssi;
    e.pkc = (uint8_t)pkc;
    switch (d->portnum) {
    case PURR_MT_PORT_TEXT:
        if (for_me || bcast) {
            e.kind = PURR_MT_EV_TEXT;
            size_t n = d->payload_len < PURR_MT_TEXT_MAX ? d->payload_len : PURR_MT_TEXT_MAX;
            memcpy(e.text, d->payload, n);
            e.text[n] = '\0';
            ev_push(m, &e);
        }
        break;
    case PURR_MT_PORT_NODEINFO: {
        purr_mt_user_t u;
        if (d->payload != NULL && purr_mt_user_get(d->payload, d->payload_len, &u) == 0) {
            purr_mt_nodeinfo_t *n = node_get(m, h->from, now_ms, 1);
            strncpy(n->long_name, u.long_name, sizeof(n->long_name) - 1);
            strncpy(n->short_name, u.short_name, sizeof(n->short_name) - 1);
            n->has_key = u.has_key;
            memcpy(n->public_key, u.public_key, 32);
            e.kind = PURR_MT_EV_NODE;
            ev_push(m, &e);
            if (d->want_response && (for_me || bcast) && h->from != m->cfg.my_num) {
                queue_nodeinfo(m, now_ms, h->from, 0, h->id);           /* they asked who we are: told to them, not to everyone */
            }
        }
        break;
    }
    case PURR_MT_PORT_POSITION: {
        purr_mt_pos_t p;
        if (d->payload != NULL && purr_mt_pos_get(d->payload, d->payload_len, &p) == 0) {
            purr_mt_nodeinfo_t *n = node_get(m, h->from, now_ms, 1);
            n->has_pos = 1;
            n->lat_e7 = p.lat_e7;
            n->lon_e7 = p.lon_e7;
            n->alt_m = p.has_alt ? p.alt_m : 0;
            e.kind = PURR_MT_EV_NODE;
            ev_push(m, &e);
        }
        break;
    }
    case PURR_MT_PORT_ROUTING: {
        uint32_t err = 0;
        if (for_me && d->request_id != 0 && d->payload != NULL && purr_mt_routing_get(d->payload, d->payload_len, &err) == 0 && err == 0) {
            for (int i = 0; i < PURR_MT_PENDING; i++) {
                if (m->pending[i].used && m->pending[i].id == d->request_id) {
                    m->pending[i].used = 0;
                    e.kind = PURR_MT_EV_DELIVERED;
                    e.id = d->request_id;
                    ev_push(m, &e);
                }
            }
        }
        break;
    }
    case 256:                                                           /* PRIVATE_APP: whatever an application on top of the mesh sends (the admin channel) */
        if ((for_me || bcast) && d->payload != NULL && d->payload_len <= PURR_MT_TEXT_MAX) {
            e.kind = PURR_MT_EV_PRIVATE;
            e.len = (uint16_t)d->payload_len;
            memcpy(e.text, d->payload, d->payload_len);
            ev_push(m, &e);
        }
        break;
    default:
        break;                                                          /* telemetry, admin, traceroute...: heard, passed on, not acted on here */
    }
    if (for_me && h->want_ack && d->portnum != PURR_MT_PORT_ROUTING) {
        queue_ack(m, now_ms, h->from, ch, h->id, pkc);
    }
}

void purr_mt_node_rx(purr_mt_t *m, uint32_t now_ms, const uint8_t *frame, size_t len, int rssi, int snr)
{
    purr_mt_hdr_t h;
    if (purr_mt_hdr_get(frame, len, &h) < 0 || len > 255) {
        return;
    }
    if (h.from == m->cfg.my_num) {                                      /* our own packet, passed on by someone: the implicit acknowledgement of a broadcast */
        for (int i = 0; i < PURR_MT_PENDING; i++) {
            if (m->pending[i].used && m->pending[i].id == h.id && !m->pending[i].want_ack) {
                purr_mt_event_t e;
                memset(&e, 0, sizeof(e));
                e.kind = PURR_MT_EV_HEARD;
                e.id = h.id;
                e.from = h.from;
                ev_push(m, &e);
                m->pending[i].used = 0;
            }
        }
        return;
    }
    if (seen_before(m, h.from, h.id, now_ms)) {
        cancel_relay(m, h.from, h.id);                                  /* someone else passed it on already: ours is not needed */
        return;
    }
    purr_mt_nodeinfo_t *n = node_get(m, h.from, now_ms, 1);
    n->last_heard_ms = now_ms;
    n->snr = (int8_t)snr;
    n->hops_away = h.hop_start != 0 && h.hop_start >= h.hop_limit ? (uint8_t)(h.hop_start - h.hop_limit) : 0;

    /* a channel whose hash matches and whose key makes sense of it */
    size_t plen = len - PURR_MT_HDR_LEN;
    uint8_t plain[PURR_MT_MAX_PAYLOAD + 1];
    int matched = -1;
    purr_mt_data_t d;
    int pkc = 0;
    if (h.channel == 0 && h.to == m->cfg.my_num && m->cfg.has_pkc && plen > PURR_MT_PKC_OVERHEAD && plen <= PURR_MT_MAX_PAYLOAD && n->has_key) {
        int pl = purr_mt_pkc_open(m->cfg.priv, n->public_key, h.from, h.id, frame + PURR_MT_HDR_LEN, plen, plain);
        if (pl > 0 && purr_mt_data_get(plain, (size_t)pl, &d) == 0 && d.portnum != 0) {
            matched = primary_channel(m);
            pkc = 1;
        }
    }
    if (matched < 0 && plen <= PURR_MT_MAX_PAYLOAD) {
        for (int i = 0; i < PURR_MT_MAX_CHANNELS && matched < 0; i++) {
            const purr_mt_channel_t *c = &m->cfg.channels[i];
            if (c->role == 0 || c->hash != h.channel) {
                continue;
            }
            memcpy(plain, frame + PURR_MT_HDR_LEN, plen);
            purr_mt_crypt(c->key, c->keylen, h.from, h.id, plain, plen);
            if (purr_mt_data_get(plain, plen, &d) == 0 && d.portnum != 0) {
                matched = i;
            }
        }
    }
    if (matched >= 0) {
        n->channel = (uint8_t)matched;
        handle_data(m, now_ms, &h, matched, &d, rssi, snr, pkc);
    }

    /* pass it on (also what could not be read: the mesh carries packets for channels this node is not in) */
    if (m->cfg.rebroadcast && h.to != m->cfg.my_num && h.hop_limit > 0) {
        uint8_t f[255];
        memcpy(f, frame, len);
        purr_mt_hdr_t r = h;
        r.hop_limit = (uint8_t)(h.hop_limit - 1);
        r.relay_node = (uint8_t)(m->cfg.my_num & 0xFF);
        purr_mt_hdr_put(&r, f);
        queue(m, f, len, now_ms + relay_delay_ms(m, snr), h.id, h.from, 1);
    }
}

/* ---------------------------------------------------------------- what goes out */

int purr_mt_node_tick(purr_mt_t *m, uint32_t now_ms, uint8_t *frame, size_t cap, size_t *len)
{
    for (int i = 0; i < PURR_MT_PENDING; i++) {
        purr_mt_pending_t *p = &m->pending[i];
        if (!p->used || (int32_t)(now_ms - p->retry_ms) < 0) {
            continue;
        }
        if (!p->want_ack) {
            p->used = 0;                                                /* a broadcast nobody was heard passing on: no news is not a failure */
        } else if (p->tries_left > 0) {
            p->tries_left--;
            queue_text(m, now_ms, p->to, p->channel, p->id, p->text, p->text_len, 1);
            p->retry_ms = now_ms + purr_mt_airtime_ms(m->cfg.preset, PURR_MT_HDR_LEN + p->text_len + 8) * 6u + 4000u + (rnd(m) % 3000u);
        } else {
            purr_mt_event_t e;
            memset(&e, 0, sizeof(e));
            e.kind = PURR_MT_EV_FAILED;
            e.id = p->id;
            e.to = p->to;
            ev_push(m, &e);
            p->used = 0;
        }
    }
    if ((int32_t)(now_ms - m->next_nodeinfo_ms) >= 0) {
        purr_mt_node_announce(m, now_ms);
    }
    if ((int32_t)(now_ms - m->tx_free_ms) < 0) {
        return 0;                                                       /* the last frame is still on the air, or just ended */
    }
    int best = -1;
    for (int i = 0; i < PURR_MT_TXQ; i++) {
        if (m->txq[i].used && (int32_t)(now_ms - m->txq[i].due_ms) >= 0 && (best < 0 || (int32_t)(m->txq[i].due_ms - m->txq[best].due_ms) < 0)) {
            best = i;
        }
    }
    if (best < 0 || m->txq[best].len > cap) {
        return 0;
    }
    memcpy(frame, m->txq[best].frame, m->txq[best].len);
    *len = m->txq[best].len;
    m->tx_free_ms = now_ms + purr_mt_airtime_ms(m->cfg.preset, m->txq[best].len) + 150u;
    m->txq[best].used = 0;
    return 1;
}
