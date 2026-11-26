/* purr_px_link.c - see purr_px_link.h. */
#include <string.h>

#include "purr_px_link.h"
#include "purr_px_crypto.h"

#define UDP_PORT          47804
#define BEACON_MS         1000
#define STALE_MS          6000
#define ONLINE_MS         3500
#define CLAIM_WINDOW_MS   120000
#define BLOB_MAGIC        "PXID1"

static const purr_pxl_io_t *s_io;

/* ---------------------------------------------------------------- state */


/* Scratch for a whole message: static, not on the stack (the caller's stack is small, and the driver is only ever called by one task at a time). */


/* ---------------------------------------------------------------- small helpers */

static uint32_t now_ms(void) { return (uint32_t)(s_io->now_us() / 1000u); }
static int same_id(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 8) == 0; }

static void copy_name(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    for (; i + 1 < cap && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static const char HEX[] = "0123456789abcdef";

/* ---------------------------------------------------------------- storage: identity and the pairing store in one blob */

static void blob_save(purr_pxl_t *L)
{
    uint8_t *buf = L->blob;
    int n = 0;
    memcpy(buf, BLOB_MAGIC, 5);
    n = 5;
    memcpy(buf + n, L->priv, 32);
    n += 32;
    memcpy(buf + n, L->pub, 64);
    n += 64;
    int w = purr_px_store_save(&L->store, buf + n, (int)sizeof(L->blob) - n);
    if (w < 0) {
        return;
    }
    n += w;
    if (s_io->store_save(buf, (size_t)n) == 0) {
        L->dirty = 0;
    }
}

static int blob_load(purr_pxl_t *L)
{
    uint8_t *buf = L->blob;
    int n = s_io->store_load(buf, sizeof(L->blob));
    if (n < 5 + 96 || memcmp(buf, BLOB_MAGIC, 5) != 0) {
        return 0;
    }
    uint8_t id[8];
    purr_px_id_from_pub(buf + 5 + 32, id);
    memcpy(L->priv, buf + 5, 32);
    memcpy(L->pub, buf + 5 + 32, 64);
    memcpy(L->id, id, 8);
    if (purr_px_store_load(&L->store, buf + 5 + 96, n - (5 + 96)) != 0) {
        purr_px_store_init(&L->store);                      /* identity kept, the pairings were damaged: start from none */
    }
    return 1;
}

/* ---------------------------------------------------------------- frames */

static pxl_addr_t *addr_for(purr_pxl_t *L, const uint8_t id[8], int create)
{
    pxl_addr_t *free_slot = NULL;
    for (int i = 0; i < PURR_PX_PEERS; i++) {
        if (L->addr[i].used && same_id(L->addr[i].id, id)) {
            return &L->addr[i];
        }
        if (!L->addr[i].used && free_slot == NULL) {
            free_slot = &L->addr[i];
        }
    }
    if (create && free_slot != NULL) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->used = 1;
        memcpy(free_slot->id, id, 8);
        return free_slot;
    }
    return NULL;
}

/* One frame to `to` (NULL: everyone in range, on both transports). */
static void frame_out(purr_pxl_t *L, const pxl_addr_t *to, uint8_t type, uint8_t flags, const uint8_t *payload, int n)
{
    uint8_t f[PURR_PX_MAX_FRAME];
    purr_px_hdr_t h = {.type = type, .flags = flags};
    memcpy(h.id, L->id, 8);
    int hl = purr_px_hdr_put(&h, f, (int)sizeof(f));
    if (hl < 0 || n < 0 || hl + n > (int)sizeof(f)) {
        return;
    }
    memcpy(f + hl, payload, (size_t)n);
    int total = hl + n;
    if (to == NULL) {
        s_io->radio_send(NULL, f, (size_t)total);
        if (L->udp > 0) {
            s_io->udp_send(L->udp, 0xFFFFFFFFu, UDP_PORT, f, (size_t)total);
        }
        return;
    }
    if (to->via == PURR_LINK_VIA_RADIO) {
        s_io->radio_send(to->mac, f, (size_t)total);
    } else if (L->udp > 0) {
        s_io->udp_send(L->udp, to->ip, to->port, f, (size_t)total);
    }
}

static int roles_now(const purr_pxl_t *L)
{
    int r = PURR_PX_ROLE_SCREEN;
    if (L->pair.state == PURR_PX_PAIR_WAIT_USER || L->pair.state == PURR_PX_PAIR_WAIT_REVEAL) {
        r |= PURR_PX_ROLE_PAIRING;
    }
    if (s_io->headless && !L->store.has_owner) {
        r |= PURR_PX_ROLE_UNOWNED;
    }
    if (L->pair_window_until != 0 && (int32_t)(L->pair_window_until - now_ms()) > 0) {
        r |= PURR_PX_ROLE_PAIRING;
    }
    return r;
}

static void send_beacon(purr_pxl_t *L)
{
    purr_px_beacon_t b;
    memset(&b, 0, sizeof(b));
    b.roles = (uint8_t)roles_now(L);
    b.klass = s_io->klass;
    b.caps = s_io->caps;
    b.counter = ++L->beacon_counter;
    copy_name(b.name, sizeof(b.name), L->name);
    uint8_t p[PURR_PX_BEACON_LEN];
    int n = purr_px_beacon_put(&b, p, (int)sizeof(p));
    if (n > 0) {
        frame_out(L, NULL, PURR_PX_T_BEACON, 0, p, n);
    }
}

/* ---------------------------------------------------------------- sessions */

static pxl_sess_t *sess_find(purr_pxl_t *L, const uint8_t id[8])
{
    for (int i = 0; i < PURR_PXL_SESSIONS; i++) {
        if (L->sess[i].used && same_id(L->sess[i].id, id)) {
            return &L->sess[i];
        }
    }
    return NULL;
}

static pxl_sess_t *sess_new(purr_pxl_t *L, const uint8_t id[8])
{
    pxl_sess_t *s = sess_find(L, id);
    if (s == NULL) {
        for (int i = 0; i < PURR_PXL_SESSIONS && s == NULL; i++) {
            if (!L->sess[i].used) {
                s = &L->sess[i];
            }
        }
    }
    if (s == NULL) {
        s = &L->sess[0];                                    /* all busy: the oldest slot is given up */
    }
    memset(s, 0, sizeof(*s));
    s->used = 1;
    memcpy(s->id, id, 8);
    purr_px_reasm_init(&s->reasm);
    purr_px_sess_init(&s->s, s_io->ecc, L->pub, L->priv);
    s->started_ms = now_ms();
    return s;
}

static void sess_send(purr_pxl_t *L, pxl_sess_t *s, uint8_t channel, const uint8_t *data, int n)
{
    uint8_t *rec = L->rec;
    int rl = purr_px_sess_seal(&s->s, channel, 0, data, n, rec, (int)sizeof(L->rec));
    if (rl <= 0) {
        return;
    }
    pxl_addr_t *a = addr_for(L, s->id, 0);
    if (a == NULL) {
        return;
    }
    int pieces = purr_px_frag_count(rl);
    uint16_t id = ++s->next_msg;
    for (int i = 0; i < pieces; i++) {
        uint8_t piece[PURR_PX_MAX_PAYLOAD];
        int pl = purr_px_frag_put(id, rec, rl, i, piece, (int)sizeof(piece));
        if (pl > 0) {
            frame_out(L, a, PURR_PX_T_SECURE, 0, piece, pl);
        }
    }
}

static void control_in(purr_pxl_t *L, pxl_sess_t *s, const uint8_t *m, int n)
{
    if (n < 1) {
        return;
    }
    if (m[0] == CTL_WIFI_OFFER && n >= 3) {
        int sl = m[1];
        if (2 + sl + 1 > n || sl > 32) {
            return;
        }
        char ssid[33], pass[65];
        memcpy(ssid, m + 2, (size_t)sl);
        ssid[sl] = '\0';
        int pl = m[2 + sl];
        if (2 + sl + 1 + pl > n || pl > 64) {
            return;
        }
        memcpy(pass, m + 3 + sl, (size_t)pl);
        pass[pl] = '\0';
        /* A network a paired device hands over is taken as trusted: saved and marked, with a one-line notice (plan1.md, "Auto-connect"). */
        if (s_io->net_saved_add != NULL && s_io->net_saved_add(ssid, pass, s->id) == 0) {
            if (purr_px_store_add_net(&L->store, ssid, s->id, 1) >= 0) {
                L->dirty = 1;
            }
            uint8_t ack[2] = {CTL_WIFI_ACK, 1};
            sess_send(L, s, PURR_PX_CH_CONTROL, ack, 2);
        }
        memset(pass, 0, sizeof(pass));
    } else if (m[0] == CTL_PING) {
        uint8_t pong = CTL_PING;
        sess_send(L, s, PURR_PX_CH_CONTROL, &pong, 1);
    }
}

static void secure_in(purr_pxl_t *L, const purr_px_hdr_t *h, const uint8_t *p, int n, uint32_t t)
{
    pxl_sess_t *s = sess_find(L, h->id);
    if (s == NULL || !s->s.ready) {
        return;
    }
    if (purr_px_reasm_push(&s->reasm, t, p, n) <= 0) {
        return;                                             /* more pieces to come (or a bad one) */
    }
    uint8_t ch, fl;
    uint8_t *pt = L->pt;
    int pl = purr_px_sess_open(&s->s, s->reasm.buf, s->reasm.len, &ch, &fl, pt, (int)sizeof(L->pt));
    purr_px_reasm_init(&s->reasm);
    if (pl < 0) {
        return;                                             /* did not authenticate, or a replay: dropped without a word */
    }
    if (ch == PURR_PX_CH_CONTROL) {
        control_in(L, s, pt, pl);
        return;
    }
    if (s->in_len == 0 && pl <= (int)sizeof(s->in)) {
        memcpy(s->in, pt, (size_t)pl);
        s->in_len = pl > 0 ? pl : -1;
        s->in_ch = ch;
    }
}

/* ---------------------------------------------------------------- pairing */

static void pair_reset(purr_pxl_t *L)
{
    purr_px_pair_init(&L->pair, s_io->ecc, L->pub, L->priv, L->name);
    L->pair_auto = 0;
    L->pair_prompted = 0;
    L->pair_flags = 0;
}

static void pair_send(purr_pxl_t *L, const uint8_t *id, const uint8_t *m, int n)
{
    if (n > 0) {
        frame_out(L, addr_for(L, id, 0), PURR_PX_T_PAIR, 0, m, n);
        if (n <= (int)sizeof(L->pair_last)) {                   /* kept, and sent again until the other end answers: a radio drops frames */
            memcpy(L->pair_last, m, (size_t)n);
            L->pair_last_n = n;
            L->pair_last_ms = now_ms();
            L->pair_resends = 0;
        }
    }
}

static void pair_finish_if_done(purr_pxl_t *L)
{
    if (L->pair.state != PURR_PX_PAIR_DONE) {
        return;
    }
    purr_px_peer_rec_t rec;
    if (purr_px_pair_result(&L->pair, &rec) == 1) {
        uint8_t flags = L->pair_flags;
        if (purr_px_store_add_peer(&L->store, rec.pub, rec.name, flags) >= 0) {
            if (!L->store.has_owner && (flags & PURR_PX_PEER_AUTO)) {
                purr_px_store_set_owner(&L->store, rec.id);
            }
            L->dirty = 1;
        }
    }
    pair_reset(L);
}

static int policy_for(purr_pxl_t *L, const uint8_t id[8], uint32_t t)
{
    purr_px_policy_in_t in;
    memset(&in, 0, sizeof(in));
    in.peer_known = purr_px_store_find_peer(&L->store, id) != NULL;
    in.peer_close = purr_px_peer_is_close(&L->table, id, t);
    in.we_unowned = s_io->headless && !L->store.has_owner;       /* a first claim is for a device nobody can be asked at */
    in.claim_window_open = (t - L->boot_ms) < CLAIM_WINDOW_MS;
    in.pairing_window_open = L->pair_window_until != 0 && (int32_t)(L->pair_window_until - t) > 0;
    in.allow_any_network = L->store.allow_any_network;
    in.we_have_screen = 1;
    return (int)purr_px_policy(&in);
}

static void pair_in(purr_pxl_t *L, const purr_px_hdr_t *h, const uint8_t *p, int n, uint32_t t)
{
    if (n >= 1 && p[0] == HELLO1_TAG) {                     /* a paired device wants a session */
        if (!purr_px_store_find_peer(&L->store, h->id)) {
            return;
        }
        pxl_sess_t *s = sess_new(L, h->id);
        uint8_t out[PURR_PX_SESS_HELLO2 + 1];
        int w = purr_px_sess_server_accept(&s->s, &L->store, p + 1, n - 1, out + 1, (int)sizeof(out) - 1);
        if (w < 0) {
            s->used = 0;
            return;
        }
        out[0] = HELLO2_TAG;
        frame_out(L, addr_for(L, h->id, 0), PURR_PX_T_PAIR, 0, out, w + 1);
        return;
    }
    if (n >= 1 && p[0] == HELLO2_TAG) {
        pxl_sess_t *s = sess_find(L, h->id);
        if (s != NULL && s->s.role == 1 && !s->s.ready) {
            if (purr_px_sess_client_finish(&s->s, &L->store, p + 1, n - 1) != 0) {
                s->used = 0;
            }
        }
        return;
    }
    /* A message heard twice (the other end sent it again because our answer was lost): not given to the pairing again, our last answer is sent again instead. */
    if (L->pair_rx_n == n && n > 0 && same_id(L->pair_peer, h->id) && memcmp(L->pair_rx, p, (size_t)n) == 0) {
        if (L->pair_last_n > 0) {
            frame_out(L, addr_for(L, h->id, 0), PURR_PX_T_PAIR, 0, L->pair_last, L->pair_last_n);
        }
        return;
    }
    /* Pairing messages. A request from a stranger is judged by the policy first: nothing is answered unless somebody may be asked. */
    if (n >= 1 && p[0] == 1 && L->pair.state == PURR_PX_PAIR_IDLE) {
        int d = policy_for(L, h->id, t);
        if (d == PURR_PX_DENY) {
            return;
        }
        memcpy(L->pair_peer, h->id, 8);
        L->pair_auto = d == PURR_PX_AUTO_PAIR;
        L->pair_flags = L->pair_auto ? PURR_PX_PEER_AUTO : 0;
    } else if (!same_id(L->pair_peer, h->id) || L->pair.state == PURR_PX_PAIR_IDLE) {
        return;                                             /* not the device we are pairing with */
    }
    if (n <= (int)sizeof(L->pair_rx)) {
        memcpy(L->pair_rx, p, (size_t)n);
        L->pair_rx_n = n;
    }
    uint8_t out[PURR_PX_PAIR_MAX_MSG];
    int w = purr_px_pair_rx(&L->pair, t, p, n, out, (int)sizeof(out));
    if (w > 0) {
        pair_send(L, L->pair_peer, out, w);
    }
    if (L->pair.state == PURR_PX_PAIR_WAIT_USER && L->pair_auto && L->pair.user == 0) {
        w = purr_px_pair_user(&L->pair, 1, out, (int)sizeof(out));
        if (w > 0) {
            pair_send(L, L->pair_peer, out, w);
        }
    }
    pair_finish_if_done(L);
}

/* ---------------------------------------------------------------- receiving */

static void frame_in(purr_pxl_t *L, uint8_t via, const uint8_t *mac, uint32_t ip, uint16_t port, int rssi, const uint8_t *f, int n, uint32_t t)
{
    purr_px_hdr_t h;
    int hl = purr_px_hdr_get(f, n, &h);
    if (hl < 0 || same_id(h.id, L->id)) {
        return;
    }
    pxl_addr_t *a = addr_for(L, h.id, 1);
    if (a != NULL) {
        a->via = via;
        if (mac != NULL) {
            memcpy(a->mac, mac, 6);                         /* both are kept: a device heard both ways is reached the way it was heard last, and its address stays known */
        }
        if (via == PURR_LINK_VIA_UDP) {
            a->ip = ip;
            a->port = port;
        }
    }
    const uint8_t *p = f + hl;
    int pn = n - hl;
    if (h.type == PURR_PX_T_BEACON) {
        purr_px_beacon_t b;
        if (purr_px_beacon_get(p, pn, &b) > 0) {
            purr_px_table_seen(&L->table, h.id, rssi, t, &b);
        }
    } else if (h.type == PURR_PX_T_PAIR) {
        pair_in(L, &h, p, pn, t);
    } else if (h.type == PURR_PX_T_SECURE) {
        secure_in(L, &h, p, pn, t);
    }
}

/* ---------------------------------------------------------------- the ops */

static int l_tick(void *h, uint32_t unused)
{
    (void)unused;
    purr_pxl_t *L = h;
    uint32_t t = now_ms();
    uint8_t buf[PURR_PX_MAX_FRAME + 8];
    for (int i = 0; i < 12; i++) {                          /* a bounded amount per call: a flood cannot starve the caller */
        uint8_t mac[6];
        int8_t rssi = -127;
        int n = s_io->radio_recv(mac, &rssi, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        frame_in(L, PURR_LINK_VIA_RADIO, mac, 0, 0, rssi, buf, n, t);
    }
    if (L->udp > 0) {
        for (int i = 0; i < 12; i++) {
            uint32_t ip = 0;
            uint16_t port = 0;
            int n = s_io->udp_recv(L->udp, &ip, &port, buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            frame_in(L, PURR_LINK_VIA_UDP, NULL, ip, port, -70, buf, n, t);      /* the network says nothing of distance: never "close" */
        }
    }
    if (t - L->last_beacon_ms >= BEACON_MS) {
        L->last_beacon_ms = t;
        send_beacon(L);
        purr_px_table_prune(&L->table, t, STALE_MS);
    }
    if (L->pair_last_n > 0 && t - L->pair_last_ms >= 500 && L->pair_resends < (L->pair.state != PURR_PX_PAIR_IDLE ? 40 : 4)) {
        frame_out(L, addr_for(L, L->pair_peer, 0), PURR_PX_T_PAIR, 0, L->pair_last, L->pair_last_n);
        L->pair_last_ms = t;
        L->pair_resends++;
    }
    if (L->pair.state != PURR_PX_PAIR_IDLE) {
        purr_px_pair_tick(&L->pair, t);
        if (L->pair.state == PURR_PX_PAIR_FAILED) {
            pair_reset(L);
        }
    }
    for (int i = 0; i < PURR_PXL_SESSIONS; i++) {
        purr_px_reasm_expire(&L->sess[i].reasm, t, 3000);
    }
    if (L->dirty) {
        blob_save(L);
    }
    return 0;
}

static int l_self(void *h, uint8_t id[8], char name[17])
{
    purr_pxl_t *L = h;
    memcpy(id, L->id, 8);
    copy_name(name, 17, L->name);
    return 0;
}

static void fill_peer(purr_pxl_t *L, purr_link_peer_t *o, const uint8_t id[8], const purr_px_peer_t *np, const purr_px_peer_rec_t *rec, uint32_t t)
{
    memset(o, 0, sizeof(*o));
    memcpy(o->id, id, 8);
    o->rssi = -127;
    o->paired = rec != NULL;
    if (rec != NULL) {
        copy_name(o->name, sizeof(o->name), rec->name);
    }
    if (np != NULL) {
        copy_name(o->name, sizeof(o->name), np->beacon.name);
        o->roles = np->beacon.roles;
        o->klass = np->beacon.klass;
        o->rssi = np->rssi;
        o->online = (t - np->last_seen_ms) < ONLINE_MS;
        o->close = purr_px_peer_is_close(&L->table, id, t) ? 1 : 0;
        pxl_addr_t *a = addr_for(L, id, 0);
        o->via = a != NULL ? a->via : 0;
        o->ip = a != NULL ? a->ip : 0;
        o->caps_lo = (uint8_t)np->beacon.caps;
    }
    pxl_sess_t *s = sess_find(L, id);
    o->session = s != NULL && s->s.ready;
}

static int l_nearby(void *h, purr_link_peer_t *out, int max)
{
    purr_pxl_t *L = h;
    uint32_t t = now_ms();
    int n = 0;
    for (int i = 0; i < PURR_PX_PEERS; i++) {
        const purr_px_peer_t *p = &L->table.p[i];
        if (!p->used) {
            continue;
        }
        if (n < max) {
            fill_peer(L, &out[n], p->id, p, purr_px_store_find_peer(&L->store, p->id), t);
        }
        n++;
    }
    return n;
}

static int l_paired(void *h, purr_link_peer_t *out, int max)
{
    purr_pxl_t *L = h;
    uint32_t t = now_ms();
    int n = 0;
    for (int i = 0; i < PURR_PX_MAX_PEERS; i++) {
        const purr_px_peer_rec_t *r = &L->store.peers[i];
        if (!r->used) {
            continue;
        }
        if (n < max) {
            fill_peer(L, &out[n], r->id, purr_px_table_find(&L->table, r->id), r, t);
        }
        n++;
    }
    return n;
}

static int l_pair(void *h, const uint8_t id[8])
{
    purr_pxl_t *L = h;
    if (L->pair.state != PURR_PX_PAIR_IDLE || addr_for(L, id, 0) == NULL) {
        return -1;
    }
    pair_reset(L);
    L->pair_rx_n = L->pair_last_n = 0;
    memcpy(L->pair_peer, id, 8);
    uint8_t out[PURR_PX_PAIR_MAX_MSG];
    int w = purr_px_pair_start(&L->pair, now_ms(), out, (int)sizeof(out));
    if (w <= 0) {
        return -1;
    }
    pair_send(L, id, out, w);
    return 0;
}

static int l_prompt(void *h, purr_link_prompt_t *o)
{
    purr_pxl_t *L = h;
    if (L->pair.state != PURR_PX_PAIR_WAIT_USER || L->pair_auto || L->pair.user != 0) {
        return 0;
    }
    memset(o, 0, sizeof(*o));
    o->kind = PURR_LINK_PROMPT_PAIR;
    memcpy(o->id, L->pair_peer, 8);
    copy_name(o->name, sizeof(o->name), L->pair.peer_name);
    o->code = purr_px_pair_code(&L->pair);
    return 1;
}

static int l_answer(void *h, int yes)
{
    purr_pxl_t *L = h;
    if (L->pair.state != PURR_PX_PAIR_WAIT_USER) {
        return -1;
    }
    uint8_t out[PURR_PX_PAIR_MAX_MSG];
    int w = purr_px_pair_user(&L->pair, yes ? 1 : 0, out, (int)sizeof(out));
    if (w > 0) {
        pair_send(L, L->pair_peer, out, w);
    }
    pair_finish_if_done(L);
    if (L->pair.state == PURR_PX_PAIR_FAILED) {
        pair_reset(L);
    }
    return 0;
}

static int l_forget(void *h, const uint8_t id[8])
{
    purr_pxl_t *L = h;
    pxl_sess_t *s = sess_find(L, id);
    if (s != NULL) {
        s->used = 0;
    }
    int r = purr_px_store_forget_peer(&L->store, id);
    if (r) {
        L->dirty = 1;
    }
    return r;
}

static int l_notice(void *h, char line[64])
{
    purr_pxl_t *L = h;
    const purr_px_net_rec_t *n = purr_px_store_next_notice(&L->store);
    if (n == NULL) {
        return 0;
    }
    const purr_px_peer_rec_t *from = purr_px_store_find_peer(&L->store, n->from);
    const char *a = "A paired device added the network ";
    size_t i = 0;
    for (; a[i] != '\0' && i < 40; i++) {
        line[i] = a[i];
    }
    for (size_t j = 0; n->ssid[j] != '\0' && i < 62; j++) {
        line[i++] = n->ssid[j];
    }
    line[i] = '\0';
    (void)from;
    L->dirty = 1;
    return 1;
}

static int l_nets(void *h, char ssid[][33], int max)
{
    purr_pxl_t *L = h;
    int n = 0;
    for (int i = 0; i < PURR_PX_MAX_NETS; i++) {
        if (L->store.nets[i].used && L->store.nets[i].by_peer) {
            if (n < max) {
                copy_name(ssid[n], 33, L->store.nets[i].ssid);
            }
            n++;
        }
    }
    return n;
}

static int l_forget_net(void *h, const char *ssid)
{
    purr_pxl_t *L = h;
    int r = purr_px_store_forget_net(&L->store, ssid);
    if (r) {
        L->dirty = 1;
    }
    return r;
}

static int l_option(void *h, int opt, int value)
{
    purr_pxl_t *L = h;
    if (opt == PURR_LINK_OPT_AUTO_ANY_NET) {
        int old = L->store.allow_any_network;
        if (value != -1000) {
            L->store.allow_any_network = value ? 1 : 0;
            L->dirty = 1;
        }
        return value == -1000 ? old : L->store.allow_any_network;
    }
    if (opt == PURR_LINK_OPT_PAIR_WINDOW) {
        uint32_t t = now_ms();
        if (value != -1000) {
            L->pair_window_until = value > 0 ? t + (uint32_t)(value > 300 ? 300 : value) * 1000u : 0;
        }
        return L->pair_window_until != 0 && (int32_t)(L->pair_window_until - t) > 0 ? (int)((L->pair_window_until - t) / 1000u) + 1 : 0;
    }
    if (opt == PURR_LINK_OPT_CLOSE_DBM) {
        int old = L->table.close_dbm;
        if (value != -1000 && value >= -90 && value <= -20) {
            purr_px_table_set_close(&L->table, value, L->table.close_hold_ms);
        }
        return value == -1000 ? old : L->table.close_dbm;
    }
    return -1;
}

static int l_connect(void *h, const uint8_t id[8])
{
    purr_pxl_t *L = h;
    if (!purr_px_store_find_peer(&L->store, id) || addr_for(L, id, 0) == NULL) {
        return -1;
    }
    pxl_sess_t *s = sess_new(L, id);
    uint8_t out[PURR_PX_SESS_HELLO1 + 1];
    int w = purr_px_sess_client_start(&s->s, id, out + 1, (int)sizeof(out) - 1);
    if (w < 0) {
        s->used = 0;
        return -1;
    }
    out[0] = HELLO1_TAG;
    frame_out(L, addr_for(L, id, 0), PURR_PX_T_PAIR, 0, out, w + 1);
    return 0;
}

static int l_send(void *h, const uint8_t id[8], uint8_t channel, const uint8_t *data, size_t len)
{
    purr_pxl_t *L = h;
    pxl_sess_t *s = sess_find(L, id);
    if (s == NULL || !s->s.ready || len > PURR_PX_MSG_MAX - PURR_PX_SESS_OVERHEAD) {
        return -1;
    }
    sess_send(L, s, channel, data, (int)len);
    return 0;
}

static int l_recv(void *h, const uint8_t id[8], uint8_t *channel, uint8_t *buf, size_t cap)
{
    purr_pxl_t *L = h;
    pxl_sess_t *s = sess_find(L, id);
    if (s == NULL) {
        return -1;
    }
    if (s->in_len == 0) {
        return 0;
    }
    int n = s->in_len < 0 ? 0 : s->in_len;
    if ((size_t)n > cap) {
        s->in_len = 0;
        return -1;
    }
    memcpy(buf, s->in, (size_t)n);
    *channel = s->in_ch;
    s->in_len = 0;
    return n;
}

static int l_share_wifi(void *h, const uint8_t id[8], const char *ssid)
{
    purr_pxl_t *L = h;
    pxl_sess_t *s = sess_find(L, id);
    if (s == NULL || !s->s.ready) {
        return -1;
    }
    char pass[65];
    if (s_io->net_saved_get == NULL || s_io->net_saved_get(ssid, pass) != 0) {
        return -1;
    }
    uint8_t m[2 + 32 + 1 + 64];
    int sl = 0, pl = 0;
    while (ssid[sl] != '\0' && sl < 32) sl++;
    while (pass[pl] != '\0' && pl < 64) pl++;
    m[0] = CTL_WIFI_OFFER;
    m[1] = (uint8_t)sl;
    memcpy(m + 2, ssid, (size_t)sl);
    m[2 + sl] = (uint8_t)pl;
    memcpy(m + 3 + sl, pass, (size_t)pl);
    sess_send(L, s, PURR_PX_CH_CONTROL, m, 3 + sl + pl);
    memset(pass, 0, sizeof(pass));
    memset(m, 0, sizeof(m));
    return 0;
}

/* the stream functions are defined below with the context type; the ops table takes the handle as void * */
static int o_stream_accept(void *h, const uint8_t *a, size_t n, uint8_t *b, size_t cap, uint8_t id[8]);
static int o_stream_start(void *h, const uint8_t id[8], uint8_t *hello1, size_t cap);
static int o_stream_finish(void *h, int stream, const uint8_t *hello2, size_t n);
static int o_stream_seal(void *h, int stream, uint8_t ch, const uint8_t *pt, size_t n, uint8_t *out, size_t cap);
static int o_stream_open(void *h, int stream, const uint8_t *rec, size_t n, uint8_t *ch, uint8_t *pt, size_t cap);
static void o_stream_close(void *h, int stream);
static int o_has_peers(void *h);

const purr_link_ops_t purr_pxl_ops = {
    .version = 2,
    .tick = l_tick, .self = l_self, .nearby = l_nearby, .paired = l_paired, .pair = l_pair,
    .prompt = l_prompt, .answer = l_answer, .forget = l_forget, .notice = l_notice,
    .nets = l_nets, .forget_net = l_forget_net, .option = l_option, .connect = l_connect,
    .send = l_send, .recv = l_recv, .share_wifi = l_share_wifi,
    .stream_accept = o_stream_accept, .stream_start = o_stream_start, .stream_finish = o_stream_finish, .stream_seal = o_stream_seal,
    .stream_open = o_stream_open, .stream_close = o_stream_close, .has_peers = o_has_peers,
};

/* ---------------------------------------------------------------- init */

int purr_pxl_init(purr_pxl_t *L, const purr_pxl_io_t *io)
{
    s_io = io;
    memset(L, 0, sizeof(*L));
    purr_px_store_init(&L->store);
    purr_px_table_init(&L->table);
    if (!blob_load(L)) {
        if (!s_io->ecc->keypair(L->pub, L->priv)) {
            return -1;
        }
        purr_px_id_from_pub(L->pub, L->id);
        L->dirty = 1;
    }
    char nm[16];
    const char *pre = s_io->name_prefix != NULL ? s_io->name_prefix : "PURR";
    size_t k = 0;
    for (; pre[k] != '\0' && k < 8; k++) {
        nm[k] = pre[k];
    }
    nm[k++] = '-';
    nm[k++] = HEX[L->id[0] >> 4];
    nm[k++] = HEX[L->id[0] & 15];
    nm[k++] = HEX[L->id[1] >> 4];
    nm[k++] = HEX[L->id[1] & 15];
    nm[k] = '\0';
    copy_name(L->name, sizeof(L->name), nm);
    L->udp = s_io->udp_open ? s_io->udp_open(UDP_PORT) : -1;
    L->boot_ms = now_ms();
    pair_reset(L);
    L->in_use = 1;
    if (L->dirty) {
        blob_save(L);
    }
    return 0;
}

void purr_pxl_shutdown(purr_pxl_t *L)
{
    if (L->in_use) {
        if (L->udp > 0 && s_io->udp_close) {
            s_io->udp_close(L->udp);
        }
        L->in_use = 0;
    }
}

/* ---------------------------------------------------------------- streams */

static int stream_slot(purr_pxl_t *L)
{
    for (int i = 0; i < PURR_PXL_STREAMS; i++) {
        if (!L->stream_used[i]) {
            return i;
        }
    }
    return -1;
}

int purr_pxl_stream_accept(purr_pxl_t *L, const uint8_t *hello1, size_t n, uint8_t *hello2, size_t cap, uint8_t peer_id[8])
{
    int i = stream_slot(L);
    if (i < 0 || n != PURR_PX_SESS_HELLO1 || cap < PURR_PX_SESS_HELLO2) {
        return -1;
    }
    purr_px_sess_init(&L->stream[i], s_io->ecc, L->pub, L->priv);
    if (purr_px_sess_server_accept(&L->stream[i], &L->store, hello1, (int)n, hello2, (int)cap) < 0) {
        return -1;
    }
    memcpy(peer_id, L->stream[i].peer_id, 8);
    L->stream_used[i] = 1;
    return i + 1;
}

int purr_pxl_stream_start(purr_pxl_t *L, const uint8_t server_id[8], uint8_t *hello1, size_t cap)
{
    int i = stream_slot(L);
    if (i < 0 || !purr_px_store_find_peer(&L->store, server_id) || cap < PURR_PX_SESS_HELLO1) {
        return -1;
    }
    purr_px_sess_init(&L->stream[i], s_io->ecc, L->pub, L->priv);
    if (purr_px_sess_client_start(&L->stream[i], server_id, hello1, (int)cap) < 0) {
        return -1;
    }
    L->stream_used[i] = 1;
    return i + 1;
}

int purr_pxl_stream_finish(purr_pxl_t *L, int stream, const uint8_t *hello2, size_t n)
{
    if (stream < 1 || stream > PURR_PXL_STREAMS || !L->stream_used[stream - 1]) {
        return -1;
    }
    if (purr_px_sess_client_finish(&L->stream[stream - 1], &L->store, hello2, (int)n) != 0) {
        L->stream_used[stream - 1] = 0;
        return -1;
    }
    return 0;
}

int purr_pxl_stream_seal(purr_pxl_t *L, int stream, uint8_t channel, const uint8_t *pt, size_t n, uint8_t *out, size_t cap)
{
    if (stream < 1 || stream > PURR_PXL_STREAMS || !L->stream_used[stream - 1]) {
        return -1;
    }
    return purr_px_sess_seal(&L->stream[stream - 1], channel, 0, pt, (int)n, out, (int)cap);
}

int purr_pxl_stream_open(purr_pxl_t *L, int stream, const uint8_t *rec, size_t n, uint8_t *channel, uint8_t *pt, size_t cap)
{
    if (stream < 1 || stream > PURR_PXL_STREAMS || !L->stream_used[stream - 1]) {
        return -1;
    }
    uint8_t fl;
    return purr_px_sess_open(&L->stream[stream - 1], rec, (int)n, channel, &fl, pt, (int)cap);
}

void purr_pxl_stream_close(purr_pxl_t *L, int stream)
{
    if (stream >= 1 && stream <= PURR_PXL_STREAMS) {
        memset(&L->stream[stream - 1], 0, sizeof(L->stream[0]));
        L->stream_used[stream - 1] = 0;
    }
}

int purr_pxl_has_peers(const purr_pxl_t *L)
{
    for (int i = 0; i < PURR_PX_MAX_PEERS; i++) {
        if (L->store.peers[i].used) {
            return 1;
        }
    }
    return 0;
}

static int o_stream_accept(void *h, const uint8_t *a, size_t n, uint8_t *b, size_t cap, uint8_t id[8]) { return purr_pxl_stream_accept(h, a, n, b, cap, id); }
static int o_stream_start(void *h, const uint8_t id[8], uint8_t *hello1, size_t cap) { return purr_pxl_stream_start(h, id, hello1, cap); }
static int o_stream_finish(void *h, int stream, const uint8_t *hello2, size_t n) { return purr_pxl_stream_finish(h, stream, hello2, n); }
static int o_stream_seal(void *h, int stream, uint8_t ch, const uint8_t *pt, size_t n, uint8_t *out, size_t cap) { return purr_pxl_stream_seal(h, stream, ch, pt, n, out, cap); }
static int o_stream_open(void *h, int stream, const uint8_t *rec, size_t n, uint8_t *ch, uint8_t *pt, size_t cap) { return purr_pxl_stream_open(h, stream, rec, n, ch, pt, cap); }
static void o_stream_close(void *h, int stream) { purr_pxl_stream_close(h, stream); }
static int o_has_peers(void *h) { return purr_pxl_has_peers(h); }
