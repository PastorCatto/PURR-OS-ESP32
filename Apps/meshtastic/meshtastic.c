/*
 * meshtastic.c - the Meshtastic messaging service (plan9.md, "Meshtastic service"). A background app (with screens of its own: nodes, channels, a join link, settings): it takes the radio lease, speaks the mesh with the
 * protocol core (Modules/meshtastic), and carries messages to and from the Messages app through the `messaging` catcall. No libc (Modules/proximity/px_libc.c has the few
 * functions the core needs).
 *
 * Threads in Messages: a channel is the peer "#LongFast" (everyone on it sees every message, so the text starts with who said it), a person is the peer "!0a1b2c3d" (a node
 * number). Sending to a person asks for an acknowledgement and tries again; the delivery state the person sees follows the mesh's.
 *
 * Settings live in this app's own folder (config.bin) and are drawn by Messages from the manifest's [[provides.messaging.setting]] list; they are applied at the next start of the
 * service (Messages restarts it when one changes).
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "purr_app_abi.h"
#include "purr_catcalls.h"
#include "../../Modules/meshtastic/purr_mt_node.h"
#include "../../Modules/meshtastic/purr_mt_pkc.h"

static const purr_cc_console_t *con;
static const purr_cc_sched_t *sc;
static const purr_cc_system_t *sys;
static const purr_cc_storage_t *stg;
static const purr_cc_messaging_t *msg;
static const purr_cc_radio_t *rad;
static const purr_cc_ui_t *ui;
static const purr_cc_app_t *app;
static const purr_cc_contacts_t *con_tab;

#define CONFIG_MAGIC 0x4D545332u        /* "MTS2": 2 = pass-on is off until the person turns it on (the town mesh is live) */
#define TICK_MS 20
#define RX_WAIT_MS 60
#define LEASE_RETRY_MS 5000u

typedef struct {
    uint32_t magic;
    char long_name[40];
    char short_name[5];
    char region[12];
    char preset[16];
    uint8_t hop_limit;
    uint8_t rebroadcast;
    int8_t tx_dbm;
    uint8_t pkc_send;                   /* send direct messages sealed with the public keys (experimental until checked against a real node) */
    uint8_t priv[32], pub[32];          /* this device's key pair for direct messages */
    struct {
        char name[12];
        uint8_t key[32];
        int8_t keylen;
        uint8_t role;
    } ch[PURR_MT_MAX_CHANNELS];
} config_t;

static config_t s_cfg;
static purr_mt_t s_node;
static int s_service = -1;
static int s_timer = -1;
static int s_leased;
static uint32_t s_lease_try_ms;
static uint32_t s_lease_not_before;         /* after handing the radio back: not asked for again before this time */
static struct { uint32_t pkt, msg; } s_sent[8];
static int s_sent_next;

static void say(const char *t)
{
    if (con != NULL) {
        con->print(t);
    }
}

static uint32_t now_ms(void) { return sc->uptime_ms(); }
static uint32_t rand_cb(void) { return rad->random32(); }

static const char HEX[] = "0123456789abcdef";

static void node_name(char *out, uint32_t num)           /* "!0a1b2c3d" */
{
    out[0] = '!';
    for (int i = 0; i < 8; i++) {
        out[1 + i] = HEX[(num >> (28 - 4 * i)) & 15];
    }
    out[9] = '\0';
}

static int parse_node(const char *s, uint32_t *num)
{
    if (s[0] != '!') {
        return -1;
    }
    uint32_t v = 0;
    for (int i = 1; i <= 8; i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1));
        if (d < 0) {
            return -1;
        }
        v = (v << 4) | (uint32_t)d;
    }
    *num = v;
    return s[9] == '\0' ? 0 : -1;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void copy(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < cap; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* ---------------------------------------------------------------- configuration */

static void defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.magic = CONFIG_MAGIC;
    uint32_t id = rad->device_id();
    char hexid[5] = {HEX[(id >> 12) & 15], HEX[(id >> 8) & 15], HEX[(id >> 4) & 15], HEX[id & 15], 0};
    copy(s_cfg.long_name, sizeof(s_cfg.long_name), "PURR ");
    copy(s_cfg.long_name + 5, sizeof(s_cfg.long_name) - 5, hexid);
    copy(s_cfg.short_name, sizeof(s_cfg.short_name), hexid);
    copy(s_cfg.region, sizeof(s_cfg.region), "US");
    copy(s_cfg.preset, sizeof(s_cfg.preset), "LongFast");
    s_cfg.hop_limit = 3;
    s_cfg.rebroadcast = 0;                                              /* a new node listens first; passing other nodes' packets on is a Settings choice */
    s_cfg.tx_dbm = 22;
    uint8_t seed[32];
    for (int i = 0; i < 32; i += 4) {
        uint32_t r = rad->random32();
        seed[i] = (uint8_t)r;
        seed[i + 1] = (uint8_t)(r >> 8);
        seed[i + 2] = (uint8_t)(r >> 16);
        seed[i + 3] = (uint8_t)(r >> 24);
    }
    purr_mt_pkc_keypair(seed, s_cfg.priv, s_cfg.pub);
    copy(s_cfg.ch[0].name, sizeof(s_cfg.ch[0].name), "LongFast");
    s_cfg.ch[0].key[0] = 1;                                             /* the default key, index 1 ("AQ==") */
    s_cfg.ch[0].keylen = 1;
    s_cfg.ch[0].role = 1;
}

static void load_config(void)
{
    defaults();
    if (stg == NULL) {
        return;
    }
    int h = stg->open("config.bin", PURR_CC_O_READ);
    if (h < 0) {
        return;
    }
    config_t c;
    int n = stg->read(h, &c, sizeof(c));
    stg->close(h);
    if (n == (int)sizeof(c) && c.magic == CONFIG_MAGIC && purr_mt_region_by_name(c.region) != NULL && purr_mt_preset_by_name(c.preset) != NULL) {
        s_cfg = c;
    }
}

static void save_config(void)
{
    if (stg == NULL) {
        return;
    }
    int h = stg->open("config.bin", PURR_CC_O_WRITE | PURR_CC_O_CREATE | PURR_CC_O_TRUNC);
    if (h >= 0) {
        stg->write(h, &s_cfg, sizeof(s_cfg));
        stg->close(h);
    }
}

static purr_mt_nodeinfo_t s_nodes_keep[PURR_MT_MAX_NODES];
static int s_node_valid;

static void start_node(void)
{
    purr_mt_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.my_num = rad->device_id();
    if (c.my_num < 4 || c.my_num == PURR_MT_BROADCAST) {
        c.my_num += 4;                                                  /* the low numbers and broadcast are reserved */
    }
    copy(c.long_name, sizeof(c.long_name), s_cfg.long_name);
    copy(c.short_name, sizeof(c.short_name), s_cfg.short_name);
    c.hop_limit = s_cfg.hop_limit;
    c.rebroadcast = s_cfg.rebroadcast;
    c.region = purr_mt_region_by_name(s_cfg.region);
    c.preset = purr_mt_preset_by_name(s_cfg.preset);
    c.rand32 = rand_cb;
    memcpy(c.priv, s_cfg.priv, 32);
    memcpy(c.pub, s_cfg.pub, 32);
    c.has_pkc = 1;
    c.pkc_send = s_cfg.pkc_send;
    if (s_node_valid) {
        memcpy(s_nodes_keep, s_node.nodes, sizeof(s_nodes_keep));        /* a setting changed: the nodes already heard stay in the list */
    }
    purr_mt_node_init(&s_node, &c, now_ms());
    if (s_node_valid) {
        memcpy(s_node.nodes, s_nodes_keep, sizeof(s_nodes_keep));
    }
    s_node_valid = 1;
    for (int i = 0; i < PURR_MT_MAX_CHANNELS; i++) {
        if (s_cfg.ch[i].role != 0) {
            purr_mt_channel_set(&s_node, i, s_cfg.ch[i].name, s_cfg.ch[i].key, s_cfg.ch[i].keylen, s_cfg.ch[i].role);
        }
    }
}

/* Joining a group from a share link (https://meshtastic.org/e/#...): its channels are added next to the ones we have (a channel with a name we already have takes the new key), and
 * the region and preset are taken if the link names them. Returns how many channels it added or changed, or -1 for a link that cannot be read. */
static int apply_url(const char *url)
{
    purr_mt_url_t u;
    if (purr_mt_url_parse(url, &u) != 0) {
        return -1;
    }
    int changed = 0;
    for (int i = 0; i < u.count; i++) {
        const char *name = u.ch[i].name[0] != '\0' ? u.ch[i].name : (u.preset[0] != '\0' ? u.preset : s_cfg.preset);
        int slot = -1;
        for (int k = 0; k < PURR_MT_MAX_CHANNELS; k++) {
            if (s_cfg.ch[k].role != 0 && streq(s_cfg.ch[k].name, name)) {
                slot = k;
                break;
            }
        }
        if (slot < 0) {
            for (int k = 0; k < PURR_MT_MAX_CHANNELS; k++) {
                if (s_cfg.ch[k].role == 0) {
                    slot = k;
                    break;
                }
            }
        }
        if (slot < 0) {
            continue;                                                   /* all eight are in use */
        }
        uint8_t keep_role = s_cfg.ch[slot].role == 1 ? 1 : 2;
        memset(&s_cfg.ch[slot], 0, sizeof(s_cfg.ch[slot]));
        copy(s_cfg.ch[slot].name, sizeof(s_cfg.ch[slot].name), name);
        memcpy(s_cfg.ch[slot].key, u.ch[i].psk, (size_t)u.ch[i].psk_len);
        s_cfg.ch[slot].keylen = (int8_t)u.ch[i].psk_len;
        s_cfg.ch[slot].role = keep_role;
        changed++;
    }
    if (u.region[0] != '\0') {
        copy(s_cfg.region, sizeof(s_cfg.region), u.region);
    }
    if (u.preset[0] != '\0' && purr_mt_preset_by_name(u.preset) != NULL) {
        copy(s_cfg.preset, sizeof(s_cfg.preset), u.preset);
    }
    return changed;
}

/* A link left in this app's folder (join.txt) is applied at the start and removed; our own channels are written to link.txt for sharing. Until the app has a screen of its own this is
 * how a group is joined and shared. */
static void join_and_share(void)
{
    if (stg == NULL) {
        return;
    }
    int h = stg->open("join.txt", PURR_CC_O_READ);
    if (h >= 0) {
        char url[400];
        int n = stg->read(h, url, sizeof(url) - 1);
        stg->close(h);
        if (n > 0) {
            url[n] = '\0';
            while (n > 0 && (url[n - 1] == '\n' || url[n - 1] == '\r' || url[n - 1] == ' ')) {
                url[--n] = '\0';
            }
            int c = apply_url(url);
            say(c >= 0 ? "meshtastic: joined from join.txt\n" : "meshtastic: join.txt is not a Meshtastic link\n");
            if (c >= 0) {
                save_config();
            }
        }
        stg->remove("join.txt");
    }
    purr_mt_url_t u;
    memset(&u, 0, sizeof(u));
    for (int i = 0; i < PURR_MT_MAX_CHANNELS; i++) {
        if (s_cfg.ch[i].role != 0 && u.count < 8) {
            copy(u.ch[u.count].name, sizeof(u.ch[0].name), s_cfg.ch[i].name);
            memcpy(u.ch[u.count].psk, s_cfg.ch[i].key, (size_t)s_cfg.ch[i].keylen);
            u.ch[u.count].psk_len = s_cfg.ch[i].keylen;
            u.count++;
        }
    }
    copy(u.region, sizeof(u.region), s_cfg.region);
    copy(u.preset, sizeof(u.preset), s_cfg.preset);
    char link[300];
    int ln = purr_mt_url_make(&u, link, sizeof(link));
    int w = ln > 0 ? stg->open("link.txt", PURR_CC_O_WRITE | PURR_CC_O_CREATE | PURR_CC_O_TRUNC) : -1;
    if (w >= 0) {
        stg->write(w, link, (size_t)ln);
        stg->close(w);
    }
}

/* the modem settings of the primary channel on the chosen region and preset */
static void radio_cfg(purr_cc_radio_cfg_t *r)
{
    const purr_mt_region_t *reg = s_node.cfg.region;
    const purr_mt_preset_t *pre = s_node.cfg.preset;
    const char *name = s_cfg.ch[0].name;
    for (int i = 0; i < PURR_MT_MAX_CHANNELS; i++) {
        if (s_cfg.ch[i].role == 1) {
            name = s_cfg.ch[i].name;
        }
    }
    int slot = purr_mt_slot(reg, pre, name, 0);
    memset(r, 0, sizeof(*r));
    r->freq_hz = purr_mt_freq_hz(reg, pre, slot);
    r->bw_hz = pre->bw_hz;
    r->sf = pre->sf;
    r->cr = pre->cr;
    r->tx_dbm = s_cfg.tx_dbm < reg->max_dbm ? s_cfg.tx_dbm : reg->max_dbm;
    r->crc_on = 1;
    r->preamble = pre->preamble;
    r->sync_word = PURR_MT_SX126X_SYNC;
}

static void try_lease(void)
{
    purr_cc_radio_cfg_t r;
    radio_cfg(&r);
    int rc = rad->lease(&r);
    s_lease_try_ms = now_ms();
    if (rc == PURR_RADIO_OK) {
        s_leased = 1;
        msg->service_online(1);
        say("meshtastic: on the air\n");
    } else {
        s_leased = 0;
        msg->service_online(0);
    }
}

/* ---------------------------------------------------------------- Messages <-> the mesh */

static int channel_by_name(const char *name)
{
    for (int i = 0; i < PURR_MT_MAX_CHANNELS; i++) {
        if (s_node.cfg.channels[i].role != 0 && streq(s_node.cfg.channels[i].name, name)) {
            return i;
        }
    }
    return -1;
}

static void send_outgoing(void)
{
    purr_cc_msg_t m;
    char peer[40];
    int guard = 4;
    while (guard-- > 0 && msg->next_outgoing(&m, peer, sizeof(peer))) {
        uint32_t to = PURR_MT_BROADCAST;
        int ch = -1;
        int ok = 0;
        if (peer[0] == '#') {
            ch = channel_by_name(peer + 1);
            ok = ch >= 0;
        } else if (parse_node(peer, &to) == 0) {
            ch = -1;                                                    /* a direct message goes out on the primary channel */
            ok = 1;
        }
        uint32_t id = ok ? purr_mt_node_send_text(&s_node, now_ms(), to, ch, m.text) : 0;
        if (id == 0) {
            msg->delivery(m.id, PURR_MSGCC_FAILED);
            continue;
        }
        s_sent[s_sent_next].pkt = id;
        s_sent[s_sent_next].msg = m.id;
        s_sent_next = (s_sent_next + 1) % 8;
        msg->delivery(m.id, PURR_MSGCC_SENT);
    }
}

static uint32_t msg_of(uint32_t pkt)
{
    for (int i = 0; i < 8; i++) {
        if (s_sent[i].pkt == pkt && pkt != 0) {
            return s_sent[i].msg;
        }
    }
    return 0;
}

static void who(char *out, size_t cap, uint32_t num)                    /* a short name for a node: what it calls itself, else its number */
{
    const purr_mt_nodeinfo_t *n = purr_mt_node_find(&s_node, num);
    if (n != NULL && n->short_name[0] != '\0') {
        copy(out, cap, n->short_name);
    } else {
        char id[10];
        node_name(id, num);
        copy(out, cap, id);
    }
}

static void deliver_events(void)
{
    purr_mt_event_t e;
    while (purr_mt_node_event(&s_node, &e)) {
        if (e.kind == PURR_MT_EV_TEXT) {
            char peer[40], pname[24], text[PURR_MSGCC_TEXT];
            if (e.to == PURR_MT_BROADCAST) {
                copy(peer, sizeof(peer), "#");
                copy(peer + 1, sizeof(peer) - 1, s_node.cfg.channels[e.channel].name);
                copy(pname, sizeof(pname), s_node.cfg.channels[e.channel].name);
                char w[24];
                who(w, sizeof(w), e.from);
                size_t k = 0;
                for (; w[k] != '\0' && k < 20; k++) {
                    text[k] = w[k];
                }
                text[k++] = ':';
                text[k++] = ' ';
                size_t j = 0;
                for (; e.text[j] != '\0' && k + 1 < sizeof(text); j++) {
                    text[k++] = e.text[j];
                }
                text[k] = '\0';
            } else {
                node_name(peer, e.from);
                const purr_mt_nodeinfo_t *n = purr_mt_node_find(&s_node, e.from);
                copy(pname, sizeof(pname), n != NULL && n->long_name[0] != '\0' ? n->long_name : peer);
                copy(text, sizeof(text), e.text);
            }
            msg->incoming(peer, pname, text, NULL);
        } else if (e.kind == PURR_MT_EV_DELIVERED) {
            uint32_t m = msg_of(e.id);
            if (m != 0) {
                msg->delivery(m, PURR_MSGCC_DELIVERED);
            }
        } else if (e.kind == PURR_MT_EV_FAILED) {
            uint32_t m = msg_of(e.id);
            if (m != 0) {
                msg->delivery(m, PURR_MSGCC_FAILED);
            }
        }
    }
}


/* ---------------------------------------------------------------- the app's own screens */

enum { P_HOME, P_NODES, P_CHANNELS, P_SHARE, P_JOIN, P_SETTINGS, P_NAME, P_NODE };
static uint32_t s_node_sel;                         /* the node on the node page */
static int s_page;
static int s_sel;                                   /* the channel on the share page */
static int s_h_btn[8];                              /* handles of this page's buttons, in order */
static int s_h_in;                                  /* the page's text field */
static int s_h_row[PURR_MT_MAX_NODES];
static uint32_t s_row_node[PURR_MT_MAX_NODES];
static int s_rows;
static int settings_row[6];

static void utoa_(char *dst, uint32_t v)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0 && n < 11);
    for (int i = 0; i < n; i++) {
        dst[i] = tmp[n - 1 - i];
    }
    dst[n] = '\0';
}

static void cat(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(dst);
    copy(dst + n, cap - n, src);
}

static void cat_num(char *dst, size_t cap, uint32_t v)
{
    char t[12];
    utoa_(t, v);
    cat(dst, cap, t);
}

static void ago(char *dst, size_t cap, uint32_t ms)
{
    dst[0] = '\0';
    uint32_t s = ms / 1000u;
    if (s < 5) {
        cat(dst, cap, "now");
    } else if (s < 120) {
        cat_num(dst, cap, s);
        cat(dst, cap, " s ago");
    } else if (s < 7200) {
        cat_num(dst, cap, s / 60u);
        cat(dst, cap, " min ago");
    } else {
        cat_num(dst, cap, s / 3600u);
        cat(dst, cap, " h ago");
    }
}

static void restart_service(void)                                       /* after a setting changed: the node is built again and the radio asked for with the new modem settings */
{
    save_config();
    start_node();
    s_lease_not_before = 0;
    try_lease();
}

static void show_home(void);
static void show_nodes(void);
static void show_channels(void);
static void show_share(int ch);
static void show_join(void);
static void show_settings(void);
static void show_name(void);
static void show_node(uint32_t num);

static void page_begin(int page, const char *title, int app_handles_back)
{
    s_page = page;
    ui->clear();
    ui->win_back(app_handles_back);
    ui->win_title(title);
    ui->win_action(PURR_UI_ACT_NONE);
    memset(s_h_btn, 0, sizeof(s_h_btn));
    s_h_in = 0;
    s_rows = 0;
}

static void show_home(void)
{
    page_begin(P_HOME, "Meshtastic", 0);
    char line[80];
    if (s_leased) {
        purr_cc_radio_cfg_t r;
        radio_cfg(&r);
        copy(line, sizeof(line), "On the air: ");
        cat(line, sizeof(line), s_cfg.region);
        cat(line, sizeof(line), " ");
        cat(line, sizeof(line), s_cfg.preset);
        ui->label(line, PURR_UI_ROLE_TITLE);
        char f[24];
        utoa_(f, r.freq_hz / 1000000u);
        copy(line, sizeof(line), f);
        cat(line, sizeof(line), ".");
        char frac[12];
        utoa_(frac, (r.freq_hz / 1000u) % 1000u + 1000u);
        cat(line, sizeof(line), frac + 1);
        cat(line, sizeof(line), " MHz");
        ui->label(line, PURR_UI_ROLE_DIM);
    } else {
        ui->label("Waiting for the radio", PURR_UI_ROLE_TITLE);
        ui->label("Another app may have it, or the radio driver is not loaded.", PURR_UI_ROLE_DIM);
    }
    copy(line, sizeof(line), "You are ");
    cat(line, sizeof(line), s_cfg.long_name);
    ui->label(line, PURR_UI_ROLE_TEXT);
    copy(line, sizeof(line), "Nodes heard: ");
    cat_num(line, sizeof(line), (uint32_t)purr_mt_node_count(&s_node));
    ui->label(line, PURR_UI_ROLE_DIM);
    ui->spacer(6);
    s_h_btn[0] = ui->button("Nodes");
    s_h_btn[1] = ui->button("Channels");
    s_h_btn[2] = ui->button("Join a group");
    s_h_btn[3] = ui->button("Settings");
}

static void show_nodes(void)
{
    page_begin(P_NODES, "Nodes", 1);
    uint32_t now = now_ms();
    int order[PURR_MT_MAX_NODES], n = 0;
    int total = purr_mt_node_count(&s_node);
    for (int i = 0; i < total && n < PURR_MT_MAX_NODES; i++) {
        const purr_mt_nodeinfo_t *nd = purr_mt_node_at(&s_node, i);
        if (nd == NULL || nd->num == s_node.cfg.my_num) {
            continue;
        }
        int k = n++;
        while (k > 0 && purr_mt_node_at(&s_node, order[k - 1])->last_heard_ms < nd->last_heard_ms) {      /* newest heard first */
            order[k] = order[k - 1];
            k--;
        }
        order[k] = i;
    }
    if (n == 0) {
        ui->label("No other node heard yet.", PURR_UI_ROLE_TITLE);
        ui->label("Nodes show up here as they announce themselves, a few minutes after the radio starts.", PURR_UI_ROLE_DIM);
    }
    for (int i = 0; i < n; i++) {
        const purr_mt_nodeinfo_t *nd = purr_mt_node_at(&s_node, order[i]);
        char title[48], sub[64], id[12];
        node_name(id, nd->num);
        copy(title, sizeof(title), nd->long_name[0] != '\0' ? nd->long_name : id);
        copy(sub, sizeof(sub), nd->hops_away == 0 ? "direct" : "");
        if (nd->hops_away > 0) {
            cat_num(sub, sizeof(sub), nd->hops_away);
            cat(sub, sizeof(sub), nd->hops_away == 1 ? " hop" : " hops");
        }
        cat(sub, sizeof(sub), " · ");
        char a[24];
        ago(a, sizeof(a), now - nd->last_heard_ms);
        cat(sub, sizeof(sub), a);
        if (nd->has_pos) {
            cat(sub, sizeof(sub), " · position known");
        }
        s_h_row[i] = ui->row(title, sub);
        s_row_node[i] = nd->num;
    }
    s_rows = n;
}

static void fmt_deg(char *dst, size_t cap, int32_t e7)                 /* degrees with five decimals, from tenths of millionths */
{
    dst[0] = '\0';
    int64_t v = e7;
    if (v < 0) {
        cat(dst, cap, "-");
        v = -v;
    }
    cat_num(dst, cap, (uint32_t)(v / 10000000));
    cat(dst, cap, ".");
    char f[12];
    utoa_(f, (uint32_t)((v % 10000000) / 100) + 100000u);
    cat(dst, cap, f + 1);
}

static void show_node(uint32_t num)
{
    s_node_sel = num;
    const purr_mt_nodeinfo_t *nd = purr_mt_node_find(&s_node, num);
    char id[12];
    node_name(id, num);
    page_begin(P_NODE, nd != NULL && nd->short_name[0] != '\0' ? nd->short_name : id, 1);
    char line[96];
    if (nd != NULL) {
        ui->label(nd->long_name[0] != '\0' ? nd->long_name : id, PURR_UI_ROLE_TITLE);
        copy(line, sizeof(line), id);
        cat(line, sizeof(line), nd->hops_away == 0 ? " · heard directly" : " · through other nodes");
        ui->label(line, PURR_UI_ROLE_DIM);
        char a[24];
        ago(a, sizeof(a), now_ms() - nd->last_heard_ms);
        copy(line, sizeof(line), "Last heard ");
        cat(line, sizeof(line), a);
        ui->label(line, PURR_UI_ROLE_TEXT);
        if (nd->has_pos) {
            char la[20], lo[20];
            fmt_deg(la, sizeof(la), nd->lat_e7);
            fmt_deg(lo, sizeof(lo), nd->lon_e7);
            copy(line, sizeof(line), "Position ");
            cat(line, sizeof(line), la);
            cat(line, sizeof(line), ", ");
            cat(line, sizeof(line), lo);
            ui->label(line, PURR_UI_ROLE_TEXT);
        }
        ui->label(nd->has_key ? "Has a public key: direct messages can be sealed." : "No public key known yet.", PURR_UI_ROLE_DIM);
    }
    ui->spacer(6);
    s_h_btn[0] = ui->button("Message");
    int in = con_tab != NULL ? (int)con_tab->find("meshtastic", id) : 0;
    s_h_btn[1] = ui->button(in > 0 ? "In your contacts" : "Save to contacts");
}

static void show_channels(void)
{
    page_begin(P_CHANNELS, "Channels", 1);
    for (int i = 0; i < PURR_MT_MAX_CHANNELS; i++) {
        if (s_cfg.ch[i].role == 0) {
            continue;
        }
        char sub[48];
        copy(sub, sizeof(sub), s_cfg.ch[i].role == 1 ? "primary" : "secondary");
        cat(sub, sizeof(sub), s_cfg.ch[i].keylen == 0 ? " · not encrypted" : " · encrypted");
        s_h_row[i] = ui->row(s_cfg.ch[i].name, sub);
    }
    s_rows = PURR_MT_MAX_CHANNELS;
    ui->spacer(6);
    s_h_btn[0] = ui->button("Join a group");
}

static void show_share(int ch)
{
    s_sel = ch;
    page_begin(P_SHARE, s_cfg.ch[ch].name, 1);
    purr_mt_url_t u;
    memset(&u, 0, sizeof(u));
    copy(u.ch[0].name, sizeof(u.ch[0].name), s_cfg.ch[ch].name);
    memcpy(u.ch[0].psk, s_cfg.ch[ch].key, (size_t)s_cfg.ch[ch].keylen);
    u.ch[0].psk_len = s_cfg.ch[ch].keylen;
    u.count = 1;
    copy(u.region, sizeof(u.region), s_cfg.region);
    copy(u.preset, sizeof(u.preset), s_cfg.preset);
    char link[300];
    ui->label(s_cfg.ch[ch].role == 1 ? "The primary channel." : "A secondary channel.", PURR_UI_ROLE_TITLE);
    if (purr_mt_url_make(&u, link, sizeof(link)) > 0) {
        ui->label("Anyone with this link can read and write on the channel. Send it only to people you want in it.", PURR_UI_ROLE_DIM);
        ui->label(link, PURR_UI_ROLE_TEXT);
    }
    ui->spacer(6);
    if (s_cfg.ch[ch].role != 1) {
        s_h_btn[0] = ui->button("Leave this channel");
    }
}

static void show_join(void)
{
    page_begin(P_JOIN, "Join a group", 1);
    ui->label("Paste a Meshtastic channel link (https://meshtastic.org/e/#...).", PURR_UI_ROLE_DIM);
    s_h_in = ui->input_line(300, "https://meshtastic.org/e/#");
    s_h_btn[0] = ui->button("Join");
    ui->focus(s_h_in);
}

static const char *yes_no(int v) { return v ? "on" : "off"; }

static void show_settings(void)
{
    page_begin(P_SETTINGS, "Settings", 1);
    char t[48], sub[48];
    settings_row[0] = ui->row("Your name", s_cfg.long_name);
    copy(t, sizeof(t), "Region: ");
    cat(t, sizeof(t), s_cfg.region);
    settings_row[1] = ui->row(t, "tap for the next one");
    copy(t, sizeof(t), "Modem preset: ");
    cat(t, sizeof(t), s_cfg.preset);
    settings_row[2] = ui->row(t, "tap for the next one");
    copy(t, sizeof(t), "Hop limit: ");
    cat_num(t, sizeof(t), s_cfg.hop_limit);
    settings_row[3] = ui->row(t, "how far your messages travel");
    copy(t, sizeof(t), "Pass other nodes' messages on: ");
    cat(t, sizeof(t), yes_no(s_cfg.rebroadcast));
    settings_row[4] = ui->row(t, "turning it off makes the mesh weaker");
    copy(t, sizeof(t), "Sealed direct messages: ");
    cat(t, sizeof(t), yes_no(s_cfg.pkc_send));
    settings_row[5] = ui->row(t, "experimental; needs the other node's key");
    (void)sub;
}

static void show_name(void)
{
    page_begin(P_NAME, "Your name", 1);
    ui->label("The name other nodes show for you.", PURR_UI_ROLE_DIM);
    s_h_in = ui->input_line(36, "name");
    ui->set_text(s_h_in, s_cfg.long_name);
    s_h_btn[0] = ui->button("Save");
}

static void click(int h)
{
    switch (s_page) {
    case P_HOME:
        if (h == s_h_btn[0]) show_nodes();
        else if (h == s_h_btn[1]) show_channels();
        else if (h == s_h_btn[2]) show_join();
        else if (h == s_h_btn[3]) show_settings();
        break;
    case P_NODES:
        for (int i = 0; i < s_rows; i++) {
            if (h == s_h_row[i]) {
                show_node(s_row_node[i]);
                break;
            }
        }
        break;
    case P_NODE:
        if (h == s_h_btn[0]) {
            char peer[12];
            node_name(peer, s_node_sel);
            const purr_mt_nodeinfo_t *nd = purr_mt_node_find(&s_node, s_node_sel);
            int tid = msg->thread_for("meshtastic", peer, nd != NULL && nd->long_name[0] != '\0' ? nd->long_name : peer);
            if (tid > 0) {
                msg->open_thread((uint32_t)tid);
            }
        } else if (h == s_h_btn[1] && con_tab != NULL) {
            char peer[12];
            node_name(peer, s_node_sel);
            if (con_tab->find("meshtastic", peer) != 0) {
                ui->status("Already in your contacts", 0);
                break;
            }
            const purr_mt_nodeinfo_t *nd = purr_mt_node_find(&s_node, s_node_sel);
            int id = con_tab->add(nd != NULL && nd->long_name[0] != '\0' ? nd->long_name : peer);
            if (id > 0 && con_tab->set_addr((uint32_t)id, "meshtastic", peer) == 0) {
                ui->status("Saved to contacts", 0);
                show_node(s_node_sel);
            } else {
                ui->status("Could not save (allow contacts when asked)", 1);
            }
        }
        break;
    case P_CHANNELS:
        if (h == s_h_btn[0]) {
            show_join();
            break;
        }
        for (int i = 0; i < PURR_MT_MAX_CHANNELS; i++) {
            if (s_cfg.ch[i].role != 0 && h == s_h_row[i]) {
                show_share(i);
                break;
            }
        }
        break;
    case P_SHARE:
        if (h == s_h_btn[0] && s_cfg.ch[s_sel].role == 2) {                    /* leave: the channel is dropped, the primary stays */
            memset(&s_cfg.ch[s_sel], 0, sizeof(s_cfg.ch[s_sel]));
            restart_service();
            show_channels();
        }
        break;
    case P_JOIN:
        if (h == s_h_btn[0]) {
            char url[320];
            ui->get_text(s_h_in, url, sizeof(url));
            int c = apply_url(url);
            if (c >= 0) {
                restart_service();
                ui->status("Joined", 0);
                show_channels();
            } else {
                ui->status("That is not a Meshtastic link", 1);
            }
        }
        break;
    case P_SETTINGS:
        if (h == settings_row[0]) {
            show_name();
        } else if (h == settings_row[1]) {
            int n = purr_mt_region_count(), cur = 0;
            for (int i = 0; i < n; i++) {
                if (streq(purr_mt_region(i)->name, s_cfg.region)) {
                    cur = i;
                }
            }
            copy(s_cfg.region, sizeof(s_cfg.region), purr_mt_region((cur + 1) % n)->name);
            restart_service();
            show_settings();
        } else if (h == settings_row[2]) {
            int n = purr_mt_preset_count(), cur = 0;
            for (int i = 0; i < n; i++) {
                if (streq(purr_mt_preset(i)->name, s_cfg.preset)) {
                    cur = i;
                }
            }
            copy(s_cfg.preset, sizeof(s_cfg.preset), purr_mt_preset((cur + 1) % n)->name);
            restart_service();
            show_settings();
        } else if (h == settings_row[3]) {
            s_cfg.hop_limit = (uint8_t)(s_cfg.hop_limit >= 7 ? 1 : s_cfg.hop_limit + 1);
            restart_service();
            show_settings();
        } else if (h == settings_row[4]) {
            s_cfg.rebroadcast = (uint8_t)!s_cfg.rebroadcast;
            restart_service();
            show_settings();
        } else if (h == settings_row[5]) {
            s_cfg.pkc_send = (uint8_t)!s_cfg.pkc_send;
            restart_service();
            show_settings();
        }
        break;
    case P_NAME:
        if (h == s_h_btn[0]) {
            char nm[40];
            ui->get_text(s_h_in, nm, sizeof(nm));
            if (nm[0] != '\0') {
                copy(s_cfg.long_name, sizeof(s_cfg.long_name), nm);
                char sh[5];
                copy(sh, sizeof(sh), nm);
                copy(s_cfg.short_name, sizeof(s_cfg.short_name), sh);
                restart_service();
                show_settings();
            }
        }
        break;
    default:
        break;
    }
}

static void go_back(void)
{
    switch (s_page) {
    case P_NAME: show_settings(); break;
    case P_SHARE: show_channels(); break;
    case P_NODE: show_nodes(); break;
    case P_JOIN: show_channels(); break;
    case P_HOME: if (app != NULL) app->close_self(); break;
    default: show_home(); break;
    }
}

/* ---------------------------------------------------------------- the loop */

static uint32_t s_n_rx, s_n_rx_err, s_n_tx;                /* what the radio did: packets heard, receive errors, packets sent */
static int s_last_rssi;

static void report(void)                                                /* one line on the console every 20 s: whether the radio hears anything at all */
{
    static uint32_t last;
    uint32_t t = now_ms();
    if (t - last < 20000u) {
        return;
    }
    last = t;
    char line[120];
    copy(line, sizeof(line), "meshtastic: heard ");
    cat_num(line, sizeof(line), s_n_rx);
    cat(line, sizeof(line), ", receive errors ");
    cat_num(line, sizeof(line), s_n_rx_err);
    cat(line, sizeof(line), ", sent ");
    cat_num(line, sizeof(line), s_n_tx);
    cat(line, sizeof(line), ", nodes ");
    cat_num(line, sizeof(line), (uint32_t)purr_mt_node_count(&s_node));
    cat(line, sizeof(line), s_leased ? ", radio held" : ", radio NOT held");
    cat(line, sizeof(line), "\n");
    say(line);
}

static void pump(void)
{
    uint32_t t = now_ms();
    report();
    if (!s_leased) {
        if ((int32_t)(t - s_lease_not_before) >= 0 && t - s_lease_try_ms >= LEASE_RETRY_MS) {
            try_lease();
        }
        return;
    }
    send_outgoing();
    uint8_t buf[255];
    purr_cc_radio_rx_t info;
    int n = rad->receive(buf, sizeof(buf), &info, RX_WAIT_MS);
    if (n > 0) {
        s_n_rx++;
        s_last_rssi = info.rssi_dbm;
        purr_mt_node_rx(&s_node, now_ms(), buf, (size_t)n, info.rssi_dbm, info.snr_db);
    } else if (n < 0 && n != PURR_RADIO_LOST) {
        s_n_rx_err++;
    } else if (n == PURR_RADIO_LOST) {
        s_leased = 0;
        msg->service_online(0);
        return;
    }
    uint8_t out[255];
    size_t ol = 0;
    if (purr_mt_node_tick(&s_node, now_ms(), out, sizeof(out), &ol)) {
        s_n_tx++;
        if (rad->send(out, ol, 4000) == PURR_RADIO_LOST) {
            s_leased = 0;
            msg->service_online(0);
        }
    }
    deliver_events();
    static uint32_t last_count_ms;
    static int last_count = -1;
    if (ui != NULL && (s_page == P_HOME || s_page == P_NODES) && t - last_count_ms >= 5000u) {      /* a screen that shows how many nodes are known keeps up with it */
        last_count_ms = t;
        int c = purr_mt_node_count(&s_node);
        if (c != last_count) {
            last_count = c;
            if (s_page == P_HOME) {
                show_home();
            } else {
                show_nodes();
            }
        }
    }
}

static int on_create(const purr_app_env_t *env, const void *state, uint32_t len)
{
    (void)state;
    (void)len;
    if (env == NULL || env->catcall_get == NULL) {
        return -1;
    }
    con = (const purr_cc_console_t *)env->catcall_get("console", 1);
    sc = (const purr_cc_sched_t *)env->catcall_get("sched", 1);
    sys = (const purr_cc_system_t *)env->catcall_get("system", 2);
    stg = (const purr_cc_storage_t *)env->catcall_get("storage", 1);
    msg = (const purr_cc_messaging_t *)env->catcall_get("messaging", 1);
    rad = (const purr_cc_radio_t *)env->catcall_get("radio", 1);
    ui = (const purr_cc_ui_t *)env->catcall_get("ui", 2);
    app = (const purr_cc_app_t *)env->catcall_get("app", 1);
    con_tab = (const purr_cc_contacts_t *)env->catcall_get("contacts", 1);
    (void)sys;
    if (sc == NULL || msg == NULL || rad == NULL) {
        say("meshtastic: needs the sched, messaging and radio catcalls\n");
        return -1;
    }
    if (rad->count() == 0) {
        say("meshtastic: this device has no LoRa radio\n");
        return -1;
    }
    s_lease_not_before = 0;
    s_leased = 0;
    load_config();
    join_and_share();
    save_config();                                                      /* the first start writes the defaults, so Messages' settings have a file to change */
    start_node();
    s_service = msg->service_register("meshtastic", "Meshtastic", "MTS", PURR_MT_TEXT_MAX, PURR_MSGCC_IMG_NONE, 0, 1);
    if (s_service < 0) {
        say("meshtastic: the messaging service was not accepted (permission?)\n");
    }
    try_lease();
    s_timer = sc->timer_start(TICK_MS, 1);
    if (ui != NULL) {
        show_home();
    }
    return 0;
}

static void on_event(const purr_app_event_t *ev)
{
    if (ev->type == PURR_APPEV_TIMER && (int)ev->arg == s_timer) {
        pump();
    } else if (ev->type == PURR_APPEV_UI && ev->name != NULL && ui != NULL) {
        if (streq(ev->name, "click")) {
            click((int)ev->arg);
        } else if (streq(ev->name, "back")) {
            go_back();
        }
    } else if (ev->type == PURR_APPEV_USER && ev->name != NULL && streq(ev->name, "radio.yield")) {
        rad->release();                                                 /* the user switched services: hand the radio back */
        s_leased = 0;
        msg->service_online(0);
        s_lease_not_before = now_ms() + 60000u;                         /* and do not grab it back at once */
    } else if (ev->type == PURR_APPEV_PERM) {
        s_lease_try_ms = 0;                                             /* an answer to a prompt: try again now */
        s_lease_not_before = 0;
        if (s_service < 0) {
            s_service = msg->service_register("meshtastic", "Meshtastic", "MTS", PURR_MT_TEXT_MAX, PURR_MSGCC_IMG_NONE, 0, 1);
        }
    }
}

static void on_destroy(int reason)
{
    (void)reason;
    if (rad != NULL) {
        rad->release();
    }
    if (msg != NULL) {
        msg->service_online(0);
    }
}

static const purr_app_table2_t s_table = {
    .base = {.abi_version = PURR_APP_ABI_VERSION_2, .on_create = on_create, .on_destroy = on_destroy},
    .on_event = on_event,
    .memory_needed = 65536, .kind = 2 /* ui */, .flags = 0x05 /* background, persistent */,
};

__attribute__((used))
const purr_app_table_t *meshtastic_entry(void)
{
    return &s_table.base;
}
